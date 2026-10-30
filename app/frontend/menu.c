/*
 * Menus on the 480x320 logical canvas in the 12x24 font of font.h (40
 * columns), laid out from its size (menu.h), as the pages of ui.c. The
 * in-game menu and both settings pages are lists of rows (struct row: a
 * label, how its value steps and reads, what A does), run and drawn by
 * the same code; a row shared by several menus is defined once. A page
 * is drawn only when it changed (ui.c): a key, the marquee moving, an
 * armed row running out, a ROM getting its name, the header clock's
 * minute; main.c adds the battery and the volume keys.
 *
 * ROM list: one folder of roms/ at a time, its subfolders first (as
 * "name/", in yellow), then the ROMs by their No-Intro names (romdb.c;
 * the file name when unknown, " *" after the name of a modified ROM, long
 * names cut with "~"; the selected one, and the game's name above the
 * in-game menu, scroll after 1 s to show the rest, see marquee_offset),
 * up/down (left/right page), A or START starts a ROM or opens a folder, B
 * goes up to the parent folder with the folder left selected (nothing at
 * the top), the header shows the folder's path relative to roms/ instead
 * of "GBA", cut from the left with "~" when long. The folder and the
 * selected entry are kept while a game runs, so quitting it comes back to
 * the same ROM; a folder that is gone by then falls back to its parent.
 * Nothing of this is kept across boots, but a game resumed on boot
 * (resume.c) sets the folder and the ROM first (romlist_menu_select), as
 * does a resume record that was skipped. SELECT opens the global
 * settings (Volume, Brightness, CPU, Audio mode Simple/Adaptive, Debug
 * log on/off, see log.c, Date & time, the page of clock.c that sets the
 * clock and the RTC, System info, the read only page of sysinfo.c,
 * Resume on boot on/off, see resume.c, and Power off; a full page, so no
 * Back row, B goes back). In
 * game, SELECT+START opens the in-game menu: Resume, Restart game,
 * Volume, Brightness, CPU, Core, Frameskip, Settings, System info, Quit
 * to ROM list, Power off; B or SELECT+START again resumes. Restart game
 * starts the game over like the console's reset button (its save written
 * first, its resume state removed). Restart game, Restart with (below)
 * and Power off, here and in the global settings, ask first: the first A
 * only turns the row into "Press A to restart" or "Press A to power off",
 * a second A within 3 s does it, moving away or waiting cancels (see
 * struct confirm). CPU is the global CPU mode (Adaptive, Max or a fixed
 * frequency, see cpuctl.c); changed in game it applies as soon as the
 * game resumes. Core (this game: mGBA or gpSP) shows the running core;
 * left/right pick another one, the row then reads "Restart with" and A
 * (confirmed, changing the core again cancels) saves (the resume state
 * under the old core's name, so switching back resumes there) and starts
 * the game with it (resuming instead keeps it for the next start).
 * Frameskip (this game: Off, Auto, which skips while the audio buffer
 * runs low, or 1, 2, 3 frames skipped after each one shown, see core.c)
 * applies on the next frame; Auto is gpSP only, so with mGBA it is
 * skipped over, and a game set to Auto under gpSP reads "Auto: gpSP
 * only" and runs without skipping. The in-game Settings screen shows the
 * game with the effects applied (live preview) under a panel with Color
 * (this game: Auto, showing what it resolved to as "Auto (GBA)", Off,
 * GBA, GBA light or SP101), Frame blend (this game, gpSP's 50:50 mix of
 * the current and the previous frame for games that rely on LCD
 * ghosting; it applies on the next frame, mGBA has none, so with mGBA
 * the row reads "gpSP only" and cannot be changed, the setting is kept
 * for gpSP), LCD grid, Grid darkness, Dot boost and Debug log. A video
 * (video_core.c) has a shorter in-game menu without Core, Frameskip and
 * Settings, with Picture instead: Fit or Crop, a global setting
 * (settings.video_scale) the player takes on the next frame; its Restart
 * game reads "Play from start". The list shows videos (*.mpg) among the
 * ROMs, by file name, with a play triangle at the end of the row (two
 * columns less for the name); for the selected video with a resume state
 * the key hints read "A:resume m:ss" with where it resumes (read from the
 * state when the selection changes or the list opens). In
 * menus, up/down select, left/right change a value, A activates, B goes
 * back. The battery level is shown in the ROM list and settings headers
 * and right of the game's name in the in-game menu; the ROM list header
 * also has the time (HH:MM, UTC as entered, see clock.c) once the clock
 * is set.
 */
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "battery.h"
#include "clock.h"
#include "core.h"
#include "cpuctl.h"
#include "fe.h"
#include "fx.h"
#include "input.h"
#include "menu.h"
#include "osd.h"
#include "romlist.h"
#include "settings.h"
#include "sysinfo.h"

#define COL_OFF RGB(0x50, 0x58, 0x68)
/* An empty ROM list: the first line at EMPTY_Y, a blank line, then the
 * help EMPTY_LINE px apart. */
#define EMPTY_LINE (FONT_H + 4)
#define EMPTY_GAP (EMPTY_LINE + FONT_H)
#define EMPTY_Y (LIST_Y + EMPTY_GAP)
/* The global settings are a list (LIST_Y, ROW_H); in game, a panel at
 * the bottom with IG_ROW_H px rows and IG_PANEL_PAD px above and below
 * them. */
#define IG_PANEL_PAD 6
/* Messages (message_draw): the title, the body from MSG_BODY_Y with
 * MSG_LINE px lines, the POWER hint MSG_FOOT px above the bottom. */
#define MSG_TITLE_Y 20
#define MSG_BODY_Y (MSG_TITLE_Y + FONT_H + 16)
#define MSG_LINE (FONT_H + 2)
#define MSG_FOOT (FONT_H + 4)

/* A scrolling name, advanced by update (marquee_update) and drawn at the
 * offset it reached: MQ_STEP_NS steps since it was selected. */
struct marquee {
	int sel;  /* -1: start over */
	unsigned steps;
	int64_t at;  /* the time steps counts up to */
	int offset;
	char name[ROMDB_NAME_MAX + 2];
};

static struct romlist roms;
static int sel, top;
/* The folder shown, relative to roms/: "" at the top, "a/b" inside. */
static char cur_dir[ROMLIST_PATH];
static char message[96];
static char start_path[ROMLIST_PATH + 256];  /* UI_START_ROM */
/* Selected when the list opens next (romlist_menu_select), "": none. */
static char select_next[256];
static struct marquee list_mq = { .sel = -1 };
/* Where the selected video resumes (resume_hint): its path, the
 * position, -1 for none. */
static struct {
	char path[ROMLIST_PATH + 256];
	double pos;
} hint = { .pos = -1 };

/*
 * Two-press confirmation for the rows that restart the game or power off:
 * the first A arms the row (its label then asks for another A), a second
 * A on that row within CONFIRM_NS acts. Selecting another row or waiting
 * disarms it.
 */
struct confirm {
	int row;
	int64_t until;  /* the confirming A before this, 0: not armed */
};

#define CONFIRM_NS 3000000000LL

/* ------------------------------------------------------------- helpers */

void menu_volume_label(char *buf, size_t size)
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

/* In tens, plus the minimum: 25%, 30%, 40% ... 100%. */
static void brightness_step(int d)
{
	int b = settings.brightness;

	b = d > 0 ? (b / 10 + 1) * 10 : ((b + 9) / 10 - 1) * 10;
	settings.brightness = b < BRIGHTNESS_MIN ? BRIGHTNESS_MIN :
			      b > 100 ? 100 : b;
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

/* Battery icon and percent, right aligned at x_right; returns left x
 * (battery_left without drawing). */
#define BAT_ICON_W 26

static int battery_left(int x_right)
{
	char label[16];

	battery_label(battery_get(), label, sizeof(label));
	if (!label[0])
		return x_right;
	return x_right - BAT_ICON_W - 3 - 6 - draw_text_width(label);
}

static int draw_battery(struct canvas *c, int x_right, int y)
{
	const struct battery *b = battery_get();
	char label[16];
	uint16_t col = COL_DIM;
	int iw = BAT_ICON_W, ih = 14, x, fill, iy = y + (FONT_H - ih) / 2;

	battery_label(b, label, sizeof(label));
	if (!label[0])
		return x_right;
	if (battery_low())
		col = COL_ERR;
	else if (b->status == BAT_CHARGING || b->status == BAT_FULL)
		col = COL_OK;
	x = x_right - iw - 3;
	draw_frame(c, x, iy, iw, ih, col);
	draw_fill(c, x + iw, iy + 4, 3, 6, col);
	fill = (iw - 6) * (b->capacity < 0 ? 0 : b->capacity) / 100;
	draw_fill(c, x + 3, iy + 3, fill < 1 ? 1 : fill, ih - 6, col);
	x -= 6 + draw_text_width(label);
	draw_text(c, x, y, col, label);
	return x;
}

/* ------------------------------------------------------------ ROM list */

void romlist_menu_message(const char *msg)
{
	snprintf(message, sizeof(message), "%s", msg ? msg : "");
	ui_redraw();
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

static int is_video_entry(int i)
{
	return i < roms.count && !roms.roms[i].dir &&
	       romdb_is_video(roms.roms[i].file);
}

/* The columns of entry i's name: a video's marker takes two. */
static int entry_cols(int i)
{
	return is_video_entry(i) ? LIST_COLS - 2 : LIST_COLS;
}

/* The selected video's resume position, read again when the selection
 * moved to another file (or after list_open, which clears the path). */
static void resume_hint(void)
{
	char path[sizeof(hint.path)];

	path[0] = 0;
	if (roms.count && is_video_entry(sel) &&
	    snprintf(path, sizeof(path), "%s/%s", roms.dir,
		     roms.roms[sel].file) >= (int)sizeof(path))
		path[0] = 0;
	if (!strcmp(path, hint.path) && path[0])
		return;
	snprintf(hint.path, sizeof(hint.path), "%s", path);
	hint.pos = path[0] ? video_core_saved_pos(path) : -1;
	if (hint.pos > 0)
		log_verbose("roms: %s resumes at %.1f s", roms.roms[sel].file,
			    hint.pos);
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
	list_mq.offset = 0;
	hint.path[0] = 0;
	resume_hint();
	if (n < 0)
		log_msg("roms: %s cannot be opened", path);
	else
		log_msg("roms: folder /%s: %d ROMs, %d folders (%.1f ms), "
			"selected %s", cur_dir, n - roms.dirs, roms.dirs,
			(now_ns() - t0) / 1e6,
			roms.count ? roms.roms[sel].file : "nothing");
}

const char *romlist_menu_rom(void)
{
	return start_path;
}

void romlist_menu_select(const char *rel)
{
	const char *slash = strrchr(rel, '/');
	int dlen = slash ? (int)(slash - rel) : 0;

	if (dlen >= (int)sizeof(cur_dir) ||
	    strlen(slash ? slash + 1 : rel) >= sizeof(select_next))
		return;
	snprintf(cur_dir, sizeof(cur_dir), "%.*s", dlen, rel);
	snprintf(select_next, sizeof(select_next), "%s", slash ? slash + 1 : rel);
	top = 0;
}

static void romlist_menu_enter(const char *from, int64_t now)
{
	char name[256] = "";
	int dir = 0;

	(void)from;
	(void)now;
	if (select_next[0]) {
		list_open(select_next, 0, 0);
		select_next[0] = 0;
		return;
	}
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

/* Missing CRCs, 8 ms per update: once per vblank while the list is being
 * drawn, otherwise woken every 8 ms for it. The selection stays on its
 * ROM when the list is sorted again, which is drawn. */
#define CRC_SLICE_NS 8000000LL

static void romlist_work(int64_t now)
{
	const char *cur = roms.count ? roms.roms[sel].file : NULL;

	if (!roms.pending)
		return;
	if (romlist_step(&roms, CRC_SLICE_NS)) {
		for (int i = 0; i < roms.count; i++)
			if (roms.roms[i].file == cur)
				sel = i;
		list_scroll();
		ui_redraw();
	}
	if (roms.pending)
		ui_wake_at(now + CRC_SLICE_NS);
}

static struct ui_result romlist_keys(uint32_t pressed)
{
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
		return UI_OPEN(&settings_page);
	}
	if ((pressed & JOY_BIT(B)) && cur_dir[0]) {
		message[0] = 0;
		folder_up();
		return UI_STAY;
	}
	if ((pressed & (JOY_BIT(A) | JOY_BIT(START))) && roms.roms[sel].dir) {
		message[0] = 0;
		folder_enter(roms.roms[sel].file);
		return UI_STAY;
	}
	if (pressed & (JOY_BIT(A) | JOY_BIT(START))) {
		if (snprintf(start_path, sizeof(start_path), "%s/%s", roms.dir,
			     roms.roms[sel].file) >= (int)sizeof(start_path)) {
			romlist_menu_message("ROM path too long");
			return UI_STAY;
		}
		message[0] = 0;
		return UI_ACT(UI_START_ROM);
	}
	return UI_STAY;
}

static void marquee_update(struct marquee *mq, int idx, const char *name,
			   int cols, int64_t now);

/* The header's time (when set) is drawn as the minute changes, the
 * selected name's marquee as it moves. */
static struct ui_result romlist_menu_update(uint32_t pressed, int64_t now)
{
	struct ui_result r;
	struct tm tm;

	if (realtime_utc(&tm))
		ui_redraw_at(ui_clock_tick(now, CLOCK_REALTIME, 60));
	romlist_work(now);
	r = romlist_keys(pressed);
	if (roms.pending)  /* a folder just opened */
		ui_wake_at(now + CRC_SLICE_NS);
	if (r.act == UI_NONE)
		resume_hint();
	if (r.act == UI_NONE && roms.count)
		marquee_update(&list_mq, sel, roms.roms[sel].id.title,
			       entry_cols(sel), now);
	return r;
}

/* A name's length as shown, without a file name's ".gba". */
static int name_len(const char *name)
{
	return romdb_stem_len(name);
}

/*
 * The scroll offset of a name excess pixels wider than its row, steps of
 * MQ_STEP_NS after it was selected: 0 (cut with "~") for MQ_PAUSE steps
 * (1 s), then one pixel per step (60 px/s, about 4 characters a second,
 * smooth on the 60 Hz panel) until its end shows, held MQ_PAUSE steps,
 * then from the start again.
 */
#define MQ_PAUSE 60
#define MQ_STEP_NS 16666667LL

int marquee_offset(int excess, unsigned steps)
{
	unsigned t;

	if (excess <= 0)
		return 0;
	t = steps % (2 * MQ_PAUSE + (unsigned)excess);
	if (t < MQ_PAUSE)
		return 0;
	t -= MQ_PAUSE;
	return t < (unsigned)excess ? (int)t : excess;
}

/*
 * The marquee of name (row idx) in cols columns at now: a new selection or
 * name starts over. Steps are counted in time, rounded to the nearest, so
 * a redraw on every vblank (60.02 Hz) moves one pixel each (once in about
 * 50 s one stays put) and the pace does not depend on how often this runs.
 * A new offset is drawn; the next one is asked for (ui_wake_at) for when it
 * is due, which in the pauses is a second away.
 */
static void marquee_update(struct marquee *mq, int idx, const char *name,
			   int cols, int64_t now)
{
	int excess = FONT_W * (name_len(name) - cols), off;
	unsigned k;

	if (mq->sel != idx || strcmp(mq->name, name)) {
		mq->sel = idx;
		snprintf(mq->name, sizeof(mq->name), "%s", name);
		mq->steps = 0;
		mq->at = now;
	} else if (now - mq->at >= MQ_STEP_NS / 2) {
		k = (unsigned)((now - mq->at + MQ_STEP_NS / 2) / MQ_STEP_NS);
		mq->steps += k;
		mq->at += k * MQ_STEP_NS;
	}
	off = marquee_offset(excess, mq->steps);
	if (off != mq->offset) {
		mq->offset = off;
		ui_redraw();
	}
	if (excess <= 0)
		return;
	for (k = 1; marquee_offset(excess, mq->steps + k) == off; k++)
		;
	ui_wake_at(mq->at + k * MQ_STEP_NS - MQ_STEP_NS / 2);
}

/* The offset to draw idx's name at: 0 until its update. */
static int marquee_shown(const struct marquee *mq, int idx)
{
	return mq->sel == idx ? mq->offset : 0;
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
		draw_text_clip(c, x - offset, y, col, buf, x, x + FONT_W * cols);
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
	draw_text(c, x, y, col, buf);
}

/* "GBA" at the top, else the folder's path, its start cut with "~" to
 * keep the innermost folder in view; then the position. The time left of
 * the battery, only when the clock was set (realtime_utc). */
static void list_header(struct canvas *c)
{
	const char *head = cur_dir[0] ? cur_dir : "GBA";
	char pos[24] = "", buf[64];
	int right = draw_battery(c, c->w - 8, HEAD_Y);
	int cols, n = strlen(head);
	struct tm tm;

	if (realtime_utc(&tm)) {
		right -= FONT_W * 6;
		draw_textf(c, right, HEAD_Y, COL_DIM, "%02d:%02d", tm.tm_hour,
			   tm.tm_min);
	}

	if (roms.count)
		snprintf(pos, sizeof(pos), "%d/%d", sel + 1, roms.count);
	cols = (right - FONT_W - 8 - FONT_W * ((int)strlen(pos) + 1)) / FONT_W;
	if (cols > (int)sizeof(buf) - 1)
		cols = sizeof(buf) - 1;
	if (cols < 2)
		cols = 2;
	if (n > cols)
		snprintf(buf, sizeof(buf), "~%s", head + n - (cols - 1));
	else
		snprintf(buf, sizeof(buf), "%s", head);
	right = draw_text(c, 8, HEAD_Y, COL_HI, buf);
	if (pos[0])
		draw_text(c, right + FONT_W, HEAD_Y, COL_DIM, pos);
}

static void romlist_menu_draw(struct canvas *c, const struct game_view *g)
{
	const char *keys;
	char buf[48], t[16];

	(void)g;
	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	list_header(c);
	draw_fill(c, 0, HEAD_LINE_Y, c->w, 2, COL_SEL);

	if (!roms.count && cur_dir[0]) {
		draw_text(c, 16, EMPTY_Y, COL_FG, "Empty folder.");
	} else if (!roms.count) {
		static const char *const help[] = {
			"Copy .gba files to the", "roms folder on the GBA",
			"partition of the card.",
		};

		draw_text(c, 16, EMPTY_Y, COL_FG, "No ROMs found.");
		for (int i = 0; i < 3; i++)
			draw_text(c, 16, EMPTY_Y + EMPTY_GAP + i * EMPTY_LINE,
				  COL_DIM, help[i]);
	}
	for (int i = 0; i < LIST_ROWS && top + i < roms.count; i++) {
		const char *name = roms.roms[top + i].id.title;
		int y = LIST_Y + i * ROW_H, offset = 0;
		uint16_t col = top + i == sel ? COL_FG : COL_DIM;

		if (top + i == sel) {
			draw_fill(c, 0, y - ROW_PAD, c->w, ROW_H, COL_SEL);
			offset = marquee_shown(&list_mq, sel);
		}
		if (roms.roms[top + i].dir)
			col = COL_HI;
		draw_rom_name(c, LIST_X, y, col, name, entry_cols(top + i),
			      offset);
		/* A video: a play triangle right of its name. */
		if (is_video_entry(top + i))
			draw_triangle(c, LIST_X + FONT_W * LIST_COLS - 12,
				      y + (FONT_H - 12) / 2, 10, 12, col);
	}
	if (cur_dir[0])
		keys = !roms.count ? "B:up  SELECT:settings" :
		       roms.roms[sel].dir ? "A:open B:up SELECT:settings" :
		       "A:start B:up SELECT:settings";
	else
		keys = !roms.count ? "SELECT:settings" :
		       roms.roms[sel].dir ? "A:open  SELECT:settings" :
		       "A:start  SELECT:settings";
	if (roms.count && is_video_entry(sel) && hint.pos > 0) {
		osd_format_time(hint.pos, hint.pos >= 3600, t, sizeof(t));
		snprintf(buf, sizeof(buf), "A:resume %s%s", t, cur_dir[0] ?
			 " B:up SELECT:settings" : "  SELECT:settings");
		keys = buf;
	}
	if (message[0])
		draw_text(c, 8, FOOT_Y, COL_ERR, message);
	else
		draw_text(c, 8, FOOT_Y, COL_DIM, keys);
}

const struct page romlist_page = {
	"ROM list", romlist_menu_enter, romlist_menu_update, romlist_menu_draw,
	0,
};

/* -------------------------------------------------------- confirmation */

/* On every update, before the input: disarmed (and drawn so) once the
 * time is up or off the row, otherwise woken when it will be up. */
static void confirm_tick(struct confirm *cf, int row, int64_t now)
{
	if (!cf->until)
		return;
	if (now >= cf->until || row != cf->row) {
		cf->until = 0;
		ui_redraw();
	} else {
		ui_wake_at(cf->until);
	}
}

static int confirm_armed(const struct confirm *cf, int row)
{
	return cf->until && cf->row == row;
}

/* An A on the row: 1 when it confirms, otherwise it arms the row. */
static int confirm_press(struct confirm *cf, int row, int64_t now)
{
	if (confirm_armed(cf, row)) {
		cf->until = 0;
		return 1;
	}
	cf->row = row;
	cf->until = now + CONFIRM_NS;
	ui_wake_at(cf->until);
	return 0;
}

/* ----------------------------------------------------------- menu rows */

/*
 * A row of the in-game menu or the settings. Its value is an int (field)
 * that left/right step through min..max, wrapping, shown as names[v -
 * min] or as a number; or step() changes it, and value() formats it. A
 * game setting behind core options (option, core.c's rom_options[]) can
 * only be changed with a core that has it (the one picked or the one
 * running), otherwise the row is dimmed and reads "<core> only"; values
 * that core does not have are stepped over, and one already set reads
 * "<value>: <core> only", dimmed; a change goes to
 * core_options_changed(). A or
 * START steps a ROW_TOGGLE row forward and returns press, after a second
 * A when ROW_CONFIRM (struct confirm, the label reads armed meanwhile). A
 * row with pending() (the core) takes effect only through press: while
 * pending its label is pending_label, its value highlighted and A asks
 * to confirm press, otherwise A steps.
 */
struct row {
	const char *label;
	int *field;
	int min, max;
	const char *const *names;
	void (*step)(int d);
	void (*value)(char *buf, size_t size);
	int option;           /* field is a game setting behind core options */
	int (*active)(void);  /* NULL: always; dimmed (still changes) when 0 */
	const char *note;     /* after the label, when there is room */
	unsigned flags;
	struct ui_result press;
	const char *armed;
	int (*pending)(void);
	const char *pending_label;
};

#define ROW_TOGGLE  1u
#define ROW_CONFIRM 2u

/* A menu of rows and its selection; B returns back. */
struct menu {
	const struct row *const *rows;
	int count, sel;
	struct confirm confirm;  /* row: the index */
	struct ui_result back;
};

static void menu_open(struct menu *mn, const struct row *const *rows,
		      int count, struct ui_result back)
{
	mn->rows = rows;
	mn->count = count;
	mn->sel = 0;
	mn->confirm.until = 0;
	mn->back = back;
}

/* A core option row's value v (any for v < 0) with the core picked for
 * the game or the one running. */
static int row_has(const struct row *r, int v)
{
	return !r->option ||
	       core_setting_supported(r->field, rom_settings.core, v) ||
	       (core_loaded() &&
		core_setting_supported(r->field, core_info()->core, v));
}

static int row_usable(const struct row *r)
{
	return row_has(r, -1);
}

/* The first core that has value v (any for v < 0) of a row's setting. */
static const char *row_core_label(const struct row *r, int v)
{
	for (int i = 0; i < CORE_COUNT; i++)
		if (core_setting_supported(r->field, i, v))
			return core_labels[i];
	return "?";
}

static void row_step(struct menu *mn, const struct row *r, int d)
{
	if (!row_usable(r))
		return;
	if (r->step) {
		r->step(d);
	} else if (r->field) {
		int v = *r->field;

		for (int i = r->min; i <= r->max; i++) {
			v += d;
			v = v > r->max ? r->min : v < r->min ? r->max : v;
			if (row_has(r, v))
				break;
		}
		*r->field = v;
	} else {
		return;
	}
	/* A pending choice changed again: confirm it again. */
	mn->confirm.until = 0;
	if (r->option)
		core_options_changed();
	fx_update();
}

static void row_value(const struct row *r, char *buf, size_t size)
{
	buf[0] = 0;
	if (!row_usable(r))
		snprintf(buf, size, "%s only", row_core_label(r, -1));
	else if (r->field && r->names && !row_has(r, *r->field))
		snprintf(buf, size, "%s: %s only", r->names[*r->field - r->min],
			 row_core_label(r, *r->field));
	else if (r->value)
		r->value(buf, size);
	else if (r->field && r->names)
		snprintf(buf, size, "%s", r->names[*r->field - r->min]);
	else if (r->field)
		snprintf(buf, size, "%d", *r->field);
}

static struct ui_result menu_update(struct menu *mn, uint32_t pressed,
				    int64_t now)
{
	const struct row *r;

	if (pressed & JOY_BIT(UP))
		mn->sel = (mn->sel + mn->count - 1) % mn->count;
	if (pressed & JOY_BIT(DOWN))
		mn->sel = (mn->sel + 1) % mn->count;
	r = mn->rows[mn->sel];
	confirm_tick(&mn->confirm, mn->sel, now);
	if (pressed & JOY_BIT(B))
		return mn->back;
	if (pressed & JOY_BIT(LEFT))
		row_step(mn, r, -1);
	if (pressed & JOY_BIT(RIGHT))
		row_step(mn, r, 1);
	if (!(pressed & (JOY_BIT(A) | JOY_BIT(START))))
		return UI_STAY;
	if (r->pending && !r->pending()) {
		row_step(mn, r, 1);
		return UI_STAY;
	}
	if (r->flags & ROW_TOGGLE)
		row_step(mn, r, 1);
	if ((r->flags & ROW_CONFIRM) &&
	    !confirm_press(&mn->confirm, mn->sel, now))
		return UI_STAY;
	return r->press;
}

/*
 * The rows from y, row_h apart, across x .. x + w: the selection bar, the
 * label FONT_W in, the value right aligned FONT_W from the right edge;
 * with arrows (the settings) the selected value in "<>" and the others
 * one column further left.
 */
static void menu_draw_rows(struct canvas *c, const struct menu *mn, int x,
			   int y, int w, int row_h, int arrows)
{
	const int right = x + w - FONT_W;

	for (int i = 0; i < mn->count; i++, y += row_h) {
		const struct row *r = mn->rows[i];
		int on = i == mn->sel, pending = r->pending && r->pending();
		int vx, nx = x + FONT_W * (2 + (int)strlen(r->label));
		const char *label = pending ? r->pending_label : r->label;
		uint16_t col = on ? COL_FG : COL_DIM;
		char v[24];

		if (!row_usable(r) || (r->active && !r->active()) ||
		    (r->field && !row_has(r, *r->field)))
			col = on ? COL_DIM : COL_OFF;
		if (confirm_armed(&mn->confirm, i)) {
			label = r->armed;
			col = COL_HI;
		}
		if (on)
			draw_fill(c, x + 4, y - (row_h - FONT_H) / 2, w - 8, row_h,
				  COL_SEL);
		draw_text(c, x + FONT_W, y, col, label);
		row_value(r, v, sizeof(v));
		vx = right - FONT_W * ((int)strlen(v) +
				       (arrows ? on ? 2 : 1 : 0));
		/* The note unless a long value needs the room. */
		if (r->note && nx + draw_text_width(r->note) <= vx)
			draw_text(c, nx, y, on ? COL_DIM : COL_OFF, r->note);
		if (pending)
			col = COL_HI;
		if (v[0] && arrows && on)
			draw_textf(c, vx, y, col, "<%s>", v);
		else if (v[0])
			draw_text(c, vx, y, col, v);
	}
}

/* Rows of both menus. */
static const char *const off_on[] = { "Off", "On" };
static const char *const audio_labels[] = { "Simple", "Adaptive" };

static void brightness_value(char *buf, size_t size)
{
	snprintf(buf, size, "%d%%", settings.brightness);
}

static void cpu_value(char *buf, size_t size)
{
	cpuctl_mode_label(settings.cpu, buf, size);
}

/* The game's core setting, to the next core that is built in. */
static void core_step(int d)
{
	int id = rom_settings.core;

	for (int i = 0; i < CORE_GBA_COUNT; i++) {
		id = (id + CORE_GBA_COUNT + d) % CORE_GBA_COUNT;
		if (core_available(id))
			break;
	}
	rom_settings.core = id;
}

static void core_value(char *buf, size_t size)
{
	snprintf(buf, size, "%s", core_labels[rom_settings.core]);
}

/* Another core than the running one: the game restarts with it. */
static int core_pending(void)
{
	return core_loaded() && rom_settings.core != core_info()->core;
}

static int grid_on(void)
{
	return settings.grid;
}

static void grid_dim_step(int d)
{
	settings.grid_dim = level_step(grid_dim_levels, grid_dim_count,
				       settings.grid_dim, d);
}

static void grid_dim_value(char *buf, size_t size)
{
	snprintf(buf, size, "%d%%", settings.grid_dim);
}

static void grid_boost_step(int d)
{
	settings.grid_boost = level_step(grid_boost_levels, grid_boost_count,
					 settings.grid_boost, d);
}

static void grid_boost_value(char *buf, size_t size)
{
	if (settings.grid_boost)
		snprintf(buf, size, "+%d%%", settings.grid_boost);
	else
		snprintf(buf, size, "Off");
}

/* Fit or Crop for videos (global), to the running video player now. */
static void video_scale_step(int d)
{
	(void)d;
	settings.video_scale = settings.video_scale == VIDEO_CROP ? VIDEO_FIT :
			       VIDEO_CROP;
	log_msg("menu: video picture %s", video_scale_names[settings.video_scale]);
	core_options_changed();
}

static void debug_step(int d)
{
	(void)d;
	settings.debug = !settings.debug;
	log_set_debug(settings.debug);
}

static const struct row
	row_resume = { "Resume", .press = { UI_RESUME, 0 } },
	row_reset = {
		"Restart game", .flags = ROW_CONFIRM, .press = { UI_RESET, 0 },
		.armed = "Press A to restart",
	},
	row_play_start = {
		"Play from start", .flags = ROW_CONFIRM, .press = { UI_RESET, 0 },
		.armed = "Press A to restart",
	},
	row_volume = {
		"Volume", .step = audio_volume_step, .value = menu_volume_label,
	},
	row_brightness = {
		"Brightness", .step = brightness_step, .value = brightness_value,
	},
	row_cpu = { "CPU", .step = cpu_step, .value = cpu_value },
	row_core = {
		"Core", .step = core_step, .value = core_value,
		.flags = ROW_CONFIRM, .press = { UI_RESTART, 0 },
		.armed = "Press A to restart", .pending = core_pending,
		.pending_label = "Restart with",
	},
	row_blend = {
		"Frame blend", &rom_settings.blend, 0, 1, off_on,
		.option = 1, .note = "(game)", .flags = ROW_TOGGLE,
	},
	row_frameskip = {
		"Frameskip", &rom_settings.frameskip, 0, FRAMESKIP_COUNT - 1,
		frameskip_labels, .option = 1, .flags = ROW_TOGGLE,
	},
	row_game_settings = {
		"Settings", .press = { UI_PUSH, &game_settings_page },
	},
	row_sysinfo = { "System info", .press = { UI_PUSH, &sysinfo_page } },
	row_quit = { "Quit to ROM list", .press = { UI_QUIT, 0 } },
	row_poweroff = {
		"Power off", .flags = ROW_CONFIRM, .press = { UI_POWEROFF, 0 },
		.armed = "Press A to power off",
	},
	row_color = {
		"Color", &rom_settings.color, 0, COLOR_COUNT - 1,
		.value = rom_color_label, .note = "(game)", .flags = ROW_TOGGLE,
	},
	row_grid = {
		"LCD grid", &settings.grid, 0, 1, off_on, .flags = ROW_TOGGLE,
	},
	row_grid_dim = {
		"Grid darkness", .step = grid_dim_step, .value = grid_dim_value,
		.active = grid_on,
	},
	row_grid_boost = {
		"Dot boost", .step = grid_boost_step, .value = grid_boost_value,
		.active = grid_on,
	},
	row_audio = {
		"Audio", &settings.audio, AUDIO_SIMPLE, AUDIO_ADAPTIVE,
		audio_labels, .flags = ROW_TOGGLE,  /* from the next game */
	},
	row_debug = {
		"Debug log", &settings.debug, 0, 1, off_on, .step = debug_step,
		.flags = ROW_TOGGLE,
	},
	row_datetime = { "Date & time", .press = { UI_PUSH, &datetime_page } },
	row_video_scale = {
		"Picture", &settings.video_scale, 0, VIDEO_SCALE_COUNT - 1,
		video_scale_labels, .step = video_scale_step, .flags = ROW_TOGGLE,
	},
	row_resume_boot = {
		"Resume on boot", &settings.resume, 0, 1, off_on,
		.flags = ROW_TOGGLE,
	},
	row_back = { "Back", .press = { UI_BACK, 0 } };

/* -------------------------------------------------------- in-game menu */

static const struct row *const ig_rows[] = {
	&row_resume, &row_reset, &row_volume, &row_brightness, &row_cpu,
	&row_core, &row_frameskip, &row_game_settings, &row_sysinfo, &row_quit,
	&row_poweroff,
};
_Static_assert(ARRAY_SIZE(ig_rows) == IG_ROWS, "IG_ROWS in menu.h");
/* A video (video_core.c): no core, frameskip or GBA picture settings, but
 * how the picture fills the screen. */
static const struct row *const ig_video_rows[] = {
	&row_resume, &row_play_start, &row_volume, &row_brightness, &row_cpu,
	&row_video_scale, &row_sysinfo, &row_quit, &row_poweroff,
};
_Static_assert(ARRAY_SIZE(ig_video_rows) <= IG_ROWS, "IG_ROWS in menu.h");

static struct {
	struct menu menu;
	char name[ROMDB_NAME_MAX + 2];
	struct marquee mq;
} ig;

void ingame_menu_set_name(const char *rom_name)
{
	snprintf(ig.name, sizeof(ig.name), "%s", rom_name);
}

static void ingame_menu_enter(const char *from, int64_t now)
{
	(void)from;
	(void)now;
	if (core_loaded() && core_info()->core == CORE_VIDEO)
		menu_open(&ig.menu, ig_video_rows, ARRAY_SIZE(ig_video_rows),
			  UI_ACT(UI_RESUME));
	else
		menu_open(&ig.menu, ig_rows, ARRAY_SIZE(ig_rows),
			  UI_ACT(UI_RESUME));
	ig.mq.sel = -1;
	ig.mq.offset = 0;
}

/* The game's name in the box's columns left of the battery, which is
 * right aligned like the values. */
static int ig_name_cols(void)
{
	return (battery_left(IG_BOX_X + IG_BOX_W - FONT_W) - FONT_W -
		(IG_BOX_X + 12)) / FONT_W;
}

static struct ui_result ingame_menu_update(uint32_t pressed, int64_t now)
{
	struct ui_result r = menu_update(&ig.menu, pressed, now);

	marquee_update(&ig.mq, 0, ig.name, ig_name_cols(), now);
	return r;
}

static void ingame_menu_draw(struct canvas *c, const struct game_view *g)
{
	/* The box fits its rows: IG_BOX_H for the game's IG_ROWS. */
	const int bh = IG_BOX_H - (IG_ROWS - ig.menu.count) * IG_ROW_H;
	const int bx = IG_BOX_X, by = (CANVAS_H - bh) / 2, bw = IG_BOX_W;
	const int ny = by + (IG_HEAD_H - FONT_H) / 2;
	int cols;

	draw_image_dim(c, g->px, g->pitch, g->w, g->h);
	draw_fill(c, bx, by, bw, bh, COL_BOX);
	draw_frame(c, bx, by, bw, bh, COL_SEL);
	draw_battery(c, bx + bw - FONT_W, ny);
	cols = ig_name_cols();
	draw_rom_name(c, bx + 12, ny, COL_HI, ig.name, cols,
		      marquee_shown(&ig.mq, 0));
	menu_draw_rows(c, &ig.menu, bx, by + IG_HEAD_H, bw, IG_ROW_H, 0);
}

const struct page ingame_page = {
	"in-game menu", ingame_menu_enter, ingame_menu_update, ingame_menu_draw,
	0,
};

/* ------------------------------------------------------------ settings */

/* A full page (LIST_ROWS): B goes back, as the footer says. */
static const struct row *const settings_rows[] = {
	&row_volume, &row_brightness, &row_cpu, &row_audio, &row_debug,
	&row_datetime, &row_sysinfo, &row_resume_boot, &row_poweroff,
};
_Static_assert(ARRAY_SIZE(settings_rows) <= LIST_ROWS, "settings rows");
static const struct row *const game_settings_rows[] = {
	&row_color, &row_blend, &row_grid, &row_grid_dim, &row_grid_boost,
	&row_debug, &row_back,
};

/* Both pages share the menu; only one is open at a time. */
static struct menu st;
static int st_ingame;

static void settings_global_enter(const char *from, int64_t now)
{
	(void)from;
	(void)now;
	st_ingame = 0;
	menu_open(&st, settings_rows, ARRAY_SIZE(settings_rows),
		  UI_ACT(UI_BACK));
}

static void settings_game_enter(const char *from, int64_t now)
{
	(void)from;
	(void)now;
	st_ingame = 1;
	menu_open(&st, game_settings_rows, ARRAY_SIZE(game_settings_rows),
		  UI_ACT(UI_BACK));
}

static struct ui_result settings_menu_update(uint32_t pressed, int64_t now)
{
	return menu_update(&st, pressed, now);
}

static void settings_menu_draw(struct canvas *c, const struct game_view *g)
{
	const struct blit_fx *fx = fx_current();

	if (st_ingame) {
		int ph = 2 * IG_PANEL_PAD + st.count * IG_ROW_H, py = c->h - ph;

		draw_fill(c, 0, 0, c->w, c->h, 0);
		if (g->px)
			draw_image_fx(c, (c->w - 2 * g->w) / 2,
				      (c->h - 2 * g->h) / 2, g->px, g->pitch,
				      g->w, g->h, fx);
		draw_fill(c, 0, py, c->w, ph, COL_BOX);
		draw_fill(c, 0, py, c->w, 2, COL_SEL);
		menu_draw_rows(c, &st, 0,
			       py + IG_PANEL_PAD + (IG_ROW_H - FONT_H) / 2, c->w,
			       IG_ROW_H, 1);
		return;
	}
	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	draw_text(c, 8, HEAD_Y, COL_HI, "Settings");
	draw_battery(c, c->w - 8, HEAD_Y);
	draw_fill(c, 0, HEAD_LINE_Y, c->w, 2, COL_SEL);
	menu_draw_rows(c, &st, 0, LIST_Y, c->w, ROW_H, 1);
	draw_text(c, 8, FOOT_Y, COL_DIM, "B:back");
}

/* The ROM list's (global) settings and the in-game ones (over the game,
 * with the effects applied as a live preview). */
const struct page settings_page = {
	"settings", settings_global_enter, settings_menu_update,
	settings_menu_draw, 0,
};
const struct page game_settings_page = {
	"in-game settings", settings_game_enter, settings_menu_update,
	settings_menu_draw, 0,
};

void message_draw(struct canvas *c, const char *title, const char *body,
		  uint16_t col)
{
	/* The body wrapped on spaces and newlines to the width less 16 px
	 * margins, MSG_LINE px apart, above the POWER hint. */
	const int cols = (c->w - 32) / FONT_W, foot = c->h - MSG_FOOT;
	int y = MSG_BODY_Y;

	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	draw_text(c, 16, MSG_TITLE_Y, col, title);
	while (body && *body && y + FONT_H <= foot - 4) {
		char line[CANVAS_W / FONT_W + 1];
		int n = 0, brk = -1;

		while (body[n] && body[n] != '\n' && n < cols &&
		       n < (int)sizeof(line) - 1) {
			if (body[n] == ' ')
				brk = n;
			n++;
		}
		if (body[n] && body[n] != '\n' && brk > 0)
			n = brk;
		snprintf(line, sizeof(line), "%.*s", n, body);
		draw_text(c, 16, y, COL_FG, line);
		body += n;
		while (*body == ' ' || *body == '\n')
			body++;
		y += MSG_LINE;
	}
	draw_text(c, 16, foot, COL_DIM, "POWER: power off");
}
