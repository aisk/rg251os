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
 *
 * Resume states: when a game ends normally (quit, power off, Restart
 * with another core) or the device goes to sleep, main.c calls
 * core_state_save(): the save RAM is written first, then
 * retro_serialize goes to <data>/saves/<rom>.<core>.state (file name
 * only, like the .sav; per core, since mGBA and gpSP states are not
 * compatible), synchronously through write_file_atomic() so it is on the
 * card before the power goes. core_load() reads it back after the .sav
 * and before the first retro_run, and the game resumes where it was.
 * The .sav stays the primary record: a state is skipped (logged, the
 * game boots normally, nothing is deleted) when it is damaged, made for
 * another ROM of that name, or the save data differs from what it was
 * when the state was written (see struct state_header). Neither core
 * restores save RAM from a state (mGBA stores it but loads without it,
 * gpSP does not store it). core_reset() removes the state.
 *
 * Frame skipping (rom_settings.frameskip, rom_options[]): a fixed
 * interval with either core, Auto (skip while the audio buffer is low)
 * with gpSP only. For Auto gpSP registers an audio buffer status
 * callback, which core_run() calls right before every retro_run with
 * audio_buffer_status(); gpSP skips while the occupancy is under its
 * threshold (33% by default; audio.c gives the occupancy so that its
 * target fill reads 50%). core_run() reports whether the frame was
 * skipped, so the CPU controller (cpuctl.c) judges rendered frames only:
 * gpSP passes NULL to video_refresh for a skipped frame. mGBA hands over
 * its buffer again and cannot be asked (it also redraws only the
 * scanlines that changed, so the buffer does not tell either), so its
 * counter is followed here: like GBAVideo's frameskipCounter it starts
 * at 0 on load and reset, a frame is drawn when it is 0 or less, and at
 * the frame's end it counts down and goes back to mgba_frameskip as mGBA
 * last read it (in that retro_run, after GET_VARIABLE_UPDATE). The
 * settings and the keys sent are logged at the start, skipped frames at
 * the end of each game.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
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
#include "romdb.h"
#include "settings.h"

#define SAVE_TICK_NS 1000000000LL    /* how often save RAM is compared */
#define SAVE_SETTLE_NS 1000000000LL  /* unchanged this long before a write */
#define MAX_ROM_SIZE (64u << 20)
#define MAX_STATE_SIZE (4u << 20)
#define STATE_MAGIC "rg251st1"
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
	char state_path[400];
	uint32_t rom_size, rom_crc;  /* identify the ROM in state files */
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
	retro_audio_buffer_status_callback_t audio_status;  /* from the core */
	unsigned min_latency_ms;
	int refreshed, null_frame;  /* video_refresh in this retro_run */
	int mgba_skip, mgba_count;  /* mGBA's frameskip and counter */
	unsigned frames, skipped;   /* this game */
	unsigned audio_low;  /* frames the status said underrun likely */
	unsigned occ_n, occ_min;    /* buffer status reports, lowest one */
	uint64_t occ_sum;
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
 * whether <data>/bios/gba_bios.bin exists; the ones in rom_options[] on
 * the game's settings. The rest are fixed.
 */
static const char *const options[][2] = {
	{ "mgba_skip_bios", "OFF" },
	{ "mgba_idle_optimization", "Remove Known" },
	{ "mgba_audio_low_pass_filter", "disabled" },
	{ "gpsp_boot_mode", "game" },
	{ "gpsp_drc", "enabled" },
	{ "gpsp_sprlim", "disabled" },          /* keep the sprite limit */
	{ "gpsp_rtc", "auto" },
	{ "gpsp_rtc_time_source", "system" },   /* like mGBA */
	{ "gpsp_serial", "disabled" },
	{ "gpsp_rumble", "disabled" },
	{ "gpsp_sound_rate", "32768" },
	{ "gpsp_color_correction", "disabled" },  /* ours, see fx.c */
};

/*
 * Core options from the game's settings (struct rom_settings): the key,
 * the core that has it, the setting (its name and value names, for the
 * log), the option's value for each value of the setting, NULL for a
 * value that core does not have (then the core gets the first one), and
 * whether the core takes a change while running (both read these again
 * after RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE) or only at the next
 * start. A setting changed in game reaches the core through
 * core_options_changed(); a setting may need several keys.
 *
 * gpsp_frame_mixing: a 50:50 mix of this and the previous frame, for
 * games that rely on the GBA LCD's ghosting; mGBA's libretro port has no
 * frame blending.
 *
 * frameskip: gpSP's auto_threshold for Auto (gpsp_frameskip_threshold
 * is left at its default, 33%), fixed_interval with the interval for the
 * numbers. This mGBA has no automatic mode, only mgba_frameskip, the
 * frames skipped after each rendered one.
 */
struct rom_option {
	const char *key;
	int core;                   /* enum core_id */
	const char *setting;        /* its key in the settings file */
	const char *const *names;   /* the setting's values there */
	size_t off;                 /* its int in struct rom_settings */
	const char *const *values;  /* by the setting's value */
	int nvalues;
	int live;
};

static const char *const off_on[] = { "off", "on" };
static const char *const disabled_enabled[] = { "disabled", "enabled" };
static const char *const gpsp_frameskip[FRAMESKIP_COUNT] = {
	"disabled", "auto_threshold", "fixed_interval", "fixed_interval",
	"fixed_interval",
};
/* gpSP's default for Off and Auto, where it is not used. */
static const char *const gpsp_frameskip_interval[FRAMESKIP_COUNT] = {
	"1", "1", "1", "2", "3",
};
static const char *const mgba_frameskip[FRAMESKIP_COUNT] = {
	"0", NULL, "1", "2", "3",
};

#define ROM_OPTION(key, core, member, names, values, live) \
	{ key, core, #member, names, offsetof(struct rom_settings, member), \
	  values, ARRAY_SIZE(values), live }

static const struct rom_option rom_options[] = {
	ROM_OPTION("gpsp_frame_mixing", CORE_GPSP, blend, off_on,
		   disabled_enabled, 1),
	ROM_OPTION("gpsp_frameskip", CORE_GPSP, frameskip, frameskip_names,
		   gpsp_frameskip, 1),
	ROM_OPTION("gpsp_frameskip_interval", CORE_GPSP, frameskip,
		   frameskip_names, gpsp_frameskip_interval, 1),
	ROM_OPTION("mgba_frameskip", CORE_MGBA, frameskip, frameskip_names,
		   mgba_frameskip, 1),
};

/* The setting each rom option was last given to the core with. */
static int rom_option_set[ARRAY_SIZE(rom_options)];

static int rom_option_setting(const struct rom_option *o)
{
	int v = *(const int *)((const char *)&rom_settings + o->off);

	return v < 0 ? 0 : v >= o->nvalues ? o->nvalues - 1 : v;
}

/* The option's value for setting value v; one the core does not have
 * gets the first. */
static const char *rom_option_value(const struct rom_option *o, int v)
{
	return o->values[v] ? o->values[v] : o->values[0];
}

int core_setting_supported(const int *field, int core, int v)
{
	size_t off = (size_t)((const char *)field - (const char *)&rom_settings);
	int found = 0;

	for (size_t i = 0; i < ARRAY_SIZE(rom_options); i++) {
		const struct rom_option *o = &rom_options[i];

		if (o->off != off || o->core != core)
			continue;
		if (v >= 0 && v < o->nvalues && !o->values[v])
			return 0;
		found = 1;
	}
	return found;
}

const char *core_option_value(const char *key)
{
	const char *v = NULL;

	if (!strcmp(key, "mgba_use_bios"))
		v = c.have_bios ? "ON" : "OFF";
	else if (!strcmp(key, "gpsp_bios"))
		v = c.have_bios ? "auto" : "builtin";
	for (size_t i = 0; !v && i < ARRAY_SIZE(rom_options); i++)
		if (!strcmp(key, rom_options[i].key))
			v = rom_option_value(&rom_options[i],
					     rom_option_setting(&rom_options[i]));
	for (size_t i = 0; !v && i < ARRAY_SIZE(options); i++)
		if (!strcmp(key, options[i][0]))
			v = options[i][1];
	return v;
}

/*
 * The game's settings behind core options and what the running core got
 * for them, one line at the start:
 * "frameskip=auto (gpsp_frameskip=auto_threshold ...), blend=off (...)",
 * or "(not in mGBA)", "(mgba_frameskip=0; auto not in mGBA)".
 */
static void log_rom_options(void)
{
	char line[512];
	size_t n = 0;

	line[0] = 0;
	for (size_t i = 0; i < ARRAY_SIZE(rom_options); i++) {
		const struct rom_option *o = &rom_options[i];
		int v = rom_option_setting(o), keys = 0, seen = 0;
		const char *name = o->names[v];

		for (size_t j = 0; j < i; j++)
			seen |= rom_options[j].off == o->off;
		if (seen || n + 64 >= sizeof(line))
			continue;
		n += snprintf(line + n, sizeof(line) - n, "%s%s=%s (", n ? ", " : "",
			      o->setting, name);
		for (size_t j = i; j < ARRAY_SIZE(rom_options); j++) {
			const struct rom_option *k = &rom_options[j];

			if (k->off != o->off || k->core != c.info.core ||
			    n + 64 >= sizeof(line))
				continue;
			n += snprintf(line + n, sizeof(line) - n, "%s%s=%s",
				      keys++ ? " " : "", k->key,
				      rom_option_value(k, v));
		}
		if (!keys)
			n += snprintf(line + n, sizeof(line) - n, "not in %s)",
				      core_labels[c.info.core]);
		else if (!core_setting_supported((const int *)
				((const char *)&rom_settings + o->off), c.info.core, v))
			n += snprintf(line + n, sizeof(line) - n, "; %s not in %s)",
				      name, core_labels[c.info.core]);
		else
			n += snprintf(line + n, sizeof(line) - n, ")");
	}
	log_msg("core: game settings for %s: %s", core_labels[c.info.core],
		line);
}

static const char *get_variable(const char *key)
{
	const char *v = core_option_value(key);
	size_t n;

	/* What mGBA uses from now on (see the top of this file). */
	if (!strcmp(key, "mgba_frameskip"))
		c.mgba_skip = v ? atoi(v) : 0;
	/* Record each key once for the load log line. */
	n = strlen(c.options);
	if (!c.loaded && !strstr(c.options, key) &&
	    n + strlen(key) + 16 < sizeof(c.options))
		snprintf(c.options + n, sizeof(c.options) - n, "%s%s=%s",
			 n ? " " : "", key, v ? v : "(default)");
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
		return true;
	case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
		const struct retro_audio_buffer_status_callback *cb = data;
		retro_audio_buffer_status_callback_t f = cb ? cb->callback : NULL;

		if (f && !c.audio_status)
			log_msg("core: audio buffer status callback registered, "
				"called before each frame (frameskip on the audio "
				"buffer level in percent of twice the target fill, "
				"buffer %u frames)", audio_buffer());
		else if (!f && c.audio_status)
			log_msg("core: audio buffer status callback removed");
		c.audio_status = f;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY: {
		unsigned ms = data ? *(const unsigned *)data : 0;

		/* gpSP asks for 6 frames (128 ms) with frameskip on; the
		 * buffer stays as audio.c opened it. */
		if (ms != c.min_latency_ms && ms)
			log_msg("core: asks for at least %u ms of audio latency, "
				"keeping the %u frame buffer (%.1f ms)", ms,
				audio_buffer(), audio_rate() ?
				audio_buffer() * 1000.0 / audio_rate() : 0.0);
		else if (ms != c.min_latency_ms)
			log_msg("core: no minimum audio latency any more");
		c.min_latency_ms = ms;
		return true;
	}
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
	c.refreshed = 1;
	c.null_frame = !data;
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

/* -------------------------------------------------------------- states */

/*
 * The state file: this header, then what retro_serialize gave. The
 * payload CRC catches a truncated or damaged file before the core sees
 * it (gpSP does little checking of its own); the ROM's size and header
 * CRC catch a different ROM under the same file name (gpSP does not
 * check, mGBA only warns about another revision); the save CRC is of the
 * save data as written to the .sav right before, so a .sav replaced or
 * changed since makes the state stale. A file time comparison would do
 * the last part worse: copies from a PC can keep the old time, and
 * without a set RTC every boot starts the clock at the same point.
 * Native (little endian) fields; the frontend only runs on AArch64.
 */
struct state_header {
	char magic[8];        /* STATE_MAGIC */
	char core[8];         /* struct core_api name, NUL padded */
	uint32_t payload_len, payload_crc;
	uint32_t save_len, save_crc;
	uint32_t rom_size, rom_crc;
};

static const char *state_name(void)
{
	const char *p = strrchr(c.state_path, '/');

	return p ? p + 1 : c.state_path;
}

/* Make an unlink durable: fsync the saves directory. */
static void sync_dir_of(const char *path)
{
	char dir[400];
	char *slash;
	int fd;

	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (!slash)
		return;
	*slash = 0;
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

static void state_remove(const char *why)
{
	if (unlink(c.state_path) == 0) {
		sync_dir_of(c.state_path);
		log_msg("state: removed %s (%s)", state_name(), why);
	} else if (errno != ENOENT) {
		log_msg("state: removing %s (%s): %s", state_name(), why,
			strerror(errno));
	}
}

void core_state_save(const char *why)
{
	struct state_header *h;
	size_t size, slen;
	int64_t t0, t1, t2;
	uint8_t *buf;

	if (!c.loaded || !c.api->serialize_size)
		return;
	/* The .sav first: it stays the primary record. */
	core_save_sync();
	t0 = now_ns();
	size = c.api->serialize_size();
	if (!size || size > MAX_STATE_SIZE) {
		log_msg("state: %s gives a state size of %zu, not saving",
			c.api->name, size);
		return;
	}
	buf = calloc(1, sizeof(*h) + size);
	if (!buf) {
		log_msg("state: no memory for %zu bytes", size);
		return;
	}
	if (!c.api->serialize(buf + sizeof(*h), size)) {
		log_msg("state: %s could not serialize (%zu bytes), removing the "
			"old state", c.api->name, size);
		free(buf);
		state_remove("serialize failed");
		return;
	}
	t1 = now_ns();
	h = (struct state_header *)buf;
	memcpy(h->magic, STATE_MAGIC, sizeof(h->magic));
	snprintf(h->core, sizeof(h->core), "%.7s", c.api->name);
	h->payload_len = (uint32_t)size;
	h->payload_crc = romdb_crc32(0, buf + sizeof(*h), size);
	slen = c.sram ? save_size() : 0;
	h->save_len = (uint32_t)slen;
	h->save_crc = slen ? romdb_crc32(0, c.sram, slen) : 0;
	h->rom_size = c.rom_size;
	h->rom_crc = c.rom_crc;
	if (write_file_atomic(c.state_path, buf, sizeof(*h) + size)) {
		log_msg("state: writing %s failed (%s), removing the old state",
			state_name(), strerror(errno));
		state_remove("write failed");
	} else {
		t2 = now_ns();
		log_msg("state: wrote %zu bytes to %s (%s): serialize %.1f ms, "
			"write %.1f ms", sizeof(*h) + size, state_name(), why,
			(t1 - t0) / 1e6, (t2 - t1) / 1e6);
	}
	free(buf);
}

/* After save_load, before the first retro_run. Anything wrong: the game
 * boots normally, the state file is left for the next save to replace. */
static void state_load(void)
{
	const struct state_header *h;
	struct stat st;
	size_t want, len;
	int64_t t0 = now_ns(), t1;
	uint8_t *buf;
	const char *skip = NULL;
	char why[96];
	int n;

	if (!c.api->unserialize)
		return;
	if (stat(c.state_path, &st)) {
		if (errno == ENOENT)
			log_msg("state: none (%s)", state_name());
		else
			log_msg("state: %s: %s, not loaded", state_name(),
				strerror(errno));
		return;
	}
	len = (size_t)st.st_size;
	if (len < sizeof(*h) || len > sizeof(*h) + MAX_STATE_SIZE) {
		log_msg("state: %s has %zu bytes, not a state file, not loaded",
			state_name(), len);
		return;
	}
	buf = malloc(len);
	if (!buf) {
		log_msg("state: no memory for %zu bytes, not loaded", len);
		return;
	}
	n = read_file(c.state_path, buf, len);
	h = (const struct state_header *)buf;
	want = n >= (int)sizeof(*h) ? sizeof(*h) + h->payload_len : 0;
	if (n != (int)len) {
		snprintf(why, sizeof(why), "read %d of %zu bytes", n, len);
		skip = why;
	} else if (memcmp(h->magic, STATE_MAGIC, sizeof(h->magic))) {
		skip = "not a state file (magic)";
	} else if (strncmp(h->core, c.api->name, sizeof(h->core))) {
		snprintf(why, sizeof(why), "made by %.8s", h->core);
		skip = why;
	} else if (want != len) {
		snprintf(why, sizeof(why), "truncated or padded: %zu bytes, header "
			 "says %zu", len, want);
		skip = why;
	} else if (romdb_crc32(0, buf + sizeof(*h), h->payload_len) !=
		   h->payload_crc) {
		skip = "corrupt (payload CRC mismatch)";
	} else if (h->rom_size != c.rom_size || h->rom_crc != c.rom_crc) {
		skip = "made with another ROM of this name";
	} else if (h->save_len > (c.sram ? c.sram_cap : 0) ||
		   (h->save_len &&
		    romdb_crc32(0, c.sram, h->save_len) != h->save_crc)) {
		skip = "the save data changed since (.sav replaced or written "
		       "later)";
	}
	if (skip) {
		log_msg("state: %s (%zu bytes) skipped: %s; booting normally",
			state_name(), len, skip);
		free(buf);
		return;
	}
	/* mGBA's size depends on the save type detected so far, so it can
	 * differ before the first frame; the core checks its own data. */
	if (c.api->serialize_size() != h->payload_len)
		log_msg("state: %s reports %zu bytes now, the state has %u",
			c.api->name, c.api->serialize_size(), h->payload_len);
	if (!c.api->unserialize(buf + sizeof(*h), h->payload_len)) {
		log_msg("state: %s rejected %s (%u bytes); booting normally",
			c.api->name, state_name(), h->payload_len);
		free(buf);
		return;
	}
	t1 = now_ns();
	log_msg("state: loaded %zu bytes from %s in %.1f ms, resuming",
		len, state_name(), (t1 - t0) / 1e6);
	free(buf);
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
	size_t size = 0;
	int blen;
	void *rom = NULL;
	int64_t t0 = now_ns();

	if (c.api)
		core_unload();
	if (!core_available(core)) {
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
	blen = romdb_stem_len(base);
	snprintf(c.save_path, sizeof(c.save_path), "%s/saves/%.*s.sav",
		 fe_data_dir, blen, base);
	/* Per core: mGBA and gpSP states are not compatible. */
	snprintf(c.state_path, sizeof(c.state_path), "%s/saves/%.*s.%s.state",
		 fe_data_dir, blen, base, c.api->name);
	c.options[0] = 0;
	c.options_changed = 0;
	c.audio_status = NULL;
	c.min_latency_ms = 0;
	c.frames = c.skipped = c.audio_low = 0;
	c.occ_n = c.occ_sum = 0;
	c.occ_min = 100;
	c.mgba_skip = c.mgba_count = 0;
	for (size_t i = 0; i < ARRAY_SIZE(rom_options); i++)
		rom_option_set[i] = rom_option_setting(&rom_options[i]);
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
	{
		uint8_t hdr[192];
		int n = rom ? (int)(size < sizeof(hdr) ? size : sizeof(hdr)) :
			read_file(rom_path, hdr, sizeof(hdr));

		if (rom && n > 0)
			memcpy(hdr, rom, n);
		c.rom_size = (uint32_t)size;
		c.rom_crc = n > 0 ? romdb_crc32(0, hdr, n) : 0;
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
	log_rom_options();
	save_load();
	state_load();
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
	c.audio_status = NULL;
	if (c.loaded && (c.skipped || rom_settings.frameskip != FRAMESKIP_OFF)) {
		char st[96] = "no buffer status reported";

		if (c.occ_n)
			snprintf(st, sizeof(st), "buffer status %u times: avg %.0f%%, "
				 "lowest %u%%, %u under one frame", c.occ_n,
				 (double)c.occ_sum / c.occ_n, c.occ_min, c.audio_low);
		log_msg("core: frameskip %s: %u of %u frames skipped (%.1f%%), %s",
			frameskip_names[rom_settings.frameskip >= 0 &&
					rom_settings.frameskip < FRAMESKIP_COUNT ?
					rom_settings.frameskip : 0],
			c.skipped, c.frames,
			c.frames ? 100.0 * c.skipped / c.frames : 0.0, st);
	}
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

int core_available(int core)
{
	return core >= 0 && core < CORE_COUNT && core_api_get(core) != NULL;
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
 * card is pulled while the game starts over. This core's state file is
 * removed: the game runs on from the beginning, and a crash or a pulled
 * battery must not resume what was before the reset. The next quit
 * writes it again.
 */
void core_reset(void)
{
	int64_t t0 = now_ns();

	if (!c.loaded)
		return;
	core_save_sync();
	state_remove("restart game");
	c.api->reset();
	c.mgba_count = 0;  /* GBAVideoReset */
	log_msg("core: %s reset in %.1f ms", core_labels[c.info.core],
		(now_ns() - t0) / 1e6);
}

int core_run(uint32_t buttons)
{
	int skipped, mgba_skipped = c.mgba_count > 0;

	if (!c.loaded)
		return 0;
	c.buttons = buttons;
	if (c.audio_status) {
		unsigned occ;
		int low, active = audio_buffer_status(&occ, &low);

		if (active) {
			c.audio_low += low;
			c.occ_n++;
			c.occ_sum += occ;
			c.occ_min = occ < c.occ_min ? occ : c.occ_min;
		}
		c.audio_status(active, occ, active && low);
	}
	c.refreshed = c.null_frame = 0;
	c.api->run();
	/* mGBA's counter at the frame's end (0 unless it read a frameskip). */
	if (--c.mgba_count < 0)
		c.mgba_count = c.mgba_skip;
	skipped = !c.refreshed || c.null_frame || mgba_skipped;
	c.frames++;
	c.skipped += skipped;
	return skipped;
}

void core_frame_counts(unsigned *frames, unsigned *skipped)
{
	*frames = c.frames;
	*skipped = c.skipped;
}

void core_options_changed(void)
{
	if (!c.loaded)
		return;
	for (size_t i = 0; i < ARRAY_SIZE(rom_options); i++) {
		const struct rom_option *o = &rom_options[i];
		int v = rom_option_setting(o), old = rom_option_set[i];
		const char *val = rom_option_value(o, v);

		if (v == old)
			continue;
		rom_option_set[i] = v;
		/* Another value of the setting, the same for this key. */
		if (!strcmp(val, rom_option_value(o, old)) && o->values[v])
			continue;
		if (o->core != c.info.core) {
			log_msg("core: %s=%s kept for %s, %s does not have it",
				o->key, val, core_labels[o->core],
				core_labels[c.info.core]);
			continue;
		}
		if (!o->values[v])
			log_msg("core: %s=%s is not in %s, %s=%s", o->setting,
				o->names[v], core_labels[o->core], o->key, val);
		if (o->live) {
			/* Read again after the next retro_run. */
			c.options_changed = 1;
			log_msg("core: %s=%s, applied live", o->key, val);
		} else {
			log_msg("core: %s=%s from the next start", o->key, val);
		}
	}
}

const uint16_t *core_frame(int *w, int *h, int *pitch_px)
{
	*w = c.fw;
	*h = c.fh;
	*pitch_px = c.fpitch;
	return c.frame;
}
