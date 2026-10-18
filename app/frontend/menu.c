/*
 * Menus on the 480x320 logical canvas, 8x8 font at 2x (30 columns).
 *
 * ROM list: up/down (left/right page), A starts. In game, SELECT+START
 * opens the in-game menu: Resume, Volume, Brightness, Quit to ROM list,
 * Power off. B or SELECT+START again resumes.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "fe.h"
#include "libretro.h"
#include "menu.h"
#include "romlist.h"

#define BIT(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)
#define ROW_H 18
#define LIST_Y 40
#define LIST_ROWS 14
#define COLS 29

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
	const uint32_t dirs = BIT(UP) | BIT(DOWN) | BIT(LEFT) | BIT(RIGHT);
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
		return UI_NONE;
	if (pressed & BIT(UP))
		sel = sel > 0 ? sel - 1 : roms.count - 1;
	if (pressed & BIT(DOWN))
		sel = sel < roms.count - 1 ? sel + 1 : 0;
	if (pressed & BIT(LEFT))
		sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;
	if (pressed & BIT(RIGHT))
		sel = sel + LIST_ROWS < roms.count ? sel + LIST_ROWS : roms.count - 1;
	if (sel < top)
		top = sel;
	if (sel >= top + LIST_ROWS)
		top = sel - LIST_ROWS + 1;
	if (pressed & (BIT(A) | BIT(START))) {
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
		draw_textf(c, c->w - 8 - 16 * 9, 8, 2, COL_DIM, "%4d/%-4d",
			   sel + 1, roms.count);
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
			  roms.count ? "A:start  POWER:off" : "POWER:off");
}

/* -------------------------------------------------------- in-game menu */

enum { IG_RESUME, IG_VOLUME, IG_BRIGHTNESS, IG_QUIT, IG_POWEROFF, IG_COUNT };

void ingame_menu_enter(const char *rom_name)
{
	ig.sel = 0;
	snprintf(ig.name, sizeof(ig.name), "%s", rom_name);
}

int ingame_menu_update(uint32_t pressed)
{
	int d = 0;

	if (pressed & BIT(UP))
		ig.sel = (ig.sel + IG_COUNT - 1) % IG_COUNT;
	if (pressed & BIT(DOWN))
		ig.sel = (ig.sel + 1) % IG_COUNT;
	if (pressed & BIT(LEFT))
		d = -1;
	if (pressed & BIT(RIGHT))
		d = 1;
	if (pressed & BIT(B))
		return UI_RESUME;

	if (d && ig.sel == IG_VOLUME) {
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
	if (d && ig.sel == IG_BRIGHTNESS) {
		int b = settings.brightness + d * 10;

		settings.brightness = b < 10 ? 5 : b > 100 ? 100 : b;
		backlight_set(settings.brightness);
	}
	if (pressed & (BIT(A) | BIT(START))) {
		switch (ig.sel) {
		case IG_RESUME:
			return UI_RESUME;
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
		"Resume", "Volume", "Brightness", "Quit to ROM list", "Power off",
	};
	int bx = 56, by = 48, bw = c->w - 112, bh = 40 + IG_COUNT * 30;

	draw_image_dim(c, frame, pitch, w, h);
	draw_fill(c, bx, by, bw, bh, COL_BOX);
	draw_frame(c, bx, by, bw, bh, COL_SEL);
	draw_name(c, bx + 12, by + 12, COL_HI, ig.name, (bw - 24) / 16);
	for (int i = 0; i < IG_COUNT; i++) {
		int y = by + 40 + i * 30;
		uint16_t col = i == ig.sel ? COL_FG : COL_DIM;

		if (i == ig.sel)
			draw_fill(c, bx + 4, y - 5, bw - 8, 26, COL_SEL);
		draw_text(c, bx + 16, y, 2, col, items[i]);
		if (i == IG_VOLUME) {
			if (settings.volume == 0)
				draw_text(c, bx + bw - 16 - 16 * 6, y, 2, col, "  mute");
			else
				draw_textf(c, bx + bw - 16 - 16 * 6, y, 2, col,
					   "%4ddB", -(255 - settings.volume) * 3 / 8);
		} else if (i == IG_BRIGHTNESS) {
			draw_textf(c, bx + bw - 16 - 16 * 6, y, 2, col, "%5d%%",
				   settings.brightness);
		}
	}
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
