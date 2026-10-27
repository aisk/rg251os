/*
 * The page stack behind every screen (ui.h): the ROM list or the game at
 * the bottom, the menus pushed over them (in-game menu, settings, system
 * info, date and time), the error and power off screens alone. Each frame
 * the top page is updated and its result applied here (push, pop, back
 * down to the game when SELECT+START or Resume closes the menus over it),
 * then main.c's handler does the rest of an action; the new top page is
 * drawn with the paused game's last frame when a game is below. Every
 * change is logged as "ui: ..." with debug logging. Also the d-pad key
 * repeat of the menus (ui_repeat).
 */
#include "ui.h"

#include "core.h"
#include "fe.h"
#include "input.h"

#define HOTKEY (JOY_BIT(SELECT) | JOY_BIT(START))
#define DEPTH 6

static struct {
	const struct page *stack[DEPTH];
	int n;
	ui_handler handler;
	uint32_t cur, prev;  /* buttons this frame and the last one */
	uint32_t suppress;   /* held buttons hidden until released */
} u;

void ui_set_handler(ui_handler fn)
{
	u.handler = fn;
}

const struct page *ui_top(void)
{
	return u.n ? u.stack[u.n - 1] : NULL;
}

int ui_in_game(void)
{
	for (int i = 0; i < u.n; i++)
		if (u.stack[i]->game)
			return 1;
	return 0;
}

void ui_reset(const struct page *p, int64_t now)
{
	log_verbose("ui: %s", p->name);
	u.n = 0;
	u.stack[u.n++] = p;
	if (p->enter)
		p->enter(NULL, now);
}

void ui_push(const struct page *p, int64_t now)
{
	const struct page *from = ui_top();

	if (u.n == DEPTH) {
		log_msg("ui: cannot open %s over %s, too deep", p->name, from->name);
		return;
	}
	log_verbose("ui: push %s over %s", p->name, from ? from->name : "nothing");
	u.stack[u.n++] = p;
	if (p->enter)
		p->enter(from ? from->name : NULL, now);
}

void ui_pop(void)
{
	if (u.n < 2)
		return;
	u.n--;
	log_verbose("ui: pop %s, back to %s", u.stack[u.n]->name,
		    u.stack[u.n - 1]->name);
}

void ui_to_game(void)
{
	int n = u.n;

	while (n > 1 && !u.stack[n - 1]->game)
		n--;
	if (n == u.n || !u.stack[n - 1]->game)
		return;
	log_verbose("ui: back to %s from %s", u.stack[n - 1]->name,
		    u.stack[u.n - 1]->name);
	u.n = n;
}

void ui_suppress(uint32_t held)
{
	u.suppress = held;
	u.prev = held;
}

int ui_hotkey(void)
{
	return ((u.cur & HOTKEY) == HOTKEY && (~u.prev & HOTKEY)) ||
	       ((u.cur & BTN_BIT_MENU) && !(u.prev & BTN_BIT_MENU));
}

int ui_frame(struct canvas *c, uint32_t buttons, int64_t now)
{
	const struct page *p = ui_top();
	struct ui_result r = UI_STAY;
	struct game_view g = { 0 };

	u.cur = buttons;
	u.suppress &= buttons;
	if (p->game) {
		r = p->update(buttons & ~u.suppress, now);
	} else if (p->update) {
		uint32_t pressed = ui_repeat(buttons & ~u.suppress, now);

		r = ui_in_game() && ui_hotkey() ? UI_ACT(UI_RESUME) :
		    p->update(pressed, now);
	}
	if (r.act != UI_NONE) {
		u.suppress = buttons;
		if (r.act == UI_PUSH)
			ui_push(r.page, now);
		else if (r.act == UI_BACK)
			ui_pop();
		else if (r.act == UI_RESUME)
			ui_to_game();
		if (u.handler)
			u.handler(r.act, p);
	}
	u.prev = buttons;

	p = ui_top();
	if (!p->draw)
		return 0;
	if (ui_in_game() && core_loaded())
		g.px = core_frame(&g.w, &g.h, &g.pitch);
	p->draw(c, &g);
	return 1;
}

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
