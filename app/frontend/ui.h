/*
 * The screens as a stack of pages (ui.c). Each page is a struct page;
 * its update returns a struct ui_result: stay, push another page, go
 * back (pop), or an action that main.c carries out (start a game, quit
 * to the ROM list, power off, ...). A page is drawn only when something
 * on it changed; it says so with ui_redraw and the times it needs with
 * ui_redraw_at and ui_wake_at, which main.c sleeps until (ui_due).
 */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <time.h>

struct canvas;

enum ui_action {
	UI_NONE,
	UI_PUSH,      /* open result.page over this one */
	UI_BACK,      /* back to the page below */
	UI_START_ROM, /* start romlist_menu_rom() */
	UI_RESUME,    /* back into the game (the pages over it closed) */
	UI_QUIT,
	UI_POWEROFF,  /* confirmed */
	UI_RESTART,   /* restart the game with rom_settings.core (confirmed) */
	UI_RESET,     /* start the game over (confirmed) */
};

struct page;

struct ui_result {
	enum ui_action act;
	const struct page *page;  /* UI_PUSH */
};

#define UI_STAY       ((struct ui_result){ UI_NONE, 0 })
#define UI_ACT(a)     ((struct ui_result){ (a), 0 })
#define UI_OPEN(p)    ((struct ui_result){ UI_PUSH, (p) })

/* The paused game's last frame for pages over it, px NULL otherwise. */
struct game_view {
	const uint16_t *px;
	int pitch, w, h;
};

struct page {
	const char *name;  /* in the log */
	/* On a push or reset; from names the page below (NULL for none). */
	void (*enter)(const char *from, int64_t now);
	/* With the newly pressed buttons (key repeat on the d-pad), or for
	 * the game the held ones; NULL: nothing to do. The game's runs once
	 * per vblank; a menu's whenever ui_frame runs, which is once per
	 * vblank while it is drawn and otherwise on input or the times it
	 * asks for (ui_redraw_at, ui_wake_at), so its timing goes by now. */
	struct ui_result (*update)(uint32_t pressed, int64_t now);
	/* Into the canvas, which main.c then shows; NULL for the game, which
	 * presents its own frame. Shows the state as update (or enter, for a
	 * page just opened) left it, changing nothing. */
	void (*draw)(struct canvas *c, const struct game_view *game);
	int game;  /* the running game: pages over it are over a paused game */
};

/* main.c's part of an action, after ui.c changed the stack (a push, a
 * pop, back to the game); from is the page that asked. */
typedef void (*ui_handler)(enum ui_action act, const struct page *from);
void ui_set_handler(ui_handler fn);

/* The stack: only p (startup, a game started or quit, the error and
 * power off screens), one more on top, one less, down to the game. */
void ui_reset(const struct page *p, int64_t now);
void ui_push(const struct page *p, int64_t now);
void ui_pop(void);
void ui_to_game(void);
const struct page *ui_top(void);
/* A game page is on the stack, under the top or the top itself. */
int ui_in_game(void);

/* One frame: the top page updated with buttons (held, as input.c has
 * them), the result applied; SELECT+START over a paused game resumes it.
 * The top page is drawn into c only when something changed: a key press
 * (newly pressed, or repeated), a page change, ui_redraw, or a time from
 * ui_redraw_at that has come. Returns 1 when it was drawn. */
int ui_frame(struct canvas *c, uint32_t buttons, int64_t now);
/* Draw on the next ui_frame: something shown changed outside a key
 * press (a name, the battery, the volume, a timeout). */
void ui_redraw(void);
/* From a page's update: draw at t (CLOCK_MONOTONIC ns), or just run
 * ui_frame again by t (a marquee step, a timeout, background work, key
 * repeat). Asked again on every update, the earliest kept. */
void ui_redraw_at(int64_t t);
void ui_wake_at(int64_t t);
/* When ui_frame is needed next with no key pressed: <= now when a
 * redraw is pending, INT64_MAX when nothing is waiting. */
int64_t ui_due(void);
/* Just past the next time clk (CLOCK_REALTIME, CLOCK_BOOTTIME) reaches
 * a multiple of period_s seconds, on now's clock: for a clock shown. */
int64_t ui_clock_tick(int64_t now, clockid_t clk, int period_s);
/* Held buttons ignored until released (and not a new press for the
 * hotkey), after a page change outside ui_frame. */
void ui_suppress(uint32_t held);
/* SELECT+START (one of them newly pressed) or Esc/F1, this frame. */
int ui_hotkey(void);

/* Button edges with key repeat for the d-pad (and a wake for the next
 * repeat while one is held). */
uint32_t ui_repeat(uint32_t held, int64_t now);

#endif
