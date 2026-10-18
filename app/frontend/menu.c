/*
 * Menus on the 480x320 logical canvas, 8x8 font at 2x (30 columns).
 *
 * ROM list: up/down (left/right page), A or START starts, SELECT opens
 * the global settings (Volume, Brightness, CPU, LCD grid, Grid darkness,
 * Dot boost, Audio mode Simple/Adaptive, Audio test: A plays a 3 s
 * 440 Hz tone through the game's audio path, and Debug log on/off, see
 * log.c). In game, SELECT+START opens the in-game menu: Resume, Volume,
 * Brightness, CPU, Core, Settings, Quit to ROM list, Power off; B or
 * SELECT+START again resumes. CPU is the global CPU mode (Adaptive, Max
 * or a fixed frequency, see cpuctl.c); changed in game it applies as soon
 * as the game resumes. Core (this game: mGBA or gpSP) shows the running
 * core; left/right pick another one, the row then reads "Restart with"
 * and A saves and restarts the game with it (resuming instead keeps it
 * for the next start). The in-game Settings screen shows the game with
 * the effects applied (live preview) under a panel with Color (this
 * game), LCD grid, Grid darkness, Dot boost and Debug log. In menus,
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

static struct romlist roms;
static int sel, top;
static char message[96];

static struct {
	int sel;
	char name[64];
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

void romlist_menu_enter(void)
{
	char dir[300];
	int n;

	snprintf(dir, sizeof(dir), "%s/roms", fe_data_dir);
	n = romlist_scan(&roms, dir);
	log_msg("roms: %d in %s", n, dir);
	if (sel >= roms.count)
		sel = roms.count ? roms.count - 1 : 0;
}

int romlist_menu_update(uint32_t pressed, char *path, size_t size)
{
	if (!roms.count)
		pressed &= JOY_BIT(SELECT);
	if (pressed & JOY_BIT(UP))
		sel = sel > 0 ? sel - 1 : roms.count - 1;
	if (pressed & JOY_BIT(DOWN))
		sel = sel < roms.count - 1 ? sel + 1 : 0;
	if (pressed & JOY_BIT(LEFT))
		sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;
	if (pressed & JOY_BIT(RIGHT))
		sel = sel + LIST_ROWS < roms.count ? sel + LIST_ROWS : roms.count - 1;
	if (sel < top)
		top = sel;
	if (sel >= top + LIST_ROWS)
		top = sel - LIST_ROWS + 1;
	if (pressed & JOY_BIT(SELECT)) {
		message[0] = 0;
		return UI_SETTINGS;
	}
	if (pressed & (JOY_BIT(A) | JOY_BIT(START))) {
		snprintf(path, size, "%s/roms/%s", fe_data_dir, roms.names[sel]);
		message[0] = 0;
		return UI_START_ROM;
	}
	return UI_NONE;
}

static void draw_name(struct canvas *c, int x, int y, uint16_t col,
		      const char *name, int cols)
{
	char buf[64];
	size_t n = strlen(name);

	if (n > 4 && !strcasecmp(name + n - 4, ".gba"))
		n -= 4;
	if ((int)n > cols) {
		snprintf(buf, sizeof(buf), "%.*s~", cols - 1, name);
	} else {
		snprintf(buf, sizeof(buf), "%.*s", (int)n, name);
	}
	draw_text(c, x, y, 2, col, buf);
}

void romlist_menu_draw(struct canvas *c)
{
	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	draw_text(c, 8, 8, 2, COL_HI, "GBA");
	if (roms.count)
		draw_textf(c, 8 + 16 * 4, 8, 2, COL_DIM, "%d/%d", sel + 1,
			   roms.count);
	draw_battery(c, c->w - 8, 8);
	draw_fill(c, 0, 30, c->w, 2, COL_SEL);

	if (!roms.count) {
		draw_text(c, 16, 80, 2, COL_FG, "No ROMs found.");
		draw_text(c, 16, 120, 2, COL_DIM, "Copy .gba files to the");
		draw_text(c, 16, 144, 2, COL_DIM, "roms folder on the GBA");
		draw_text(c, 16, 168, 2, COL_DIM, "partition of the card.");
	}
	for (int i = 0; i < LIST_ROWS && top + i < roms.count; i++) {
		int y = LIST_Y + i * ROW_H;

		if (top + i == sel)
			draw_fill(c, 0, y - 1, c->w, ROW_H, COL_SEL);
		draw_name(c, 8, y, top + i == sel ? COL_FG : COL_DIM,
			  roms.names[top + i], COLS);
	}
	if (message[0])
		draw_text(c, 8, c->h - 18, 2, COL_ERR, message);
	else
		draw_text(c, 8, c->h - 18, 2, COL_DIM,
			  roms.count ? "A:start  SELECT:settings" :
			  "SELECT:settings");
}

/* -------------------------------------------------------- in-game menu */

enum {
	IG_RESUME, IG_VOLUME, IG_BRIGHTNESS, IG_CPU, IG_CORE, IG_SETTINGS,
	IG_QUIT, IG_POWEROFF, IG_COUNT
};

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

void ingame_menu_enter(const char *rom_name)
{
	ig.sel = 0;
	snprintf(ig.name, sizeof(ig.name), "%s", rom_name);
}

int ingame_menu_update(uint32_t pressed)
{
	int d = 0;

	if (pressed & JOY_BIT(UP))
		ig.sel = (ig.sel + IG_COUNT - 1) % IG_COUNT;
	if (pressed & JOY_BIT(DOWN))
		ig.sel = (ig.sel + 1) % IG_COUNT;
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
	if (pressed & (JOY_BIT(A) | JOY_BIT(START))) {
		switch (ig.sel) {
		case IG_RESUME:
			return UI_RESUME;
		case IG_CORE:
			if (core_pending())
				return UI_RESTART;
			core_step(1);
			break;
		case IG_SETTINGS:
			return UI_SETTINGS;
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
		"Resume", "Volume", "Brightness", "CPU", "Core", "Settings",
		"Quit to ROM list", "Power off",
	};
	int bx = 56, by = 48, bw = c->w - 112, bh = 40 + IG_COUNT * 28;

	draw_image_dim(c, frame, pitch, w, h);
	draw_battery(c, c->w - 8, 16);
	draw_fill(c, bx, by, bw, bh, COL_BOX);
	draw_frame(c, bx, by, bw, bh, COL_SEL);
	draw_name(c, bx + 12, by + 12, COL_HI, ig.name, (bw - 24) / 16);
	for (int i = 0; i < IG_COUNT; i++) {
		int y = by + 40 + i * 28;
		uint16_t col = i == ig.sel ? COL_FG : COL_DIM;

		if (i == ig.sel)
			draw_fill(c, bx + 4, y - 5, bw - 8, 26, COL_SEL);
		draw_text(c, bx + 16, y, 2, col, i == IG_CORE && core_pending() ?
			  "Restart with" : items[i]);
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
		}
	}
}

/* ------------------------------------------------------------ settings */

enum {
	SI_VOLUME, SI_BRIGHTNESS, SI_CPU, SI_COLOR, SI_GRID, SI_GRID_DIM,
	SI_GRID_BOOST, SI_AUDIO, SI_AUDIO_TEST, SI_DEBUG, SI_BACK,
};

static const char *const si_names[] = {
	"Volume", "Brightness", "CPU", "Color", "LCD grid", "Grid darkness",
	"Dot boost", "Audio", "Audio test", "Debug log", "Back",
};

static const uint8_t si_romlist[] = {
	SI_VOLUME, SI_BRIGHTNESS, SI_CPU, SI_GRID, SI_GRID_DIM, SI_GRID_BOOST,
	SI_AUDIO, SI_AUDIO_TEST, SI_DEBUG, SI_BACK,
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
		/* Applied when the next game (or test tone) starts. */
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
		if (item == SI_GRID || item == SI_COLOR || item == SI_AUDIO ||
		    item == SI_DEBUG)
			settings_change(item, 1);
		if (item == SI_AUDIO_TEST)
			audio_test_tone(3.0);  /* blocks for the tone */
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
		snprintf(buf, size, "%s", color_mode_labels[rom_settings.color]);
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
	case SI_AUDIO_TEST:
		snprintf(buf, size, "440Hz");
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
			draw_fill(c, x + 4, y - 4, w - 8, row_h, COL_SEL);
		draw_text(c, x + 12, y, 2, col, si_names[item]);
		if (item == SI_COLOR)
			draw_text(c, x + 12 + 16 * 6, y, 2,
				  i == st.sel ? COL_DIM : COL_OFF, "(game)");
		settings_value(item, v, sizeof(v));
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

/* Color bars and ramps to judge the effects without a game. */
static const uint16_t *test_pattern(int *w, int *h)
{
	static uint16_t px[240 * 56];
	static int done;
	static const uint16_t bars[8] = {
		0xffdf, 0xffc0, 0x07df, 0x07c0, 0xf81f, 0xf800, 0x001f, 0x0000,
	};

	*w = 240;
	*h = 56;
	if (done)
		return px;
	for (int y = 0; y < 56; y++)
		for (int x = 0; x < 240; x++) {
			int v = x * 32 / 240;  /* 0..31 */
			uint16_t p;

			if (y < 28)
				p = bars[x / 30];
			else if (y < 35)
				p = (uint16_t)(v << 11 | v << 6 | v);
			else if (y < 42)
				p = (uint16_t)(v << 11);
			else if (y < 49)
				p = (uint16_t)(v << 6);
			else
				p = (uint16_t)v;
			px[y * 240 + x] = p;
		}
	done = 1;
	return px;
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
		settings_rows(c, 0, py + 10, c->w, 22);
		return;
	}
	draw_fill(c, 0, 0, c->w, c->h, COL_BG);
	draw_text(c, 8, 8, 2, COL_HI, "Settings");
	draw_battery(c, c->w - 8, 8);
	draw_fill(c, 0, 30, c->w, 2, COL_SEL);
	settings_rows(c, 0, 40, c->w, 17);
	frame = test_pattern(&w, &h);
	draw_image_fx(c, 0, c->h - 2 * h, frame, w, w, h, fx);
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
