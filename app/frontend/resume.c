/*
 * Resume on boot: powering off in game works like sleep. When the device
 * is powered off while a game runs (in-game menu, the power key held)
 * and its resume state was written, main.c writes config/resume.txt
 * (rom=<path relative to roms/>, core=<mgba|gpsp>, why=<how>), atomically
 * like the settings. The next boot reads it after the settings and input
 * (main.c) and starts that game through the normal start, which loads
 * the state; the ROM list is set to the ROM's folder and the ROM, so
 * Quit to ROM list comes back there as after a start from the list.
 *
 * The record is used once: a boot that skips it removes it, a boot that
 * uses it first rewrites it with attempted=1, and removes it once the
 * game ran RESUME_OK_S. So a game that crashes or hangs on resuming is
 * not resumed again (the next boot finds attempted=1 and shows the
 * list). Quitting to the list, Restart game, Restart with another core
 * and powering off outside a game also remove it. resume_check gives the
 * reasons to skip (the setting "Resume on boot" off, the ROM or its state
 * file gone, the game's core changed since); main.c adds B held at boot
 * and a state that does not load (core.c's checks). Every step is logged
 * as "resume: ...".
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core.h"
#include "fe.h"
#include "resume.h"
#include "settings.h"

const char *const resume_check_names[] = {
	[RESUME_GO] = "resuming",
	[RESUME_NONE] = "no record, the last session did not end in game",
	[RESUME_BAD] = "the record is damaged",
	[RESUME_OFF] = "Resume on boot is off",
	[RESUME_ATTEMPTED] = "the last boot's resume did not run 5 s (crashed "
			     "or powered off), not trying again",
	[RESUME_NO_ROM] = "the ROM is gone",
	[RESUME_CORE] = "the game's core was changed since",
	[RESUME_NO_STATE] = "no resume state for that core",
	[RESUME_ESCAPE] = "B held at boot",
	[RESUME_FAILED] = "the game did not start or its state did not load",
};

static void record_path(char *buf, size_t size)
{
	snprintf(buf, size, "%s/config/resume.txt", fe_data_dir);
}

int resume_parse(struct resume_rec *r, const char *text)
{
	int have_rom = 0, have_core = 0;

	memset(r, 0, sizeof(*r));
	while (text && *text) {
		const char *eol = strchr(text, '\n');
		size_t len = eol ? (size_t)(eol - text) : strlen(text);
		char line[ROMLIST_PATH + 8];

		if (len && text[len - 1] == '\r')
			len--;
		if (len < sizeof(line)) {
			memcpy(line, text, len);
			line[len] = 0;
			if (!strncmp(line, "rom=", 4) && line[4] &&
			    strlen(line + 4) < sizeof(r->rom)) {
				snprintf(r->rom, sizeof(r->rom), "%s", line + 4);
				have_rom = 1;
			} else if (!strncmp(line, "core=", 5)) {
				for (int i = 0; i < CORE_COUNT; i++)
					if (!strcmp(line + 5, core_names[i])) {
						r->core = i;
						have_core = 1;
					}
			} else if (!strcmp(line, "attempted=1")) {
				r->attempted = 1;
			} else if (!strncmp(line, "why=", 4)) {
				snprintf(r->why, sizeof(r->why), "%.63s", line + 4);
			}
		}
		text = eol ? eol + 1 : NULL;
	}
	/* No way out of roms/. */
	if (have_rom && (r->rom[0] == '/' || strstr(r->rom, "..")))
		have_rom = 0;
	return have_rom && have_core ? 0 : -1;
}

int resume_format(const struct resume_rec *r, char *buf, size_t size)
{
	int n = snprintf(buf, size, "rom=%s\ncore=%s\nwhy=%s\n%s", r->rom,
			 core_names[r->core], r->why,
			 r->attempted ? "attempted=1\n" : "");

	return n > 0 && (size_t)n < size ? n : -1;
}

int resume_read(struct resume_rec *r)
{
	char path[300], buf[ROMLIST_PATH + 160];
	int n;

	record_path(path, sizeof(path));
	n = read_file(path, buf, sizeof(buf) - 1);
	if (n < 0) {
		memset(r, 0, sizeof(*r));
		return errno == ENOENT ? -1 : -2;
	}
	buf[n] = 0;
	return resume_parse(r, buf) ? -2 : 0;
}

int resume_write(const struct resume_rec *r)
{
	char path[300], buf[ROMLIST_PATH + 160];
	int n = resume_format(r, buf, sizeof(buf));

	record_path(path, sizeof(path));
	if (n < 0 || write_file_atomic(path, buf, (size_t)n)) {
		log_msg("resume: writing %s failed: %s", path,
			n < 0 ? "too long" : strerror(errno));
		return -1;
	}
	return 0;
}

void resume_clear(const char *why)
{
	char path[300];

	record_path(path, sizeof(path));
	if (unlink(path) == 0)
		log_msg("resume: record removed (%s)", why);
	else if (errno != ENOENT)
		log_msg("resume: removing %s (%s): %s", path, why,
			strerror(errno));
}

int resume_rom_path(const struct resume_rec *r, char *buf, size_t size)
{
	int n = snprintf(buf, size, "%s/roms/%s", fe_data_dir, r->rom);

	return n > 0 && (size_t)n < size ? 0 : -1;
}

int resume_rom_rel(const char *rom_path, char *buf, size_t size)
{
	char prefix[300];
	int n = snprintf(prefix, sizeof(prefix), "%s/roms/", fe_data_dir);

	if (n <= 0 || (size_t)n >= sizeof(prefix) ||
	    strncmp(rom_path, prefix, (size_t)n) || !rom_path[n] ||
	    strlen(rom_path + n) >= size)
		return -1;
	snprintf(buf, size, "%s", rom_path + n);
	return 0;
}

enum resume_check resume_check(int read_result, const struct resume_rec *r,
			       int on)
{
	char path[ROMLIST_PATH + 300], state[400];
	const char *base;
	struct stat st;
	int core;

	if (read_result)
		return read_result == -1 ? RESUME_NONE : RESUME_BAD;
	if (!on)
		return RESUME_OFF;
	if (r->attempted)
		return RESUME_ATTEMPTED;
	if (resume_rom_path(r, path, sizeof(path)) || stat(path, &st) ||
	    !S_ISREG(st.st_mode))
		return RESUME_NO_ROM;
	base = strrchr(r->rom, '/');
	base = base ? base + 1 : r->rom;
	rom_settings_load(base);
	core = rom_settings.core;
	rom_settings_load(NULL);
	if (core != r->core)
		return RESUME_CORE;
	if (core_state_path(path, core, state, sizeof(state)) ||
	    stat(state, &st))
		return RESUME_NO_STATE;
	return RESUME_GO;
}
