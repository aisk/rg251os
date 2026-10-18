/*
 * libretro glue for a statically linked core (mGBA, or the stub core).
 *
 * Saves: mGBA's libretro port never writes save files itself (it has no
 * use of the save directory). It keeps battery saves (SRAM, flash, EEPROM)
 * in a 128KiB buffer exposed as RETRO_MEMORY_SAVE_RAM, prefilled with
 * 0xFF, and reads it into the emulated cartridge on the first retro_run.
 * So the frontend copies <data>/saves/<rom>.sav into that buffer after
 * retro_load_game and before the first retro_run, and writes it back when
 * it changed. retro_get_memory_size() reports 128KiB until the game's save
 * type is detected, then the real size, which is what gets written.
 *
 * Writes go through write_file_atomic() in a forked child, so a slow card
 * never stalls emulation; the child sees a consistent snapshot of the
 * buffer thanks to copy on write.
 */
#include <errno.h>
#include <fcntl.h>
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
#include "fe.h"
#include "libretro.h"

#define SAVE_SETTLE_NS 1000000000LL
#define MAX_ROM_SIZE (64u << 20)

static struct {
	int loaded;
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
} c;

/* ----------------------------------------------------------- callbacks */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	char buf[400];
	va_list ap;
	size_t n;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	/* mGBA logs every DMA at debug level, thousands of lines a minute */
	if (level >= RETRO_LOG_WARN)
		log_msg("core: %s", buf);
	else if (level == RETRO_LOG_INFO)
		log_verbose("core: %s", buf);
}

static const char *get_variable(const char *key)
{
	if (!strcmp(key, "mgba_use_bios"))
		return c.have_bios ? "ON" : "OFF";
	if (!strcmp(key, "mgba_skip_bios"))
		return "OFF";
	if (!strcmp(key, "mgba_idle_optimization"))
		return "Remove Known";
	if (!strcmp(key, "mgba_frameskip"))
		return "0";
	if (!strcmp(key, "mgba_audio_low_pass_filter"))
		return "disabled";
	return NULL;
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
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 0;  /* legacy SET_VARIABLES */
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
	int n;

	c.sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	c.sram_cap = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	c.dirty = 0;
	free(c.last);
	c.last = NULL;
	if (!c.sram || !c.sram_cap) {
		log_msg("save: core has no save memory");
		return;
	}
	n = read_file(c.save_path, c.sram, c.sram_cap);
	if (n > 0)
		log_msg("save: loaded %d bytes from %s", n, c.save_path);
	else
		log_msg("save: no save file %s (%s)", c.save_path,
			n < 0 ? strerror(errno) : "empty");
	c.last = malloc(c.sram_cap);
	if (c.last)
		memcpy(c.last, c.sram, c.sram_cap);
}

static size_t save_size(void)
{
	size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);

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
	c.next_tick_ns = now + 1000000000LL;
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

int core_load(const char *rom_path, char *err, size_t errlen)
{
	struct retro_system_info si = { 0 };
	struct retro_system_av_info av = { 0 };
	struct retro_game_info gi = { 0 };
	const char *base = strrchr(rom_path, '/');
	char bios[340];
	size_t size = 0, blen;
	void *rom;
	int64_t t0 = now_ns();

	base = base ? base + 1 : rom_path;
	snprintf(c.bios_dir, sizeof(c.bios_dir), "%s/bios", fe_data_dir);
	snprintf(bios, sizeof(bios), "%s/gba_bios.bin", c.bios_dir);
	c.have_bios = access(bios, R_OK) == 0;
	blen = strlen(base);
	if (blen > 4 && base[blen - 4] == '.')
		blen -= 4;
	snprintf(c.save_path, sizeof(c.save_path), "%s/saves/%.*s.sav",
		 fe_data_dir, (int)blen, base);

	rom = read_rom(rom_path, &size);
	if (!rom) {
		snprintf(err, errlen, "Cannot read ROM: %s", strerror(errno));
		log_msg("core: read %s: %s", rom_path, strerror(errno));
		return -1;
	}

	retro_set_environment(environment);
	retro_set_video_refresh(video_refresh);
	retro_set_audio_sample(audio_sample);
	retro_set_audio_sample_batch(audio_batch);
	retro_set_input_poll(input_poll);
	retro_set_input_state(input_state);
	retro_init();
	retro_get_system_info(&si);
	c.info.name = si.library_name;
	c.info.version = si.library_version;

	gi.path = rom_path;
	gi.data = rom;
	gi.size = size;
	if (!retro_load_game(&gi)) {
		free(rom);
		retro_deinit();
		snprintf(err, errlen, "The core could not load this ROM.");
		log_msg("core: retro_load_game(%s) failed", rom_path);
		return -1;
	}
	free(rom);  /* the core keeps its own copy */
	retro_get_system_av_info(&av);
	c.info.fps = av.timing.fps;
	c.info.sample_rate = av.timing.sample_rate;
	c.info.width = av.geometry.base_width;
	c.info.height = av.geometry.base_height;
	c.frame = NULL;
	c.loaded = 1;
	c.save_pid = 0;
	c.next_tick_ns = 0;
	log_msg("core: %s %s loaded %s (%zu KiB) in %lld ms: %ux%u, %.4f fps, "
		"%.0f Hz audio, BIOS %s", si.library_name, si.library_version,
		base, size >> 10, (long long)((now_ns() - t0) / 1000000),
		c.info.width, c.info.height, c.info.fps, c.info.sample_rate,
		c.have_bios ? bios : "HLE (built in)");
	save_load();
	cpu_set_governor("performance");
	return 0;
}

void core_unload(void)
{
	if (!c.loaded)
		return;
	core_save_sync();
	retro_unload_game();
	retro_deinit();
	c.loaded = 0;
	c.frame = NULL;
	free(c.last);
	c.last = NULL;
	c.sram = NULL;
	cpu_set_governor("schedutil");
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

void core_run(uint32_t buttons)
{
	c.buttons = buttons;
	retro_run();
}

const uint16_t *core_frame(int *w, int *h, int *pitch_px)
{
	*w = c.fw;
	*h = c.fh;
	*pitch_px = c.fpitch;
	return c.frame;
}
