#ifndef MENU_H
#define MENU_H

#include <stddef.h>
#include <stdint.h>

#include "draw.h"

enum ui_action {
	UI_NONE,
	UI_START_ROM,
	UI_RESUME,
	UI_QUIT,
	UI_POWEROFF,
	UI_SETTINGS,  /* open the settings screen */
	UI_BACK,      /* leave the settings screen */
	UI_RESTART,   /* restart the game with rom_settings.core */
	UI_RESET,     /* start the game over (confirmed) */
	UI_SYSINFO,   /* open the system info page (sysinfo.c) */
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

/* A ROM name in cols columns at 2x: cut with "~" (" *" kept as "~*"),
 * or scrolled offset pixels to the left and clipped to the columns. */
void draw_rom_name(struct canvas *c, int x, int y, uint16_t col,
		   const char *name, int cols, int offset);
/* The scroll offset in pixels of the selected name, excess pixels wider
 * than its row, frames after it was selected (see menu.c). */
int marquee_offset(int excess, unsigned frames);

void message_draw(struct canvas *c, const char *title, const char *body,
		  uint16_t col);

#endif
