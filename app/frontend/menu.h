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

void message_draw(struct canvas *c, const char *title, const char *body,
		  uint16_t col);

#endif
