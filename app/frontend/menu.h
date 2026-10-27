#ifndef MENU_H
#define MENU_H

#include <stddef.h>
#include <stdint.h>

#include "draw.h"

/*
 * Layout of the menu screens (menu.c, sysinfo.c, clock.c), from the font
 * size: the title at HEAD_Y with a line under it at HEAD_LINE_Y, the key
 * hints at FOOT_Y, lists from LIST_Y with ROW_H px rows (the selection
 * bar ROW_PAD above the text), LIST_ROWS of them on a page.
 */
#define HEAD_Y      3
#define HEAD_LINE_Y (HEAD_Y + FONT_H + 3)
#define FOOT_Y      (CANVAS_H - FONT_H - 2)
#define LIST_Y      (HEAD_LINE_Y + 8)
#define ROW_H       (FONT_H + 4)
#define ROW_PAD     ((ROW_H - FONT_H) / 2)
#define LIST_ROWS   ((FOOT_Y - 2 - (LIST_Y - ROW_PAD)) / ROW_H)
/* Left margin and the columns of a ROM name in the list. */
#define LIST_X      8
#define LIST_COLS   ((CANVAS_W - 2 * LIST_X) / FONT_W)

/* The in-game menu: a box with the game's name and the battery in its
 * IG_HEAD_H px top, then IG_ROWS rows IG_ROW_H px apart and IG_TAIL px
 * under them, centered on the screen. */
#define IG_ROWS     11
#define IG_BOX_X    40
#define IG_BOX_W    (CANVAS_W - 2 * IG_BOX_X)
#define IG_HEAD_H   (FONT_H + 8)
#define IG_ROW_H    (FONT_H + 1)
#define IG_TAIL     5
#define IG_BOX_Y    ((CANVAS_H - IG_BOX_H) / 2)
#define IG_BOX_H    (IG_HEAD_H + IG_ROWS * IG_ROW_H + IG_TAIL)

enum ui_action {
	UI_NONE,
	UI_START_ROM,
	UI_RESUME,
	UI_QUIT,
	UI_POWEROFF,  /* confirmed */
	UI_SETTINGS,  /* open the settings screen */
	UI_BACK,      /* leave the settings screen */
	UI_RESTART,   /* restart the game with rom_settings.core (confirmed) */
	UI_RESET,     /* start the game over (confirmed) */
	UI_SYSINFO,   /* open the system info page (sysinfo.c) */
	UI_DATETIME,  /* open the Date & time page (clock.c) */
};

/* Button edges with key repeat for the d-pad. */
uint32_t ui_repeat(uint32_t held, int64_t now);

void romlist_menu_enter(void);
int romlist_menu_update(uint32_t pressed, char *path, size_t size);
void romlist_menu_draw(struct canvas *c);
void romlist_menu_message(const char *msg);

void ingame_menu_enter(const char *rom_name);
int ingame_menu_update(uint32_t pressed);
void ingame_menu_draw(struct canvas *c, const uint16_t *frame, int pitch,
		      int w, int h);

/* Settings: global ones from the ROM list, grid and the game's color
 * correction in game (drawn over the game picture as a live preview). */
void settings_menu_enter(int ingame);
int settings_menu_update(uint32_t pressed);
void settings_menu_draw(struct canvas *c, const uint16_t *frame, int pitch,
			int w, int h);

/* A ROM name in cols columns: cut with "~" (" *" kept as "~*"),
 * or scrolled offset pixels to the left and clipped to the columns. */
void draw_rom_name(struct canvas *c, int x, int y, uint16_t col,
		   const char *name, int cols, int offset);
/* The scroll offset in pixels of the selected name, excess pixels wider
 * than its row, frames after it was selected (see menu.c). */
int marquee_offset(int excess, unsigned frames);

void message_draw(struct canvas *c, const char *title, const char *body,
		  uint16_t col);

#endif
