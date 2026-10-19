/*
 * libretro glue for the statically linked cores (mGBA and gpSP, or the
 * stub core). The core is picked per game (rom_settings.core); all calls
 * go through its struct core_api (cores.c), and only one core is
 * initialized at a time: core_unload() deinits it fully, so the next game
 * can use the other one.
 *
 * Saves: neither core writes save files itself (no use of the save
 * directory). Both keep battery saves (SRAM, flash, EEPROM) in a 128KiB
 * buffer exposed as RETRO_MEMORY_SAVE_RAM, prefilled with 0xFF. mGBA reads
 * it into the emulated cartridge on the first retro_run, gpSP emulates
 * straight out of it. So the frontend copies <data>/saves/<rom>.sav (by
 * file name only, whatever folder of roms/ the ROM is in, see settings.c)
 * into that buffer after retro_load_game and before the first retro_run,
 * and writes it back when it changed. The layout in the buffer is the same
 * for both (raw SRAM, flash banks in order, EEPROM 8 byte blocks with the
 * first bit in bit 7), so one .sav works with either core. What gets
 * written is the real size: mGBA's retro_get_memory_size() reports it once
 * the save type is detected, for gpSP (always 128KiB) cores.c works it out
 * from the detected type. Without a .sav, a RetroArch <rom>.srm (same raw
 * format) is loaded instead; it is left alone and saves go to the .sav.
 * Neither core keeps RTC state in the save: both read the system clock.
 *
 * Writes go through write_file_atomic() in a forked child, so a slow card
 * never stalls emulation; the child sees a consistent snapshot of the
 * buffer thanks to copy on write.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audio.h"
#include "core.h"
#include "core_api.h"
#include "fe.h"
#include "libretro.h"

#define SAVE_TICK_NS 1000000000LL    /* how often save RAM is compared */
#define SAVE_SETTLE_NS 1000000000LL  /* unchanged this long before a write */
#define MAX_ROM_SIZE (64u << 20)
/* Core log lines per game. mGBA logs every DMA at info level (hundreds
 * per frame in some games); writing those to the card cost milliseconds
 * per frame and, when a write blocked on the card, stalled a frame for
 * over a second. */
#define MAX_INFO_LINES 100
#define MAX_WARN_LINES 200

static struct {
	const struct core_api *api;  /* the initialized core, or NULL */
	int loaded;
	int in_game;         /* after load: core info lines are limited */
	struct core_info info;
	uint32_t buttons;
	const uint16_t *frame;
	int fw, fh, fpitch;
	char bios_dir[300];
	int have_bios;
	char save_path[400];
	uint8_t *sram;       /* the core's buffer */
	size_t sram_cap;
	uint8_t *last;       /* last seen contents */
	int dirty;           /* last differs from what is on the card */
	int64_t changed_ns, next_tick_ns;
	pid_t save_pid;
	size_t save_len;
	unsigned saves_written;
	unsigned bad_cmds;
	unsigned info_lines, warn_lines, dropped_lines;
	char options[512];   /* answered core options, logged after load */
	int options_changed; /* for RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE */
} c;

/* ----------------------------------------------------------- callbacks */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	char buf[400];
	va_list ap;
	size_t n;

	if (level < RETRO_LOG_INFO)
		return;
	if (level == RETRO_LOG_INFO && !log_debug())
		return;
	if (level == RETRO_LOG_INFO ?
	    c.in_game && c.info_lines++ >= MAX_INFO_LINES :
	    c.warn_lines++ >= MAX_WARN_LINES) {
		c.dropped_lines++;
		return;
	}
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	if (level >= RETRO_LOG_WARN)
		log_msg("core: %s", buf);
	else
		log_verbose("core: %s", buf);
}

/*
 * Core options. Keys are prefixed per core, so one table serves both;
 * keys not listed get the core's default. The BIOS options depend on
 * whether <data>/bios/gba_bios.bin exists, gpSP's frame mixing (a 50:50
 * mix of this and the previous frame, for games that rely on the GBA
 * LCD's ghosting) on the game's blend setting. mGBA's libretro port has
 * no frame blending.
 */
static const char *const options[][2] = {
	{ "mgba_skip_bios", "OFF" },
	{ "mgba_idle_optimization", "Remove Known" },
	{ "mgba_frameskip", "0" },
	{ "mgba_audio_low_pass_filter", "disabled" },
	{ "gpsp_boot_mode", "game" },
	{ "gpsp_drc", "enabled" },
	{ "gpsp_sprlim", "disabled" },          /* keep the sprite limit */
	{ "gpsp_rtc", "auto" },
	{ "gpsp_rtc_time_source", "system" },   /* like mGBA */
	{ "gpsp_serial", "disabled" },
	{ "gpsp_rumble", "disabled" },
	{ "gpsp_sound_rate", "32768" },
	{ "gpsp_frameskip", "disabled" },
	{ "gpsp_color_correction", "disabled" },  /* ours, see fx.c */
};

static const char *get_variable(const char *key)
{
	const char *v = NULL;
	size_t n;

	if (!strcmp(key, "mgba_use_bios"))
		v = c.have_bios ? "ON" : "OFF";
	else if (!strcmp(key, "gpsp_bios"))
		v = c.have_bios ? "auto" : "builtin";
	else if (!strcmp(key, "gpsp_frame_mixing"))
		v = rom_settings.blend ? "enabled" : "disabled";
	for (size_t i = 0; !v && i < ARRAY_SIZE(options); i++)
		if (!strcmp(key, options[i][0]))
			v = options[i][1];
	/* Record each key once for the load log line. */
	n = strlen(c.options);
	if (!c.loaded && !strstr(c.options, key) &&
	    n + strlen(key) + 16 < sizeof(c.options))
		snprintf(c.options + n, sizeof(c.options) - n, "%s%s=%s",
			 n ? " " : "", key, v ? v : "(default)");
	if (c.loaded && !strcmp(key, "gpsp_frame_mixing"))
		log_msg("core: %s=%s, applied live", key, v);
	return v;
}

static bool environment(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format f = *(const enum retro_pixel_format *)data;

		if (f != RETRO_PIXEL_FORMAT_RGB565)
			log_msg("core asks for pixel format %d, only RGB565 supported",
				(int)f);
		return f == RETRO_PIXEL_FORMAT_RGB565;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = c.have_bios ? c.bios_dir : NULL;
		return c.have_bios;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = NULL;
		return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = data;

		v->value = get_variable(v->key);
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		/* gpSP asks after every retro_run and then reads all its
		 * options again; mGBA asks too and finds nothing it knows. */
		*(bool *)data = c.options_changed;
		c.options_changed = 0;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 0;  /* legacy SET_VARIABLES */
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
		log_msg("core message: %s",
			((const struct retro_message *)data)->msg);
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
		log_msg("core message: %s",
			((const struct retro_message_ext *)data)->msg);
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
		return true;
	default:
		if (c.bad_cmds++ < 40)
			log_verbose("core: unhandled environment cmd %u", cmd);
		return false;
	}
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data)
		return;  /* dupe: keep the previous frame */
	c.frame = data;
	c.fw = (int)w;
	c.fh = (int)h;
	c.fpitch = (int)(pitch / 2);
}

static void audio_sample(int16_t l, int16_t r)
{
	int16_t s[2] = { l, r };

	audio_push(s, 1);
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
	audio_push(data, frames);
	return frames;
}

static void input_poll(void)
{
}

static int16_t input_state(unsigned port, unsigned device, unsigned index,
			   unsigned id)
{
	(void)index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD)
		return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return (int16_t)(c.buttons & 0xffff);
	return id < 16 ? (c.buttons >> id) & 1 : 0;
}

/* ---------------------------------------------------------------- saves */

static void save_load(void)
{
	size_t len = strlen(c.save_path);
	char srm[400];
	int n;

	c.sram = c.api->get_memory_data(RETRO_MEMORY_SAVE_RAM);
	c.sram_cap = c.api->get_memory_size(RETRO_MEMORY_SAVE_RAM);
	c.dirty = 0;
	free(c.last);
	c.last = NULL;
	if (!c.sram || !c.sram_cap) {
		log_msg("save: core has no save memory");
		return;
	}
	n = read_file(c.save_path, c.sram, c.sram_cap);
	if (n > 0) {
		log_msg("save: loaded %d bytes from %s", n, c.save_path);
	} else if (n < 0 && errno == ENOENT && len > 4) {
		/* RetroArch's name for the same raw save data. */
		snprintf(srm, sizeof(srm), "%.*s.srm", (int)(len - 4), c.save_path);
		n = read_file(srm, c.sram, c.sram_cap);
		if (n > 0)
			log_msg("save: loaded %d bytes from %s (RetroArch save, "
				"saving to %s)", n, srm, c.save_path);
		else
			log_msg("save: no save file %s or .srm", c.save_path);
	} else {
		log_msg("save: no save file %s (%s)", c.save_path,
			n < 0 ? strerror(errno) : "empty");
	}
	c.last = malloc(c.sram_cap);
	if (c.last)
		memcpy(c.last, c.sram, c.sram_cap);
}

static size_t save_size(void)
{
	size_t n = c.api->save_size ? c.api->save_size() :
		   c.api->get_memory_size(RETRO_MEMORY_SAVE_RAM);

	return n > c.sram_cap ? c.sram_cap : n;
}

static void save_wait_child(void)
{
	int st;

	if (c.save_pid > 0) {
		pid_t pid = c.save_pid;

		if (waitpid(pid, &st, 0) == pid)
			core_child_exited(pid, st);
		else
			/* Already reaped elsewhere without being reported: the
			 * outcome is unknown, so write again to be safe. */
			c.dirty = 1;
	}
	c.save_pid = 0;
}

void core_child_exited(pid_t pid, int status)
{
	if (pid != c.save_pid || pid <= 0)
		return;
	c.save_pid = 0;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		c.saves_written++;
		log_msg("save: wrote %zu bytes to %s", c.save_len, c.save_path);
	} else {
		log_msg("save: writing %s failed, will retry", c.save_path);
		c.dirty = 1;
	}
}

static void save_write(int sync)
{
	size_t len = save_size();

	c.save_len = len;
	c.dirty = 0;
	if (!sync) {
		pid_t pid = fork();

		if (pid == 0)
			_exit(write_file_atomic(c.save_path, c.last, len) ? 1 : 0);
		if (pid > 0) {
			c.save_pid = pid;
			return;
		}
		log_msg("save: fork: %s, writing synchronously", strerror(errno));
	}
	if (write_file_atomic(c.save_path, c.last, len) == 0) {
		c.saves_written++;
		log_msg("save: wrote %zu bytes to %s", len, c.save_path);
	} else {
		c.dirty = 1;
	}
	log_sync();
}

static int save_check(int64_t now)
{
	if (!c.last)
		return 0;
	if (memcmp(c.sram, c.last, c.sram_cap)) {
		memcpy(c.last, c.sram, c.sram_cap);
		c.dirty = 1;
		c.changed_ns = now;
	}
	return c.dirty;
}

/*
 * Collect our own save child without waiting. The main loop's generic
 * reaper normally gets it first and passes it to core_child_exited(); this
 * keeps saving going even if a caller forgets to, since a save_pid that
 * never clears would block every later background save.
 */
static void save_poll_child(void)
{
	pid_t pid = c.save_pid, r;
	int st;

	if (pid <= 0)
		return;
	r = waitpid(pid, &st, WNOHANG);
	if (r == pid) {
		core_child_exited(pid, st);
	} else if (r < 0 && errno == ECHILD) {
		/* Reaped elsewhere and never reported: write again. */
		log_msg("save: lost track of writer %d, will rewrite", (int)pid);
		c.save_pid = 0;
		c.dirty = 1;
	}
}

void core_save_tick(int64_t now)
{
	if (!c.loaded || now < c.next_tick_ns)
		return;
	c.next_tick_ns = now + SAVE_TICK_NS;
	save_poll_child();
	/* Games write saves over several frames; wait until it settles. */
	if (save_check(now) && c.save_pid == 0 &&
	    now - c.changed_ns >= SAVE_SETTLE_NS)
		save_write(0);
}

void core_save_sync(void)
{
	if (!c.loaded)
		return;
	save_wait_child();
	if (save_check(now_ns()))
		save_write(1);
}

/* ---------------------------------------------------------------- load */

static void *read_rom(const char *path, size_t *size)
{
	struct stat st;
	void *buf;
	int n;

	if (stat(path, &st))
		return NULL;
	if (st.st_size <= 0 || st.st_size > MAX_ROM_SIZE) {
		errno = EFBIG;
		return NULL;
	}
	buf = malloc(st.st_size);
	if (!buf)
		return NULL;
	n = read_file(path, buf, st.st_size);
	if (n != st.st_size) {
		free(buf);
		errno = EIO;
		return NULL;
	}
	*size = st.st_size;
	return buf;
}

int core_load(const char *rom_path, int core, char *err, size_t errlen)
{
	struct retro_system_info si = { 0 };
	struct retro_system_av_info av = { 0 };
	struct retro_game_info gi = { 0 };
	const char *base = strrchr(rom_path, '/');
	char bios[340];
	size_t size = 0, blen;
	void *rom = NULL;
	int64_t t0 = now_ns();

	if (c.api)
		core_unload();
	if (core < 0 || core >= CORE_COUNT || !core_api_get(core)) {
		log_msg("core: %s not built in, using %s",
			core >= 0 && core < CORE_COUNT ? core_names[core] : "?",
			core_names[CORE_MGBA]);
		core = CORE_MGBA;
	}
	c.api = core_api_get(core);
	base = base ? base + 1 : rom_path;
	snprintf(c.bios_dir, sizeof(c.bios_dir), "%s/bios", fe_data_dir);
	snprintf(bios, sizeof(bios), "%s/gba_bios.bin", c.bios_dir);
	c.have_bios = access(bios, R_OK) == 0;
	blen = strlen(base);
	if (blen > 4 && base[blen - 4] == '.')
		blen -= 4;
	snprintf(c.save_path, sizeof(c.save_path), "%s/saves/%.*s.sav",
		 fe_data_dir, (int)blen, base);
	c.options[0] = 0;
	c.options_changed = 0;
	c.in_game = 0;
	c.info_lines = c.warn_lines = c.dropped_lines = 0;
	c.bad_cmds = 0;

	/* gpSP reads the ROM itself (need_fullpath), mGBA wants it in memory. */
	c.api->get_system_info(&si);
	if (!si.need_fullpath) {
		rom = read_rom(rom_path, &size);
		if (!rom) {
			snprintf(err, errlen, "Cannot read ROM: %s", strerror(errno));
			log_msg("core: read %s: %s", rom_path, strerror(errno));
			c.api = NULL;
			return -1;
		}
	} else {
		struct stat st;

		size = stat(rom_path, &st) ? 0 : (size_t)st.st_size;
	}

	c.api->set_environment(environment);
	c.api->set_video_refresh(video_refresh);
	c.api->set_audio_sample(audio_sample);
	c.api->set_audio_sample_batch(audio_batch);
	c.api->set_input_poll(input_poll);
	c.api->set_input_state(input_state);
	c.api->init();
	c.api->get_system_info(&si);
	c.info.core = core;

	gi.path = rom_path;
	gi.data = rom;
	gi.size = rom ? size : 0;
	if (!c.api->load_game(&gi)) {
		free(rom);
		c.api->deinit();
		c.api = NULL;
		snprintf(err, errlen, "The core could not load this ROM.");
		log_msg("core: %s: retro_load_game(%s) failed", core_names[core],
			rom_path);
		return -1;
	}
	free(rom);  /* the core keeps its own copy */
	c.api->get_system_av_info(&av);
	c.info.fps = av.timing.fps;
	c.info.sample_rate = av.timing.sample_rate;
	c.frame = NULL;
	c.loaded = 1;
	c.save_pid = 0;
	c.next_tick_ns = 0;
	log_msg("core: %s %s loaded %s (%zu KiB) in %lld ms: %ux%u, %.4f fps, "
		"%.0f Hz audio, BIOS %s", si.library_name, si.library_version,
		base, size >> 10, (long long)((now_ns() - t0) / 1000000),
		av.geometry.base_width, av.geometry.base_height, c.info.fps,
		c.info.sample_rate,
		c.have_bios ? bios : "HLE (built in)");
	if (c.options[0])
		log_msg("core: options %s", c.options);
	save_load();
	c.in_game = 1;
	return 0;
}

void core_unload(void)
{
	if (!c.api)
		return;
	if (c.loaded) {
		core_save_sync();
		c.api->unload_game();
	}
	c.api->deinit();
	c.api = NULL;
	if (c.dropped_lines)
		log_msg("core: %u log lines dropped (info lines limited to %d per "
			"game, warnings to %d)", c.dropped_lines, MAX_INFO_LINES,
			MAX_WARN_LINES);
	if (!c.loaded)
		return;
	c.loaded = 0;
	c.frame = NULL;
	free(c.last);
	c.last = NULL;
	c.sram = NULL;
	log_msg("core: unloaded (%u save writes)", c.saves_written);
	c.saves_written = 0;
}

int core_loaded(void)
{
	return c.loaded;
}

const struct core_info *core_info(void)
{
	return &c.info;
}

/*
 * Both cores keep the save buffer through retro_reset (mGBA keeps the
 * cartridge mapped onto it, gpSP never clears it) and detect the save
 * type again on the first access, as after a load; gpSP also flushes its
 * translation caches. So the save is only written first, in case the
 * card is pulled while the game starts over.
 */
void core_reset(void)
{
	int64_t t0 = now_ns();

	if (!c.loaded)
		return;
	core_save_sync();
	c.api->reset();
	log_msg("core: %s reset in %.1f ms", core_labels[c.info.core],
		(now_ns() - t0) / 1e6);
}

void core_run(uint32_t buttons)
{
	if (!c.loaded)
		return;
	c.buttons = buttons;
	c.api->run();
}

void core_options_changed(void)
{
	if (!c.loaded)
		return;
	if (c.info.core == CORE_GPSP)
		c.options_changed = 1;
	else
		log_msg("core: blend %s kept for gpSP, %s has no frame blending",
			rom_settings.blend ? "on" : "off",
			core_labels[c.info.core]);
}

const uint16_t *core_frame(int *w, int *h, int *pitch_px)
{
	*w = c.fw;
	*h = c.fh;
	*pitch_px = c.fpitch;
	return c.frame;
}
