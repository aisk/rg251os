#ifndef MENU_H
#define MENU_H

#include <stddef.h>
#include <stdint.h>

#include "draw.h"
#include "ui.h"

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

/* The menu pages (menu.c): the ROM list, the in-game menu, and the
 * settings, global ones from the ROM list, grid and the game's color
 * correction in game (drawn over the game picture as a live preview). */
extern const struct page romlist_page;
extern const struct page ingame_page;
extern const struct page settings_page;
extern const struct page game_settings_page;

/* The ROM path after UI_START_ROM from the ROM list. */
const char *romlist_menu_rom(void);
/* A message (an error) in the ROM list's footer instead of the key
 * hints until a key acts there, "" or NULL for none. */
void romlist_menu_message(const char *msg);
/* The game's name shown in the in-game menu. */
void ingame_menu_set_name(const char *rom_name);

/* A ROM name in cols columns: cut with "~" (" *" kept as "~*"),
 * or scrolled offset pixels to the left and clipped to the columns. */
void draw_rom_name(struct canvas *c, int x, int y, uint16_t col,
		   const char *name, int cols, int offset);
/* The scroll offset in pixels of the selected name, excess pixels wider
 * than its row, steps (60 a second) after it was selected (see menu.c). */
int marquee_offset(int excess, unsigned steps);

void message_draw(struct canvas *c, const char *title, const char *body,
		  uint16_t col);

#endif
