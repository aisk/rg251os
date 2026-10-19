/*
 * Menus on the 480x320 logical canvas, 8x8 font at 2x (30 columns).
 *
 * ROM list: one folder of roms/ at a time, its subfolders first (as
 * "name/", in yellow), then the ROMs by their No-Intro names (romdb.c;
 * the file name when unknown, " *" after the name of a modified ROM,
 * long names cut with "~"; the selected one, and the game's name above
 * the in-game menu, scroll after 1 s to show the rest, see
 * marquee_offset), up/down (left/right page), A or START starts a ROM or
 * opens a folder, B goes up to the parent folder with the folder left
 * selected (nothing at the top), the header shows the folder's path
 * relative to roms/ instead of "GBA", cut from the left with "~" when
 * long. The folder and the selected entry are kept while a game runs, so
 * quitting it comes back to the same ROM; a folder that is gone by then
 * falls back to its parent. Nothing of this is kept across boots. SELECT
 * opens the global settings (Volume, Brightness, CPU, Audio mode
 * Simple/Adaptive, Debug log on/off, see log.c, and System info, the read
 * only page of sysinfo.c). In game, SELECT+START opens the in-game menu:
 * Resume, Restart game, Volume, Brightness, CPU, Core, Frame blend,
 * Settings, System info, Quit to ROM list, Power off; B or
 * SELECT+START again resumes. Restart game starts the game over like the
 * console's reset button (its save written first); the first A only turns
 * the row into "Press A to restart", a second A within 3 s does it,
 * moving away or waiting cancels. CPU is the global CPU mode
 * (Adaptive, Max or a fixed frequency, see cpuctl.c); changed in game it
 * applies as soon as the game resumes. Core (this game: mGBA or gpSP)
 * shows the running core; left/right pick another one, the row then
 * reads "Restart with" and A saves and restarts the game with it
 * (resuming instead keeps it for the next start). Frame blend (this
 * game, gpSP's 50:50 mix of the current and the previous frame for games
 * that rely on LCD ghosting) applies on the next frame; mGBA has none,
 * so with mGBA the row reads "gpSP only" and cannot be changed, the
 * setting is kept for gpSP. The in-game Settings screen shows the game
 * with the effects applied (live preview) under a panel with Color (this
 * game: Auto, showing what it resolved to as "Auto (GBA)", Off, GBA, GBA
 * light or SP101), LCD grid, Grid darkness, Dot boost and Debug log. In menus,
 * up/down select, left/right change a value, A activates, B goes back.
 * The battery level is shown in the ROM list header and above the
 * in-game menus.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "battery.h"
#include "core.h"
#include "core_api.h"
#include "cpuctl.h"
#include "fe.h"
#include "fx.h"
#include "input.h"
#include "menu.h"
#include "romlist.h"

#define ROW_H 18
#define LIST_Y 40
#define LIST_ROWS 14
#define COLS 29
#define COL_OFF RGB(0x50, 0x58, 0x68)

/* A scrolling name: frames drawn since it was selected. */
struct marquee {
	int sel;  /* -1: start over */
	unsigned frames;
	char name[ROMDB_NAME_MAX + 2];
};

static struct romlist roms;
static int sel, top;
/* The folder shown, relative to roms/: "" at the top, "a/b" inside. */
static char cur_dir[ROMLIST_PATH];
static char message[96];
static struct marquee list_mq = { .sel = -1 };

static struct {
	int sel;
	int confirm;  /* Restart game armed: frames left, see IG_CONFIRM_FRAMES */
	char name[ROMDB_NAME_MAX + 2];
	struct marquee mq;
} ig;

uint32_t ui_repeat(uint32_t held, int64_t now)
{
	static uint32_t prev;
	static int64_t next;
	const uint32_t dirs = JOY_BIT(UP) | JOY_BIT(DOWN) | JOY_BIT(LEFT) |
			      JOY_BIT(RIGHT);
	uint32_t pressed = held & ~prev;

	if (pressed & dirs)
		next = now + 350000000LL;
	else if ((held & dirs) && (held & dirs) == (prev & dirs) && now >= next) {
		pressed |= held & dirs;
		next = now + 70000000LL;
	}
	prev = held;
	return pressed;
}

/* ------------------------------------------------------------- helpers */

static void volume_step(int d)
{
	/* 8 steps of 0.375 dB = 3 dB. */
	const int floor = 255 - 8 * 20;  /* -60 dB, below is mute */
	int v = settings.volume;

	if (d > 0)
		v = v < floor ? floor : v + 8 > 255 ? 255 : v + 8;
	else
		v = v - 8 < floor ? 0 : v - 8;
	settings.volume = v;
	audio_set_volume(v);
}

static void volume_label(char *buf, size_t size)
{
	if (settings.volume == 0)
		snprintf(buf, size, "mute");
	else
		snprintf(buf, size, "%ddB", -(255 - settings.volume) * 3 / 8);
}

static void cpu_step(int d)
{
	settings.cpu = cpuctl_mode_step(settings.cpu, d);
	cpuctl_set_mode(settings.cpu);
}

static void brightness_step(int d)
{
	int b = settings.brightness + d * 10;

	settings.brightness = b < 10 ? 5 : b > 100 ? 100 : b;
	backlight_set(settings.brightness);
}

/* Next or previous entry of a level list, from any current value. */
static int level_step(const int *levels, int n, int cur, int d)
{
	if (d > 0) {
		for (int i = 0; i < n; i++)
			if (levels[i] > cur)
				return levels[i];
		return levels[n - 1];
	}
	for (int i = n - 1; i >= 0; i--)
		if (levels[i] < cur)
			return levels[i];
	return levels[0];
}

/* Battery icon and percent, right aligned at x_right; returns left x. */
static int draw_battery(struct canvas *c, int x_right, int y)
{
	const struct battery *b = battery_get();
	char label[16];
	uint16_t col = COL_DIM;
	int iw = 26, ih = 14, x, fill;

	battery_label(b, label, sizeof(label));
	if (!label[0])
		return x_right;
	if (battery_low())
		col = COL_ERR;
	else if (b->status == BAT_CHARGING || b->status == BAT_FULL)
		col = COL_OK;
	x = x_right - iw - 3;
	draw_frame(c, x, y + 1, iw, ih, col);
	draw_fill(c, x + iw, y + 5, 3, 6, col);
	fill = (iw - 6) * (b->capacity < 0 ? 0 : b->capacity) / 100;
	draw_fill(c, x + 3, y + 4, fill < 1 ? 1 : fill, ih - 6, col);
	x -= 6 + 16 * (int)strlen(label);
	draw_text(c, x, y, 2, col, label);
	return x;
}

/* ------------------------------------------------------------ ROM list */

void romlist_menu_message(const char *msg)
{
	snprintf(message, sizeof(message), "%s", msg ? msg : "");
}

/* The selected row in view, the list not scrolled past its end. */
static void list_scroll(void)
{
	if (sel < top)
		top = sel;
	if (sel >= top + LIST_ROWS)
		top = sel - LIST_ROWS + 1;
	if (top > roms.count - LIST_ROWS)
		top = roms.count > LIST_ROWS ? roms.count - LIST_ROWS : 0;
}

/* Scan cur_dir (or the first parent that still exists) and select the
 * entry named select (a folder if dir), else the row at index fallback. */
static void list_open(const char *select, int dir, int fallback)
{
	int64_t t0 = now_ns();
	char path[ROMLIST_PATH];
	char *up;
	int n;

	for (;;) {
		n = snprintf(path, sizeof(path), "%s/roms%s%s", fe_data_dir,
			     cur_dir[0] ? "/" : "", cur_dir) < (int)sizeof(path) ?
		    romlist_scan(&roms, path) : -1;
		if (n >= 0 || !cur_dir[0])
			break;
		log_msg("roms: folder %s cannot be opened, going up", cur_dir);
		up = strrchr(cur_dir, '/');
		if (up)
			*up = 0;
		else
			cur_dir[0] = 0;
	}
	sel = fallback;
	for (int i = 0; select && select[0] && i < roms.count; i++)
		if (roms.roms[i].dir == dir && !strcmp(roms.roms[i].file, select))
			sel = i;
	if (sel >= roms.count)
		sel = roms.count ? roms.count - 1 : 0;
	list_scroll();
	list_mq.sel = -1;
	if (n < 0)
		log_msg("roms: %s cannot be opened", path);
	else
		log_msg("roms: folder /%s: %d ROMs, %d folders (%.1f ms), "
			"selected %s", cur_dir, n - roms.dirs, roms.dirs,
			(now_ns() - t0) / 1e6,
			roms.count ? roms.roms[sel].file : "nothing");
}

void romlist_menu_enter(void)
{
	char name[256] = "";
	int dir = 0;

	/* The same folder and entry as before (a game started from it). */
	if (roms.count) {
		snprintf(name, sizeof(name), "%s", roms.roms[sel].file);
		dir = roms.roms[sel].dir;
	}
	list_open(name, dir, sel);
}

/* Into the selected folder, or B: up to the parent with the folder left
 * selected. */
static void folder_enter(const char *name)
{
	size_t n = strlen(cur_dir);

	if (n + 1 + strlen(name) >= sizeof(cur_dir)) {
		romlist_menu_message("Folder path too long");
		log_msg("roms: %s/%s: path too long", cur_dir, name);
		return;
	}
	snprintf(cur_dir + n, sizeof(cur_dir) - n, "%s%s", n ? "/" : "", name);
	top = 0;
	list_open(NULL, 0, 0);
}

static void folder_up(void)
{
	char *cut = strrchr(cur_dir, '/'), name[ROMLIST_PATH];

	snprintf(name, sizeof(name), "%s", cut ? cut + 1 : cur_dir);
	if (cut)
		*cut = 0;
	else
		cur_dir[0] = 0;
	list_open(name, 1, 0);
}

/* Missing CRCs, 8 ms per frame; the selection stays on its ROM when the
 * list is sorted again. */
static void romlist_work(void)
{
	const char *cur = roms.count ? roms.roms[sel].file : NULL;

	if (!roms.pending || !romlist_step(&roms, 8000000))
		return;
	for (int i = 0; i < roms.count; i++)
		if (roms.roms[i].file == cur)
			sel = i;
	list_scroll();
}

int romlist_menu_update(uint32_t pressed, char *path, size_t size)
{
	romlist_work();
	if (!roms.count)
		pressed &= JOY_BIT(SELECT) | JOY_BIT(B);
	if (pressed & JOY_BIT(UP))
		sel = sel > 0 ? sel - 1 : roms.count - 1;
	if (pressed & JOY_BIT(DOWN))
		sel = sel < roms.count - 1 ? sel + 1 : 0;
	if (pressed & JOY_BIT(LEFT))
		sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;
	if (pressed & JOY_BIT(RIGHT))
		sel = sel + LIST_ROWS < roms.count ? sel + LIST_ROWS : roms.count - 1;
	list_scroll();
	if (pressed & JOY_BIT(SELECT)) {
		message[0] = 0;
		list_mq.sel = -1;  /* not drawn meanwhile */
		return UI_SETTINGS;
	}
	if ((pressed & JOY_BIT(B)) && cur_dir[0]) {
		message[0] = 0;
		folder_up();
		return UI_NONE;
	}
	if ((pressed & (JOY_BIT(A) | JOY_BIT(START))) && roms.roms[sel].dir) {
		message[0] = 0;
		folder_enter(roms.roms[sel].file);
		return UI_NONE;
	}
	if (pressed & (JOY_BIT(A) | JOY_BIT(START))) {
		if (snprintf(path, size, "%s/%s", roms.dir, roms.roms[sel].file) >=
		    (int)size) {
			romlist_menu_message("ROM path too long");
			return UI_NONE;
		}
		message[0] = 0;
		return UI_START_ROM;
	}
	return UI_NONE;
}

/* A name's length as shown, without a file name's ".gba". */
static int name_len(const char *name)
{
	size_t n = strlen(name);

	if (n > 4 && !strcasecmp(name + n - 4, ".gba"))
		n -= 4;
	return n;
}

/*
 * The scroll offset of a name excess pixels wider than its row, frames
 * after it was selected: 0 (cut with "~") for MQ_PAUSE frames, then one
 * pixel per frame (60 px/s, about 4 characters a second, smooth on the
 * 60 Hz panel) until its end shows, held MQ_PAUSE frames, then from the
 * start again. Counted in frames drawn (the menus are drawn on every
 * vblank) rather than in time, so every frame moves by exactly one pixel.
 */
#define MQ_PAUSE 60

int marquee_offset(int excess, unsigned frames)
{
	unsigned t;

	if (excess <= 0)
		return 0;
	t = frames % (2 * MQ_PAUSE + (unsigned)excess);
	if (t < MQ_PAUSE)
		return 0;
	t -= MQ_PAUSE;
	return t < (unsigned)excess ? (int)t : excess;
}

/* Frames drawn since this name was selected, counting this one. */
static unsigned marquee_tick(struct marquee *mq, int idx, const char *name)
{
	if (mq->sel != idx || strcmp(mq->name, name)) {
		mq->sel = idx;
		snprintf(mq->name, sizeof(mq->name), "%s", name);
		mq->frames = 0;
	} else {
		mq->frames++;
	}
	return mq->frames;
}

/* The scroll offset of name in cols columns, shown in this frame. */
static int marquee_step(struct marquee *mq, int idx, const char *name,
			int cols)
{
	return marquee_offset(16 * (name_len(name) - cols),
			      marquee_tick(mq, idx, name));
}

/* A ROM name in cols columns: cut with "~", a modified ROM's " *" kept;
 * or with offset > 0 all of it, offset pixels to the left, clipped to
 * the columns. */
void draw_rom_name(struct canvas *c, int x, int y, uint16_t col,
		   const char *name, int cols, int offset)
{
	char buf[ROMDB_NAME_MAX + 2];
	int n = name_len(name);

	if (offset > 0 && n > cols) {
		snprintf(buf, sizeof(buf), "%.*s", n, name);
		draw_text_clip(c, x - offset, y, 2, col, buf, x, x + 16 * cols);
		return;
	}
	if (cols > (int)sizeof(buf) - 1)
		cols = sizeof(buf) - 1;
	if (n > cols && n > 2 && !strcmp(name + n - 2, " *")) {
		snprintf(buf, sizeof(buf), "%.*s~*", cols - 2, name);
	} else if (n > cols) {
		snprintf(buf, sizeof(buf), "%.*s~", cols - 1, name);
	} else {
		snprintf(buf, sizeof(buf), "%.*s", n, name);
	}
	draw_text(c, x, y, 2, col, buf);
}

/* "GBA" at the top, else the folder's path, its start cut with "~" to
 * keep the innermost folder in view; then the position. */
static void list_header(struct canvas *c)
{
	const char *head = cur_dir[0] ? cur_dir : "GBA";
	char pos[24] = "", buf[32];
	int right = draw_battery(c, c->w - 8, 8);
	int cols, n = strlen(head);

	if (roms.count)
		snprintf(pos, sizeof(pos), "%d/%d", sel + 1, roms.count);
	cols = (right - 16 - 8 - 16 * ((int)strlen(pos) + 1)) / 16;
	if (cols > (int)sizeof(buf) - 1)
		cols = sizeof(buf) - 1;
	if (cols < 2)
		cols = 2;
	if (n > cols)
		snprintf(buf, sizeof(buf), "~%s", head + n - (cols - 1));
	else
		snprintf(buf, sizeof(buf), "%s", head);
	draw_text(c, 8, 8, 2, COL_HI, buf);
	if (pos[0])
		draw_text(c, 8 + 16 * ((int)strlen(buf) + 1), 8, 2, COL_DIM, pos);
}

void romlist_menu_draw(struct canvas *c)
{
	const char *keys;

	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	list_header(c);
	draw_fill(c, 0, 30, c->w, 2, COL_SEL);

	if (!roms.count && cur_dir[0]) {
		draw_text(c, 16, 80, 2, COL_FG, "Empty folder.");
	} else if (!roms.count) {
		draw_text(c, 16, 80, 2, COL_FG, "No ROMs found.");
		draw_text(c, 16, 120, 2, COL_DIM, "Copy .gba files to the");
		draw_text(c, 16, 144, 2, COL_DIM, "roms folder on the GBA");
		draw_text(c, 16, 168, 2, COL_DIM, "partition of the card.");
	}
	for (int i = 0; i < LIST_ROWS && top + i < roms.count; i++) {
		const char *name = roms.roms[top + i].id.title;
		int y = LIST_Y + i * ROW_H, offset = 0;
		uint16_t col = top + i == sel ? COL_FG : COL_DIM;

		if (top + i == sel) {
			draw_fill(c, 0, y - 1, c->w, ROW_H, COL_SEL);
			/* A new selection, or the selected ROM getting
			 * its final name, starts over. */
			offset = marquee_step(&list_mq, sel, name, COLS);
		}
		if (roms.roms[top + i].dir)
			col = COL_HI;
		draw_rom_name(c, 8, y, col, name, COLS, offset);
	}
	if (cur_dir[0])
		keys = !roms.count ? "B:up  SELECT:settings" :
		       roms.roms[sel].dir ? "A:open B:up SELECT:settings" :
		       "A:start B:up SELECT:settings";
	else
		keys = !roms.count ? "SELECT:settings" :
		       roms.roms[sel].dir ? "A:open  SELECT:settings" :
		       "A:start  SELECT:settings";
	if (message[0])
		draw_text(c, 8, c->h - 18, 2, COL_ERR, message);
	else
		draw_text(c, 8, c->h - 18, 2, COL_DIM, keys);
}

/* -------------------------------------------------------- in-game menu */

enum {
	IG_RESUME, IG_RESET, IG_VOLUME, IG_BRIGHTNESS, IG_CPU, IG_CORE,
	IG_BLEND, IG_SETTINGS, IG_SYSINFO, IG_QUIT, IG_POWEROFF, IG_COUNT
};
/* Name and rows in a 36 + 11 * 22 px box from y 34 to 312, below the
 * battery. */
#define IG_HEAD_H 36
#define IG_ROW_H 22
/* Restart game armed: frames (the menu is updated on every vblank) left
 * for the confirming A. */
#define IG_CONFIRM_FRAMES (3 * 60)

/* The game's core setting, to the next core that is built in. */
static void core_step(int d)
{
	int id = rom_settings.core;

	for (int i = 0; i < CORE_COUNT; i++) {
		id = (id + CORE_COUNT + d) % CORE_COUNT;
		if (core_api_get(id))
			break;
	}
	rom_settings.core = id;
}

static int core_pending(void)
{
	return core_loaded() && rom_settings.core != core_info()->core;
}

/* Frame blending exists in gpSP only: the running core or the one the
 * game restarts with. */
static int blend_available(void)
{
	return rom_settings.core == CORE_GPSP ||
	       (core_loaded() && core_info()->core == CORE_GPSP);
}

static void blend_toggle(void)
{
	if (!blend_available())
		return;
	rom_settings.blend = !rom_settings.blend;
	core_options_changed();
}

void ingame_menu_enter(const char *rom_name)
{
	ig.sel = 0;
	ig.confirm = 0;
	snprintf(ig.name, sizeof(ig.name), "%s", rom_name);
	ig.mq.sel = -1;
}

int ingame_menu_update(uint32_t pressed)
{
	int d = 0, prev = ig.sel;

	if (ig.confirm > 0)
		ig.confirm--;
	if (pressed & JOY_BIT(UP))
		ig.sel = (ig.sel + IG_COUNT - 1) % IG_COUNT;
	if (pressed & JOY_BIT(DOWN))
		ig.sel = (ig.sel + 1) % IG_COUNT;
	if (ig.sel != prev)
		ig.confirm = 0;
	if (pressed & JOY_BIT(LEFT))
		d = -1;
	if (pressed & JOY_BIT(RIGHT))
		d = 1;
	if (pressed & JOY_BIT(B))
		return UI_RESUME;

	if (d && ig.sel == IG_VOLUME)
		volume_step(d);
	if (d && ig.sel == IG_BRIGHTNESS)
		brightness_step(d);
	if (d && ig.sel == IG_CPU)
		cpu_step(d);
	if (d && ig.sel == IG_CORE)
		core_step(d);
	if (d && ig.sel == IG_BLEND)
		blend_toggle();
	if (pressed & (JOY_BIT(A) | JOY_BIT(START))) {
		switch (ig.sel) {
		case IG_RESUME:
			return UI_RESUME;
		case IG_RESET:
			if (ig.confirm > 0) {
				ig.confirm = 0;
				return UI_RESET;
			}
			ig.confirm = IG_CONFIRM_FRAMES;
			break;
		case IG_CORE:
			if (core_pending())
				return UI_RESTART;
			core_step(1);
			break;
		case IG_BLEND:
			blend_toggle();
			break;
		case IG_SETTINGS:
			return UI_SETTINGS;
		case IG_SYSINFO:
			return UI_SYSINFO;
		case IG_QUIT:
			return UI_QUIT;
		case IG_POWEROFF:
			return UI_POWEROFF;
		}
	}
	return UI_NONE;
}

void ingame_menu_draw(struct canvas *c, const uint16_t *frame, int pitch,
		      int w, int h)
{
	static const char *const items[IG_COUNT] = {
		"Resume", "Restart game", "Volume", "Brightness", "CPU", "Core",
		"Frame blend", "Settings", "System info", "Quit to ROM list",
		"Power off",
	};
	int bx = 56, by = 34, bw = c->w - 112;
	int bh = IG_HEAD_H + IG_COUNT * IG_ROW_H;
	int cols = (bw - 24) / 16;

	draw_image_dim(c, frame, pitch, w, h);
	draw_battery(c, c->w - 8, 16);
	draw_fill(c, bx, by, bw, bh, COL_BOX);
	draw_frame(c, bx, by, bw, bh, COL_SEL);
	draw_rom_name(c, bx + 12, by + 12, COL_HI, ig.name, cols,
		      marquee_step(&ig.mq, 0, ig.name, cols));
	for (int i = 0; i < IG_COUNT; i++) {
		int y = by + IG_HEAD_H + i * IG_ROW_H;
		uint16_t col = i == ig.sel ? COL_FG : COL_DIM;
		const char *label = items[i];

		if (i == IG_BLEND && !blend_available())
			col = i == ig.sel ? COL_DIM : COL_OFF;
		if (i == IG_CORE && core_pending())
			label = "Restart with";
		if (i == IG_RESET && ig.confirm > 0) {
			label = "Press A to restart";
			col = COL_HI;
		}
		if (i == ig.sel)
			draw_fill(c, bx + 4, y - (IG_ROW_H - 16) / 2, bw - 8,
				  IG_ROW_H, COL_SEL);
		draw_text(c, bx + 16, y, 2, col, label);
		if (i == IG_VOLUME) {
			char v[16];

			volume_label(v, sizeof(v));
			draw_textf(c, bx + bw - 16 - 16 * 6, y, 2, col, "%6s", v);
		} else if (i == IG_BRIGHTNESS) {
			draw_textf(c, bx + bw - 16 - 16 * 6, y, 2, col, "%5d%%",
				   settings.brightness);
		} else if (i == IG_CPU) {
			char v[16];

			cpuctl_mode_label(settings.cpu, v, sizeof(v));
			draw_text(c, bx + bw - 16 - 16 * (int)strlen(v), y, 2, col, v);
		} else if (i == IG_CORE) {
			const char *v = core_labels[rom_settings.core];

			draw_text(c, bx + bw - 16 - 16 * (int)strlen(v), y, 2,
				  core_pending() ? COL_HI : col, v);
		} else if (i == IG_BLEND) {
			const char *v = !blend_available() ? "gpSP only" :
					rom_settings.blend ? "On" : "Off";

			draw_text(c, bx + bw - 16 - 16 * (int)strlen(v), y, 2, col, v);
		}
	}
}

/* ------------------------------------------------------------ settings */

enum {
	SI_VOLUME, SI_BRIGHTNESS, SI_CPU, SI_COLOR, SI_GRID, SI_GRID_DIM,
	SI_GRID_BOOST, SI_AUDIO, SI_DEBUG, SI_SYSINFO, SI_BACK,
};

static const char *const si_names[] = {
	"Volume", "Brightness", "CPU", "Color", "LCD grid", "Grid darkness",
	"Dot boost", "Audio", "Debug log", "System info", "Back",
};

static const uint8_t si_romlist[] = {
	SI_VOLUME, SI_BRIGHTNESS, SI_CPU, SI_AUDIO, SI_DEBUG, SI_SYSINFO,
	SI_BACK,
};
static const uint8_t si_ingame[] = {
	SI_COLOR, SI_GRID, SI_GRID_DIM, SI_GRID_BOOST, SI_DEBUG, SI_BACK,
};

static struct {
	const uint8_t *items;
	int count, sel, ingame;
} st;

void settings_menu_enter(int ingame)
{
	st.ingame = ingame;
	st.items = ingame ? si_ingame : si_romlist;
	st.count = ingame ? (int)sizeof(si_ingame) : (int)sizeof(si_romlist);
	st.sel = 0;
}

static void settings_change(int item, int d)
{
	switch (item) {
	case SI_VOLUME:
		volume_step(d);
		break;
	case SI_BRIGHTNESS:
		brightness_step(d);
		break;
	case SI_CPU:
		cpu_step(d);
		break;
	case SI_COLOR:
		rom_settings.color = (rom_settings.color + COLOR_COUNT + d) %
				     COLOR_COUNT;
		break;
	case SI_GRID:
		settings.grid = !settings.grid;
		break;
	case SI_GRID_DIM:
		settings.grid_dim = level_step(grid_dim_levels, grid_dim_count,
					       settings.grid_dim, d);
		break;
	case SI_GRID_BOOST:
		settings.grid_boost = level_step(grid_boost_levels,
						 grid_boost_count,
						 settings.grid_boost, d);
		break;
	case SI_AUDIO:
		/* Applied when the next game starts. */
		settings.audio = settings.audio == AUDIO_SIMPLE ? AUDIO_ADAPTIVE :
				 AUDIO_SIMPLE;
		break;
	case SI_DEBUG:
		settings.debug = !settings.debug;
		log_set_debug(settings.debug);
		break;
	}
	fx_update();
}

int settings_menu_update(uint32_t pressed)
{
	int item = st.items[st.sel];

	if (pressed & JOY_BIT(UP))
		st.sel = (st.sel + st.count - 1) % st.count;
	if (pressed & JOY_BIT(DOWN))
		st.sel = (st.sel + 1) % st.count;
	if (pressed & JOY_BIT(B))
		return UI_BACK;
	if (pressed & JOY_BIT(LEFT))
		settings_change(item, -1);
	if (pressed & JOY_BIT(RIGHT))
		settings_change(item, 1);
	if (pressed & (JOY_BIT(A) | JOY_BIT(START))) {
		if (item == SI_BACK)
			return UI_BACK;
		if (item == SI_SYSINFO)
			return UI_SYSINFO;
		if (item == SI_GRID || item == SI_COLOR || item == SI_AUDIO ||
		    item == SI_DEBUG)
			settings_change(item, 1);
	}
	return UI_NONE;
}

static void settings_value(int item, char *buf, size_t size)
{
	switch (item) {
	case SI_VOLUME:
		volume_label(buf, size);
		break;
	case SI_BRIGHTNESS:
		snprintf(buf, size, "%d%%", settings.brightness);
		break;
	case SI_CPU:
		cpuctl_mode_label(settings.cpu, buf, size);
		break;
	case SI_COLOR:
		rom_color_label(buf, size);
		break;
	case SI_GRID:
		snprintf(buf, size, "%s", settings.grid ? "On" : "Off");
		break;
	case SI_GRID_DIM:
		snprintf(buf, size, "%d%%", settings.grid_dim);
		break;
	case SI_GRID_BOOST:
		if (settings.grid_boost)
			snprintf(buf, size, "+%d%%", settings.grid_boost);
		else
			snprintf(buf, size, "Off");
		break;
	case SI_AUDIO:
		snprintf(buf, size, "%s",
			 settings.audio == AUDIO_SIMPLE ? "Simple" : "Adaptive");
		break;
	case SI_DEBUG:
		snprintf(buf, size, "%s", settings.debug ? "On" : "Off");
		break;
	default:
		buf[0] = 0;
	}
}

static void settings_rows(struct canvas *c, int x, int y, int w, int row_h)
{
	for (int i = 0; i < st.count; i++, y += row_h) {
		int item = st.items[i];
		uint16_t col = i == st.sel ? COL_FG : COL_DIM;
		char v[24];

		if (!settings.grid && (item == SI_GRID_DIM || item == SI_GRID_BOOST))
			col = i == st.sel ? COL_DIM : COL_OFF;
		if (i == st.sel)
			draw_fill(c, x + 4, y - (row_h - 16) / 2, w - 8, row_h,
				  COL_SEL);
		draw_text(c, x + 12, y, 2, col, si_names[item]);
		settings_value(item, v, sizeof(v));
		/* "(game)" unless a long value like "Auto (GBA light)" needs
		 * the room. */
		if (item == SI_COLOR && 12 + 16 * 12 <=
		    w - 12 - 16 * ((int)strlen(v) + (i == st.sel ? 2 : 1)))
			draw_text(c, x + 12 + 16 * 6, y, 2,
				  i == st.sel ? COL_DIM : COL_OFF, "(game)");
		if (v[0]) {
			if (i == st.sel)
				draw_textf(c, x + w - 12 - 16 * ((int)strlen(v) + 2),
					   y, 2, col, "<%s>", v);
			else
				draw_text(c, x + w - 12 - 16 * ((int)strlen(v) + 1),
					  y, 2, col, v);
		}
	}
}

void settings_menu_draw(struct canvas *c, const uint16_t *frame, int pitch,
			int w, int h)
{
	const struct blit_fx *fx = fx_current();

	if (st.ingame) {
		int ph = 12 + st.count * 22, py = c->h - ph;

		draw_fill(c, 0, 0, c->w, c->h, 0);
		if (frame)
			draw_image_fx(c, (c->w - 2 * w) / 2, (c->h - 2 * h) / 2,
				      frame, pitch, w, h, fx);
		draw_fill(c, 0, py, c->w, ph, COL_BOX);
		draw_fill(c, 0, py, c->w, 2, COL_SEL);
		settings_rows(c, 0, py + 9, c->w, 22);
		return;
	}
	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	draw_text(c, 8, 8, 2, COL_HI, "Settings");
	draw_battery(c, c->w - 8, 8);
	draw_fill(c, 0, 30, c->w, 2, COL_SEL);
	settings_rows(c, 0, 46, c->w, 26);
	draw_text(c, 8, c->h - 18, 2, COL_DIM, "B:back");
}

void message_draw(struct canvas *c, const char *title, const char *body,
		  uint16_t col)
{
	int y = 60;

	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	draw_text(c, 16, 24, 2, col, title);
	/* Wrap at 28 columns on spaces and newlines. */
	while (body && *body && y < c->h - 40) {
		char line[32];
		int n = 0, brk = -1;

		while (body[n] && body[n] != '\n' && n < 28) {
			if (body[n] == ' ')
				brk = n;
			n++;
		}
		if (body[n] && body[n] != '\n' && brk > 0)
			n = brk;
		snprintf(line, sizeof(line), "%.*s", n, body);
		draw_text(c, 16, y, 2, COL_FG, line);
		body += n;
		while (*body == ' ' || *body == '\n')
			body++;
		y += 22;
	}
	draw_text(c, 16, c->h - 28, 2, COL_DIM, "POWER: power off");
}
