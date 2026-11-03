/* Deterministic fighter rules and the arcade ladder: movement, prejump,
 * jumps, crouching, data-driven normals and specials with an input buffer,
 * motion and charge commands, cancels, projectiles with clashes, hurt/hit
 * boxes from the pack, hitstop, hitstun, blockstun, high/low/overhead guard,
 * pushback with corner transfer, KO, AI (with specials), the select, result
 * and clear screens and the stress scenes, plus the draw-command list (L
 * overlays the real boxes). Rules pick an action or move and its elapsed
 * ticks; the pack's steps map that onto frames, boxes and motion, so what is
 * drawn is what collides. Coordinates are integer pixels, jumps use
 * quarter-pixel vertical velocity.
 *
 * Timing, in ticks (one fight_step each): a move started on tick T shows
 * elapsed 0 on T and ends on tick T + total, when the fighter may act
 * again. A contact on tick C freezes both fighters for hitstop ticks
 * (C+1 .. C+hitstop); the defender is then stunned and acts again on tick
 * C + hitstop + hitstun (or blockstun). Pushback slides the defender
 * ceil(rest / 4) pixels per unfrozen tick; what a wall stops moves the
 * attacker back instead (not for projectile hits). A step's motion moves
 * the fighter on every unfrozen tick it shows, from the move's first tick;
 * a projectile launches on its step's first tick at the owner's anchor plus
 * its offset, moves speed pixels on every later tick, never freezes,
 * freezes only the defender on a hit and vanishes on a hit, a clash with
 * the other side's projectile, after lifetime ticks or 64 pixels off
 * screen. Up starts a JUMP_SQUAT-tick prejump; any move started in it
 * replaces the jump.
 *
 * Input: every tick (frozen or not) records the raw direction in a
 * HISTORY-tick history and updates the back/down charge counters. A button
 * press is resolved at once against the history (specials first, in pack
 * order, then the normal for the pressed button, crouching with down held)
 * and buffered: it starts as soon as the fighter can act, within
 * BUFFER_TICKS unfrozen ticks of the press (frozen ticks do not count, so a
 * press in hitstop survives it; a press in the last BUFFER_TICKS recovery
 * ticks comes out on the first free tick). A newer press replaces the
 * buffer unless that would trade a special for a normal. Motions are
 * matched backwards from the press, relative to the current facing: 236 and
 * 214 within WINDOW_QUARTER ticks, 623 within WINDOW_DRAGON; [4]6 and [2]8
 * need CHARGE_TICKS of back/down, released at most CHARGE_GRACE ticks ago
 * (or still held), and forward/up in the last CHARGE_RELEASE ticks. A
 * projectile special is not recognised while its owner has one alive.
 * Cancels: a normal whose hit connected (hit or block) can be cancelled
 * into a special (cancel "special") or another normal (cancel "chain")
 * while its elapsed is inside startup + active; the buffered move starts on
 * the first unfrozen tick, so a cancel from hitstop starts on
 * C + hitstop + 1.
 *
 * Ladder: the select screen picks P1 (d-pad, any attack confirms); each
 * stage is a match of ROUNDS_TO_WIN won rounds against a random character
 * not yet beaten in this run (a mirror with the alternate palette only for
 * a one-character roster). A loss shows the result screen, beating
 * everyone the clear screen; both return to select. Auto skips select and
 * rotates P1 through the roster each run. */
#include "fighter.h"
#include "libretro.h"
#include <stdlib.h>
#include <string.h>
#define KEY(k) (1u << RETRO_DEVICE_ID_JOYPAD_##k)
#define ATTACKS (KEY(A) | KEY(B) | KEY(X) | KEY(Y))
/* Normals by strength, strongest first; see libretro.c for the layout. */
static const uint32_t buttons[4] = {KEY(A), KEY(X), KEY(B), KEY(Y)};
static const int strength[4] = {STAND_HK, STAND_HP, STAND_LK, STAND_LP};
static const unsigned button_bit[4] = {BUTTON_HK, BUTTON_HP, BUTTON_LK, BUTTON_LP};
/* Holding back while the opponent's move or projectile is this close shows
 * the guard. */
#define GUARD_RANGE 160
/* Projectiles vanish this far beyond the screen edge. */
#define SHOT_MARGIN 64
/* KO, result and clear screens; Auto confirms them after AUTO_SCREEN. */
#define KO_TICKS 180
#define SCREEN_TICKS 180
#define AUTO_SCREEN 60
#define SCREEN_SKIP 20
static int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static uint32_t random_next(struct fight_state *s)
{
	s->rng ^= s->rng << 13;
	s->rng ^= s->rng >> 17;
	s->rng ^= s->rng << 5;
	return s->rng;
}
static void place(struct fight_state *s)
{
	memset(s->p, 0, sizeof(s->p));
	memset(s->shot, 0, sizeof(s->shot));
	s->p[0].x = 140;
	s->p[1].x = 340;
	s->p[0].hp = s->p[1].hp = 1000;
	s->p[0].face = 1;
	s->p[1].face = -1;
	s->ended = 0;
}
void fight_reset(struct fight_state *s, int automatic)
{
	memset(s, 0, sizeof(*s));
	s->rng = 0x351f123u;
	s->automatic = (uint32_t)automatic;
	s->mode = MODE_SELECT;
	s->pal[1] = 1;
	place(s);
}
void fight_start(struct fight_state *s, unsigned c0, unsigned c1)
{
	place(s);
	s->mode = MODE_FIGHT;
	s->timer = 0;
	s->chr[0] = c0;
	s->chr[1] = c1;
	s->pal[0] = 0;
	s->pal[1] = c0 == c1;
}
static int grounded(const struct fighter *p) { return !p->y && !p->vy; }
static uint32_t back_key(const struct fighter *p)
{
	return p->face > 0 ? KEY(LEFT) : KEY(RIGHT);
}
/* ---- input history, charge and motions ---- */
static unsigned direction(uint32_t keys) { return (keys >> RETRO_DEVICE_ID_JOYPAD_UP) & 15u; }
static unsigned history_at(const struct fighter *p, unsigned i)
{
	return (p->history[i / 8] >> (4 * (i % 8))) & 15u;
}
/* Numpad direction of raw bits (1 up, 2 down, 4 left, 8 right) for a facing. */
static int numpad(unsigned d, int face)
{
	int up = d & 1, down = (d >> 1) & 1, fwd = face > 0 ? (d >> 3) & 1 : (d >> 2) & 1,
	    back = face > 0 ? (d >> 2) & 1 : (d >> 3) & 1;
	if (up && down)
		up = down = 0;
	if (fwd && back)
		fwd = back = 0;
	return 5 + 3 * up - 3 * down + fwd - back;
}
/* Charge counters: byte 0 back held, 1 back grace, 2 down held, 3 down grace. */
static unsigned charge_byte(uint32_t c, int k) { return (c >> (8 * k)) & 255u; }
static uint32_t charge_update(uint32_t c, int which, int held)
{
	unsigned t = charge_byte(c, 2 * which), grace = charge_byte(c, 2 * which + 1);
	if (held) {
		t = t < 255 ? t + 1 : 255;
		grace = 0;
	} else {
		grace = t >= CHARGE_TICKS ? CHARGE_GRACE : grace ? grace - 1 : 0;
		t = 0;
	}
	c &= ~(0xffffu << (16 * which));
	return c | (t | grace << 8) << (16 * which);
}
static void record(struct fighter *p, uint32_t keys)
{
	unsigned d = direction(keys);
	for (int w = HISTORY / 8 - 1; w > 0; w--)
		p->history[w] = p->history[w] << 4 | p->history[w - 1] >> 28;
	p->history[0] = p->history[0] << 4 | d;
	int n = numpad(d, p->face);
	p->charge = charge_update(p->charge, 0, n == 1 || n == 4 || n == 7);
	p->charge = charge_update(p->charge, 1, n == 1 || n == 2 || n == 3);
}
static int charged(uint32_t c, int which)
{
	return charge_byte(c, 2 * which) >= CHARGE_TICKS || charge_byte(c, 2 * which + 1);
}
int fight_motion(const struct fighter *p, int motion)
{
	static const int seq[3][3] = {{2, 3, 6}, {2, 1, 4}, {6, 2, 3}};
	if (motion == CHARGE_46 || motion == CHARGE_28) {
		if (!charged(p->charge, motion == CHARGE_28))
			return 0;
		for (unsigned i = 0; i < CHARGE_RELEASE; i++) {
			int n = numpad(history_at(p, i), p->face);
			if (motion == CHARGE_46 ? n % 3 == 0 : n >= 7)
				return 1;
		}
		return 0;
	}
	if (motion < 0 || motion >= MOTIONS)
		return 0;
	unsigned window = motion == MOTION_623 ? WINDOW_DRAGON : WINDOW_QUARTER;
	int k = 2;
	for (unsigned i = 0; i < window && k >= 0; i++)
		if (numpad(history_at(p, i), p->face) == seq[motion][k])
			k--;
	return k < 0;
}
/* ---- poses ---- */
/* Step index for an animation after t ticks; looping wraps, others hold.
 * start receives the tick the step began on. */
static unsigned step_at(const struct assets *a, const struct anim *an, uint32_t t, int loop,
			uint32_t *start)
{
	t = loop ? t % an->total : t >= an->total ? an->total - 1u : t;
	uint32_t at = 0;
	for (unsigned i = 0; i < an->count; i++) {
		const struct step *st = &a->step[an->first + i];
		if (t < at + st->ticks) {
			if (start)
				*start = at;
			return an->first + i;
		}
		at += st->ticks;
	}
	if (start)
		*start = at - a->step[an->first + an->count - 1u].ticks;
	return an->first + an->count - 1u;
}
static const struct character *who(const struct fight_state *s, const struct assets *a, int i)
{
	return &a->chr[s->chr[i] < a->characters ? s->chr[i] : 0];
}
unsigned fight_pose(const struct fight_state *s, const struct assets *a, int i)
{
	const struct fighter *p = &s->p[i];
	const struct character *c = who(s, a, i);
	if (!p->hp)
		return step_at(a, &c->action[KO], s->ended, 0, NULL);
	if (p->move)
		return step_at(a, &c->move[p->move - 1].anim, (uint32_t)p->elapsed, 0, NULL);
	int low = p->crouch && grounded(p);
	if (p->stun)
		return step_at(a,
			       &c->action[p->block ? (low ? CROUCH_GUARD : GUARD)
						   : (low ? CROUCH_HURT : HURT)],
			       (uint32_t)p->elapsed, 0, NULL);
	if (p->squat)
		return step_at(a, &c->action[JUMP], 0, 0, NULL);
	if (!grounded(p))
		return step_at(a, &c->action[JUMP], (uint32_t)(p->vy + 48) / 3, 0, NULL);
	if (p->block)
		return step_at(a, &c->action[low ? CROUCH_GUARD : GUARD],
			       (uint32_t)p->elapsed, 0, NULL);
	if (p->crouch)
		return step_at(a, &c->action[CROUCH], s->tick, 1, NULL);
	if (p->walk)
		return step_at(a, &c->action[p->walk > 0 ? WALK_FORWARD : WALK_BACK],
			       s->tick, 1, NULL);
	return step_at(a, &c->action[IDLE], s->tick + (unsigned)i * 17, 1, NULL);
}
/* Height of a move's motion after showing elapsed 0..t. */
static int height(const struct assets *a, const struct move *m, uint32_t t)
{
	int y = 0;
	uint32_t at = 0;
	for (unsigned i = 0; i < m->anim.count && at <= t; i++) {
		const struct step *st = &a->step[m->anim.first + i];
		uint32_t n = t + 1 - at < st->ticks ? t + 1 - at : st->ticks;
		y += st->vy * (int)n;
		at += st->ticks;
	}
	return y;
}
struct area fight_box(const struct box *b, int x, int y, int face)
{
	return (struct area){face > 0 ? x + b->x : x - b->x - b->w, GROUND + y + b->y,
			     b->w, b->h};
}
static int overlap(struct area p, struct area q)
{
	return p.x < q.x + q.w && q.x < p.x + p.w && p.y < q.y + q.h && q.y < p.y + p.h;
}
static const struct projectile *shot_type(const struct fight_state *s, const struct assets *a,
					  const struct shot *sh)
{
	return &who(s, a, sh->owner)->projectile[sh->kind - 1];
}
static unsigned shot_step(const struct fight_state *s, const struct assets *a,
			  const struct shot *sh)
{
	return step_at(a, &shot_type(s, a, sh)->anim, (uint32_t)sh->age, 1, NULL);
}
static int owns_shot(const struct fight_state *s, int i)
{
	for (int k = 0; k < MAX_PROJECTILES; k++)
		if (s->shot[k].kind && s->shot[k].owner == i)
			return 1;
	return 0;
}
/* ---- AI ---- */
static uint32_t button_key(unsigned bits)
{
	for (int k = 3; k >= 0; k--)
		if (bits & button_bit[k])
			return buttons[k];
	return 0;
}
/* The AI's input for tick t of a special's motion, or 0 when it is done. */
static uint32_t macro(int motion, uint32_t t, int face, uint32_t button)
{
	uint32_t fwd = face > 0 ? KEY(RIGHT) : KEY(LEFT), back = face > 0 ? KEY(LEFT) : KEY(RIGHT),
		 down = KEY(DOWN);
	if (motion == CHARGE_46 || motion == CHARGE_28) {
		uint32_t hold = motion == CHARGE_46 ? back : down,
			 release = motion == CHARGE_46 ? fwd : KEY(UP);
		return t < CHARGE_TICKS + 2 ? hold : t == CHARGE_TICKS + 2 ? release | button : 0;
	}
	static const int seq[3][3] = {{2, 3, 6}, {2, 1, 4}, {6, 2, 3}};
	if (t >= 5)
		return 0;
	int n = seq[motion][t / 2];
	uint32_t key = (n <= 3 ? down : 0) | (n % 3 == 0 ? fwd : n % 3 == 1 ? back : 0);
	return t == 4 ? key | button : key;
}
static uint32_t ai(struct fight_state *s, const struct assets *a, int i)
{
	struct fighter *p = &s->p[i];
	const struct fighter *q = &s->p[1 - i];
	const struct character *c = who(s, a, i), *oc = who(s, a, 1 - i);
	int d = q->x - p->x, dist = abs(d);
	uint32_t toward = d > 0 ? KEY(RIGHT) : KEY(LEFT),
		 away = d > 0 ? KEY(LEFT) : KEY(RIGHT);
	uint32_t r = random_next(s);
	if (p->stun || !q->hp)
		p->plan = p->plan_tick = 0;
	if (p->plan) {
		const struct move *m = &c->move[p->plan - 1];
		uint32_t key = macro(m->motion, (uint32_t)p->plan_tick, p->face,
				     button_key(m->buttons));
		if (++p->plan_tick > CHARGE_TICKS + 3 || !key)
			p->plan = p->plan_tick = 0;
		if (key)
			return key;
	}
	/* Guard about two swings in three, at the wrong height one time in
	 * five; the choice is fixed for the whole swing. Projectiles in range
	 * are guarded the same way. */
	if (q->move && dist < GUARD_RANGE) {
		const struct move *m = &oc->move[q->move - 1];
		uint32_t h = ((s->tick - (uint32_t)q->elapsed) * 2654435761u + (unsigned)i) >> 27;
		if (h < 22) {
			int low = m->hit.guard == GUARD_LOW;
			if (h < 4)
				low = !low;
			return away | (low ? KEY(DOWN) : 0u);
		}
	}
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &s->shot[k];
		if (sh->kind && sh->owner != i && abs(sh->x - p->x) < GUARD_RANGE &&
		    (p->x - sh->x) * sh->face > 0 &&
		    (((s->tick - (uint32_t)sh->age) * 2654435761u + (unsigned)i) >> 27) < 22)
			return away;
	}
	/* Now and then a special: the projectile from afar, else any. */
	unsigned specials = c->moves - MOVES;
	if (specials && !p->move && !p->stun && grounded(p) && !((r >> 8) & 63)) {
		unsigned k = MOVES + (r >> 16) % specials;
		for (unsigned j = MOVES; dist > 180 && j < c->moves; j++)
			if (c->move[j].spawn)
				k = j;
		if (!(c->move[k].spawn && owns_shot(s, i))) {
			p->plan = (int32_t)k + 1;
			p->plan_tick = 1;
			return macro(c->move[k].motion, 0, p->face, button_key(c->move[k].buttons));
		}
	}
	/* Each 29-tick window plans one normal: walk in until its hitbox can
	 * reach the opponent's body, then press it once. */
	unsigned window = (s->tick + (unsigned)i * 17) / 29, plan = window * 2654435761u >> 24;
	int button = (int)(plan & 3), low = (int)(plan >> 2) & 1;
	const struct move *m = &c->move[strength[button] + (low ? CROUCH_LP : 0)];
	uint32_t key = dist > m->reach + 20 ? toward : 0;
	if (!key && !p->move && !p->stun && !(r & 7))
		key = buttons[button] | (low ? KEY(DOWN) : 0u);
	if ((r & 255) == 0)
		key |= KEY(UP);
	return key;
}
/* ---- fighter update ---- */
static void age(struct fighter *p)
{
	if (p->elapsed < 255)
		p->elapsed++;
}
/* Pushback: ceil(rest / 4) pixels this tick; with corner set a wall passes
 * the remainder to the other fighter (the attacker) the opposite way. */
static void slide(struct fighter *p, struct fighter *q)
{
	int dir = p->slide > 0 ? 1 : -1, d = (abs(p->slide) + 3) / 4;
	p->slide -= dir * d;
	int x = clamp(p->x + dir * d, WALL_LEFT, WALL_RIGHT), rest = d - abs(x - p->x);
	p->x = x;
	if (rest && p->corner)
		q->x = clamp(q->x - dir * rest, WALL_LEFT, WALL_RIGHT);
	if (!p->slide)
		p->corner = 0;
}
/* The move a press of edge buttons (with keys held) asks for, or -1. */
static int resolve(const struct fight_state *s, const struct assets *a, int i, uint32_t edge,
		   uint32_t keys)
{
	const struct character *c = who(s, a, i);
	unsigned pressed = 0;
	for (int k = 0; k < 4; k++)
		if (edge & buttons[k])
			pressed |= button_bit[k];
	if (!pressed)
		return -1;
	for (unsigned k = MOVES; k < c->moves; k++) {
		const struct move *m = &c->move[k];
		if ((m->buttons & pressed) && fight_motion(&s->p[i], m->motion) &&
		    !(m->spawn && owns_shot(s, i)))
			return (int)k;
	}
	for (int k = 0; k < 4; k++)
		if (edge & buttons[k])
			return strength[k] + (keys & KEY(DOWN) ? CROUCH_LP : 0);
	return -1;
}
static void start(struct fight_state *s, const struct character *c, struct fighter *p, int k)
{
	p->move = k + 1;
	p->elapsed = p->hit = p->block = p->squat = p->walk = 0;
	p->buffer = p->age = 0;
	p->crouch = k >= CROUCH_LP && k <= CROUCH_HK;
	if (c->move[k].special)
		s->specials++;
}
/* Can the connected current move be cancelled into move k now? */
static int cancellable(const struct character *c, const struct fighter *p, int k)
{
	const struct move *m = &c->move[p->move - 1];
	unsigned need = c->move[k].special ? CANCEL_SPECIAL : CANCEL_CHAIN;
	return p->hit && !m->special && (m->cancel & need) &&
	       p->elapsed < (int)m->startup + m->active;
}
static void launch(struct fight_state *s, const struct character *c, int i, unsigned kind)
{
	const struct fighter *p = &s->p[i];
	const struct projectile *t = &c->projectile[kind - 1];
	for (int k = 0; k < MAX_PROJECTILES; k++)
		if (!s->shot[k].kind) {
			s->shot[k] = (struct shot){(int32_t)kind, i, p->x + p->face * t->x,
						   p->y + t->y, p->face, 0};
			return;
		}
}
/* One fighter's tick before contacts; returns 1 if it was frozen. */
static int update(struct fight_state *s, const struct assets *a, int i, uint32_t keys,
		  int threat)
{
	struct fighter *p = &s->p[i], *q = &s->p[1 - i];
	const struct character *c = who(s, a, i);
	uint32_t edge = keys & ~(uint32_t)p->keys;
	p->keys = (int32_t)(keys & 0xffffu);
	record(p, keys);
	int k = edge & ATTACKS ? resolve(s, a, i, edge, keys) : -1;
	if (k >= 0 && !(p->buffer && c->move[p->buffer - 1].special && !c->move[k].special)) {
		p->buffer = k + 1;
		p->age = 0;
	}
	if (p->freeze) {
		p->freeze--;
		return 1;
	}
	if (p->move && p->buffer && cancellable(c, p, p->buffer - 1))
		start(s, c, p, p->buffer - 1);
	else if (p->move && ++p->elapsed >= c->move[p->move - 1].anim.total)
		p->move = p->elapsed = p->hit = 0;
	if (p->stun) {
		age(p);
		if (!--p->stun)
			p->block = p->elapsed = 0;
	}
	if (p->slide)
		slide(p, q);
	if (!grounded(p) && !p->move) {
		p->y += p->vy / 4;
		p->vy += 3;
		if (p->y >= 0)
			p->y = p->vy = 0;
	}
	p->walk = 0;
	if (!p->move && !p->stun) {
		int ground = grounded(p);
		if (ground)
			p->face = q->x >= p->x ? 1 : -1;
		p->crouch = ground && !p->squat && (keys & KEY(DOWN));
		if (p->buffer && ground) {
			start(s, c, p, p->buffer - 1);
		} else if (p->squat) {
			if (!--p->squat)
				p->vy = -48;
		} else if (ground && threat && (keys & back_key(p))) {
			if (p->block != 2)
				p->block = 2, p->elapsed = 0;
			else
				age(p);
		} else {
			if (p->block)
				p->block = p->elapsed = 0;
			if (!p->crouch) {
				int dx = (keys & KEY(RIGHT) ? 2 : 0) - (keys & KEY(LEFT) ? 2 : 0);
				p->x += dx;
				p->walk = dx * p->face > 0 ? 1 : dx ? -1 : 0;
				if ((keys & KEY(UP)) && ground)
					p->squat = JUMP_SQUAT;
			}
		}
	}
	if (p->buffer && ++p->age > BUFFER_TICKS)
		p->buffer = p->age = 0;
	if (p->move) {
		uint32_t begin;
		const struct move *m = &c->move[p->move - 1];
		const struct step *st =
		    &a->step[step_at(a, &m->anim, (uint32_t)p->elapsed, 0, &begin)];
		p->x += p->face * st->vx;
		p->y += st->vy;
		if (st->spawn && begin == (uint32_t)p->elapsed)
			launch(s, c, i, st->spawn);
	}
	p->x = clamp(p->x, WALL_LEFT, WALL_RIGHT);
	return 0;
}
/* Does fighter i's current hitbox touch the other's hurtboxes? */
static int contact(const struct fight_state *s, const struct assets *a, int i)
{
	const struct fighter *p = &s->p[i], *q = &s->p[1 - i];
	if (!p->move || !q->hp)
		return 0;
	const struct step *hit = &a->step[fight_pose(s, a, i)],
			  *body = &a->step[fight_pose(s, a, 1 - i)];
	if (!hit->group || hit->group == p->hit)
		return 0;
	for (unsigned h = 0; h < hit->hit; h++) {
		struct area x = fight_box(&a->box[hit->box + hit->hurt + h], p->x, p->y, p->face);
		for (unsigned k = 0; k < body->hurt; k++)
			if (overlap(x, fight_box(&a->box[body->box + k], q->x, q->y, q->face)))
				return 1;
	}
	return 0;
}
static int shot_hits_body(const struct fight_state *s, const struct assets *a,
			  const struct shot *sh, int i)
{
	const struct fighter *q = &s->p[i];
	const struct step *hit = &a->step[shot_step(s, a, sh)],
			  *body = &a->step[fight_pose(s, a, i)];
	for (unsigned h = 0; h < hit->hit; h++) {
		struct area x = fight_box(&a->box[hit->box + h], sh->x, sh->y, sh->face);
		for (unsigned k = 0; k < body->hurt; k++)
			if (overlap(x, fight_box(&a->box[body->box + k], q->x, q->y, q->face)))
				return 1;
	}
	return 0;
}
static int shots_clash(const struct fight_state *s, const struct assets *a,
		       const struct shot *u, const struct shot *v)
{
	const struct step *x = &a->step[shot_step(s, a, u)], *y = &a->step[shot_step(s, a, v)];
	for (unsigned h = 0; h < x->hit; h++)
		for (unsigned k = 0; k < y->hit; k++)
			if (overlap(fight_box(&a->box[x->box + h], u->x, u->y, u->face),
				    fight_box(&a->box[y->box + k], v->x, v->y, v->face)))
				return 1;
	return 0;
}
/* Can the defender block this hit with these keys? */
static int blocks(const struct fighter *q, uint32_t keys, const struct hit *h)
{
	if (q->move || (q->stun && q->block != 1) || !grounded(q) || !(keys & back_key(q)))
		return 0;
	int low = (keys & KEY(DOWN)) != 0;
	return h->guard == GUARD_HIGH || (h->guard == GUARD_LOW) == low;
}
/* Damage, stun and pushback on fighter q; a strike also freezes its
 * attacker p (projectiles pass p = NULL). */
static void struck(struct fight_state *s, struct fighter *p, struct fighter *q,
		   const struct hit *h, int guarded, int face, uint32_t keys)
{
	q->hp = clamp(q->hp - (guarded ? h->chip : h->damage), 0, 1000);
	q->move = q->hit = q->squat = 0;
	q->stun = guarded ? h->blockstun : h->hitstun;
	q->block = guarded;
	q->elapsed = 0;
	if (guarded)
		q->crouch = (keys & KEY(DOWN)) != 0;
	if (q->y)
		q->crouch = 0;
	q->slide = face * (guarded ? h->block_pushback : h->pushback);
	q->corner = p != NULL && q->slide;
	if (p && p->freeze < h->hitstop)
		p->freeze = h->hitstop;
	if (q->freeze < h->hitstop)
		q->freeze = h->hitstop;
	s->tone = 2400;
	s->hits++;
	s->guards += (unsigned)guarded;
}
static void shots_move(struct fight_state *s, const struct assets *a)
{
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		struct shot *sh = &s->shot[k];
		if (!sh->kind)
			continue;
		const struct projectile *t = shot_type(s, a, sh);
		sh->age++;
		sh->x += sh->face * t->speed;
		if (sh->age >= t->lifetime || sh->x < -SHOT_MARGIN || sh->x > FW + SHOT_MARGIN)
			memset(sh, 0, sizeof(*sh));
	}
}
static void combat(struct fight_state *s, const struct assets *a, const uint32_t keys[2])
{
	int threat[2], frozen[2];
	/* Both fighters see the other's state from the start of the tick. */
	for (int i = 0; i < 2; i++) {
		const struct fighter *p = &s->p[i], *q = &s->p[1 - i];
		threat[i] = q->move && abs(q->x - p->x) < GUARD_RANGE;
		for (int k = 0; k < MAX_PROJECTILES; k++)
			if (s->shot[k].kind && s->shot[k].owner != i &&
			    abs(s->shot[k].x - p->x) < GUARD_RANGE)
				threat[i] = 1;
	}
	shots_move(s, a);
	for (int i = 0; i < 2; i++)
		frozen[i] = update(s, a, i, keys[i], threat[i]);
	/* Resolve every contact from the same state before changing any. */
	const struct move *move[2] = {NULL, NULL};
	int guarded[2] = {0, 0}, group[2] = {0, 0}, struck_now[2] = {0, 0};
	for (int i = 0; i < 2; i++)
		if (!frozen[i] && contact(s, a, i)) {
			move[i] = &who(s, a, i)->move[s->p[i].move - 1];
			group[i] = a->step[fight_pose(s, a, i)].group;
			guarded[i] = blocks(&s->p[1 - i], keys[1 - i], &move[i]->hit);
			struck_now[1 - i] = 1;
		}
	int gone[MAX_PROJECTILES] = {0}, landed[MAX_PROJECTILES] = {0},
	    shot_guard[MAX_PROJECTILES] = {0};
	for (int k = 0; k < MAX_PROJECTILES; k++)
		for (int j = k + 1; j < MAX_PROJECTILES; j++)
			if (s->shot[k].kind && s->shot[j].kind &&
			    s->shot[k].owner != s->shot[j].owner &&
			    shots_clash(s, a, &s->shot[k], &s->shot[j])) {
				gone[k] = gone[j] = 1;
				s->clashes++;
			}
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &s->shot[k];
		int d = 1 - sh->owner;
		if (!sh->kind || gone[k] || !s->p[d].hp || struck_now[d] ||
		    !shot_hits_body(s, a, sh, d))
			continue;
		landed[k] = 1;
		struck_now[d] = 1;
		shot_guard[k] = blocks(&s->p[d], keys[d], &shot_type(s, a, sh)->hit);
	}
	for (int i = 0; i < 2; i++) {
		if (!move[i])
			continue;
		struct fighter *p = &s->p[i];
		struck(s, p, &s->p[1 - i], &move[i]->hit, guarded[i], p->face, keys[1 - i]);
		if (p->move)
			p->hit = group[i];
	}
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		struct shot *sh = &s->shot[k];
		if (landed[k]) {
			int d = 1 - sh->owner;
			struck(s, NULL, &s->p[d], &shot_type(s, a, sh)->hit, shot_guard[k],
			       sh->face, keys[d]);
		}
		if (landed[k] || gone[k])
			memset(sh, 0, sizeof(*sh));
	}
	struct fighter *p0 = &s->p[0], *p1 = &s->p[1];
	if (grounded(p0) && grounded(p1) && abs(p0->x - p1->x) < 45) {
		int mid = (p0->x + p1->x) / 2, f = p0->x <= p1->x ? 1 : -1;
		p0->x = clamp(mid - f * 23, WALL_LEFT, WALL_RIGHT);
		p1->x = clamp(mid + f * 23, WALL_LEFT, WALL_RIGHT);
	}
	if (!p0->hp || !p1->hp)
		s->ended = 1;
}
/* ---- ladder ---- */
/* Characters P1 meets in a run: everyone else, or itself in a one-character
 * roster. */
static uint32_t opponents(const struct assets *a, unsigned self)
{
	uint32_t all = a->characters >= 32 ? 0xffffffffu : (1u << a->characters) - 1u;
	return a->characters == 1 ? 1u : all & ~(1u << self);
}
static unsigned count(uint32_t m)
{
	unsigned n = 0;
	for (; m; m &= m - 1)
		n++;
	return n;
}
static void next_stage(struct fight_state *s, const struct assets *a)
{
	uint32_t left = opponents(a, s->chr[0]) & ~s->beaten;
	unsigned pick = random_next(s) % count(left), c = 0;
	for (;; c++)
		if ((left >> c) & 1 && !pick--)
			break;
	s->stage = count(s->beaten) + 1;
	s->wins[0] = s->wins[1] = 0;
	fight_start(s, s->chr[0], c);
}
static void begin_run(struct fight_state *s, const struct assets *a, unsigned c0)
{
	s->chr[0] = c0;
	s->beaten = 0;
	next_stage(s, a);
}
static void to_select(struct fight_state *s, const struct assets *a)
{
	place(s);
	s->mode = MODE_SELECT;
	s->timer = s->beaten = s->stage = s->wins[0] = s->wins[1] = 0;
	s->run++;
	s->chr[1] = s->chr[0];
	s->pal[0] = 0;
	s->pal[1] = 1;
	if (s->automatic)
		begin_run(s, a, s->run % a->characters);
}
static void round_over(struct fight_state *s, const struct assets *a)
{
	int alive0 = s->p[0].hp > 0, alive1 = s->p[1].hp > 0;
	s->round++;
	if (alive0 != alive1)
		s->wins[alive0 ? 0 : 1]++;
	if (s->wins[0] >= ROUNDS_TO_WIN) {
		s->beaten |= 1u << s->chr[1];
		uint32_t all = opponents(a, s->chr[0]);
		if ((s->beaten & all) == all) {
			place(s);
			s->mode = MODE_CLEAR;
			s->timer = 0;
			s->wins[0] = s->wins[1] = 0;
		} else
			next_stage(s, a);
	} else if (s->wins[1] >= ROUNDS_TO_WIN) {
		place(s);
		s->mode = MODE_RESULT;
		s->timer = 0;
		s->wins[0] = s->wins[1] = 0;
	} else
		place(s);
}
static void select_screen(struct fight_state *s, const struct assets *a, uint32_t pressed)
{
	int n = (int)a->characters, c = (int)s->chr[0];
	if (s->automatic) {
		begin_run(s, a, s->run % a->characters);
		return;
	}
	if (s->timer < 255)
		s->timer++;
	if (pressed & KEY(LEFT))
		c = (c + n - 1) % n;
	if (pressed & KEY(RIGHT))
		c = (c + 1) % n;
	if ((pressed & KEY(UP)) && c >= 4)
		c -= 4;
	if ((pressed & KEY(DOWN)) && c + 4 < n)
		c += 4;
	s->chr[0] = s->chr[1] = (uint32_t)c;
	if (pressed & ATTACKS)
		begin_run(s, a, (unsigned)c);
}
void fight_step(struct fight_state *s, const struct assets *a, uint32_t input,
		uint32_t second, int human)
{
	uint32_t pressed = input & ~s->previous;
	s->previous = input;
	if (pressed & KEY(START))
		s->paused ^= 1;
	if (pressed & KEY(L))
		s->boxes ^= 1;
	if (pressed & KEY(R))
		s->stress = (s->stress + 1) % 4;
	if (s->paused)
		return;
	if (s->automatic)
		s->stress = (s->tick / 600) % 4;
	s->tick++;
	if (s->mode == MODE_SELECT) {
		select_screen(s, a, pressed);
		return;
	}
	if (s->mode != MODE_FIGHT) {
		if (s->timer < 255)
			s->timer++;
		if (s->timer > (s->automatic ? AUTO_SCREEN : SCREEN_TICKS) ||
		    (s->timer > SCREEN_SKIP && (pressed & ATTACKS)))
			to_select(s, a);
		return;
	}
	if (s->ended) {
		/* Airborne fighters still land and projectiles fly while the KO
		 * plays; nothing hits. */
		for (int i = 0; i < 2; i++) {
			struct fighter *p = &s->p[i];
			if (p->move)
				p->move = p->elapsed = p->hit = 0;
			if (!grounded(p)) {
				p->y += p->vy / 4;
				p->vy += 3;
				if (p->y >= 0)
					p->y = p->vy = 0;
			}
			p->walk = 0;
		}
		shots_move(s, a);
		if (++s->ended > KO_TICKS || (pressed & (KEY(A) | KEY(B))))
			round_over(s, a);
		return;
	}
	uint32_t keys[2];
	keys[0] = s->automatic ? ai(s, a, 0) : input;
	keys[1] = human ? second : ai(s, a, 1);
	combat(s, a, keys);
}
/* ---- drawing ---- */
static void rect(struct scene *s, int x, int y, int w, int h, uint16_t color, int blend,
		 int alpha)
{
	if (s->count < COMMANDS)
		s->cmd[s->count++] = (struct command){
		    RECT, x, y, w, h, 0, 0, blend, color, (uint8_t)alpha};
}
/* Compact original 3x5 glyphs: digits, then the limited HUD alphabet. */
static const char alphabet[] = "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ:-/";
static const uint16_t glyph[] = {31599, 9362, 29671, 29647, 23497, 31183, 31215, 29257,
				 31727, 31695, 0, 11245, 27566, 31015, 27502, 31207,
				 31204, 31087, 23533, 29847, 4687, 23469, 18727, 24557,
				 31597, 31599, 31716, 31611, 27565, 31183, 29842, 23407,
				 23402, 23423, 23213, 23186, 29351, 1040, 448, 4772};
/* Each glyph row's lit run is one 2-pixel-high rectangle. */
static void text(struct scene *s, int x, int y, const char *str, uint16_t color)
{
	for (; *str; str++, x += 8) {
		const char *p = strchr(alphabet, *str);
		if (!p)
			continue;
		unsigned bits = glyph[p - alphabet];
		for (int r = 0; r < 5; r++)
			for (int c = 0; c < 3; c++) {
				if (!(bits & (1u << (14 - r * 3 - c))))
					continue;
				int n = 1;
				while (c + n < 3 && (bits & (1u << (14 - r * 3 - c - n))))
					n++;
				rect(s, x + c * 2, y + r * 2, 2 * n, 2, color, OPAQUE, 255);
				c += n - 1;
			}
	}
}
static int text_width(const char *str) { return 8 * (int)strlen(str); }
static void sprite(struct scene *s, const struct assets *a, int image, int x, int y,
		   int flip, int blend, int alpha)
{
	const struct image *im = &a->image[image];
	if (s->count < COMMANDS)
		s->cmd[s->count++] = (struct command){
		    SPRITE, flip ? x - (im->w - im->ax) : x - im->ax,
		    y - im->ay, im->w, im->h, image, flip, blend, 0, (uint8_t)alpha};
}
/* Blended effects share EFFECT_PIXEL_BUDGET opaque pixels per frame; an
 * effect that no longer fits is dropped and later smaller ones may still
 * draw. Rules never see effects, so dropping cannot change the fight. */
static void effect(struct scene *s, const struct assets *a, int image, int x, int y,
		   int flip, int blend, int alpha, uint32_t *budget)
{
	uint32_t cost = a->image[image].count;
	s->effects_requested++;
	if (cost > *budget || s->count >= COMMANDS)
		return;
	*budget -= cost;
	s->effects_drawn++;
	sprite(s, a, image, x, y, flip, blend, alpha);
}
/* Side i's image of a frame, or -1 when the cast does not match the state. */
static int cast_image(const struct fight_state *s, const struct assets *a, int i, unsigned f)
{
	if (a->cast[i][0] != (int)s->chr[i] || a->cast[i][1] != (int)s->pal[i])
		return -1;
	return a->look[i][f];
}
/* Box overlay: a one-pixel opaque outline, hitboxes also tinted. */
static void outline(struct scene *s, struct area r, uint16_t color, int alpha, int width)
{
	if (alpha)
		rect(s, r.x, r.y, r.w, r.h, color, ALPHA, alpha);
	rect(s, r.x, r.y, r.w, width, color, OPAQUE, 255);
	rect(s, r.x, r.y + r.h - width, r.w, width, color, OPAQUE, 255);
	rect(s, r.x, r.y, width, r.h, color, OPAQUE, 255);
	rect(s, r.x + r.w - width, r.y, width, r.h, color, OPAQUE, 255);
}
static void stage(struct scene *out)
{
	/* Background is deliberately drawn in software, not supplied by the host. */
	rect(out, 0, 0, FW, FH, 0x10c6, OPAQUE, 255);
	for (int i = 0; i < 8; i++)
		rect(out, i * 64, 100 + (i % 3) * 15, 46, 140, 0x212b, OPAQUE, 255);
	rect(out, 0, GROUND, FW, FH - GROUND, 0x39a8, OPAQUE, 255);
	rect(out, 0, GROUND, FW, 2, 0x8c50, OPAQUE, 255);
}
static void idle_sprite(struct scene *out, const struct fight_state *s, const struct assets *a,
			int i, int x, int y, int flip)
{
	const struct character *c = who(s, a, i);
	const struct step *st = &a->step[step_at(a, &c->action[IDLE], s->tick, 1, NULL)];
	int im = cast_image(s, a, i, st->frame);
	if (im >= 0)
		sprite(out, a, im, x, y, flip, OPAQUE, 255);
}
static void select_scene(const struct fight_state *s, const struct assets *a, struct scene *out)
{
	rect(out, 0, 0, FW, FH, 0x0841, OPAQUE, 255);
	text(out, 240 - text_width("SELECT") / 2, 12, "SELECT", 0xffff);
	for (unsigned k = 0; k < a->characters; k++) {
		int x = 16 + (int)(k % 4) * 114, y = 40 + (int)(k / 4) * 20;
		rect(out, x, y, 108, 18, 0x2124, OPAQUE, 255);
		if (k == s->chr[0])
			outline(out, (struct area){x, y, 108, 18}, 0x07e0, 0, 2);
		text(out, x + 6, y + 4, a->chr[k].name, k == s->chr[0] ? 0xffe0 : 0xffff);
	}
	rect(out, 0, 300, FW, 2, 0x8c50, OPAQUE, 255);
	idle_sprite(out, s, a, 0, 120, 300, 0);
	text(out, 220, 240, "P1", 0x07e0);
	text(out, 220, 254, a->chr[s->chr[0]].name, 0xffff);
	text(out, 220, 280, "ATTACK TO FIGHT", 0x8410);
}
static void screen_scene(const struct fight_state *s, const struct assets *a, struct scene *out)
{
	char line[24];
	unsigned all = count(opponents(a, s->chr[0]));
	int clear = s->mode == MODE_CLEAR;
	rect(out, 0, 0, FW, FH, clear ? 0x0110 : 0x2000, OPAQUE, 255);
	const char *title = clear ? "ALL CLEAR" : "YOU LOSE";
	text(out, 240 - text_width(title) / 2, 60, title, clear ? 0xffe0 : 0xf800);
	text(out, 240 - text_width(a->chr[s->chr[0]].name) / 2, 90, a->chr[s->chr[0]].name,
	     0xffff);
	unsigned beaten = count(s->beaten);
	strcpy(line, "BEATEN ");
	size_t n = strlen(line);
	line[n++] = (char)('0' + beaten / 10 % 10);
	line[n++] = (char)('0' + beaten % 10);
	line[n++] = '/';
	line[n++] = (char)('0' + all / 10 % 10);
	line[n++] = (char)('0' + all % 10);
	line[n] = 0;
	text(out, 240 - text_width(line) / 2, 110, line, 0xffff);
	rect(out, 0, 300, FW, 2, 0x8c50, OPAQUE, 255);
	idle_sprite(out, s, a, 0, 240, 300, 0);
}
static void number(char *out, unsigned v)
{
	if (v >= 10)
		*out++ = (char)('0' + v / 10 % 10);
	*out++ = (char)('0' + v % 10);
	*out = 0;
}
void fight_scene(const struct fight_state *s, const struct assets *a, struct scene *out)
{
	out->count = out->effects_requested = out->effects_drawn = 0;
	if (s->mode == MODE_SELECT) {
		select_scene(s, a, out);
		return;
	}
	if (s->mode != MODE_FIGHT) {
		screen_scene(s, a, out);
		return;
	}
	stage(out);
	for (int i = 0; i < 2; i++) {
		const struct fighter *p = &s->p[i];
		rect(out, p->x - 30, GROUND - 4, 60, 6, 0, ALPHA, 120);
		/* Sheet art faces right; the foot anchor stands on the ground. */
		const struct step *st = &a->step[fight_pose(s, a, i)];
		int im = cast_image(s, a, i, st->frame);
		if (im >= 0)
			sprite(out, a, im, p->x, GROUND + p->y, p->face < 0, OPAQUE, 255);
	}
	/* Projectiles: opaque ones as sprites, blended ones within the effect
	 * budget (ahead of the stress effects). */
	uint32_t budget = EFFECT_PIXEL_BUDGET;
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &s->shot[k];
		if (!sh->kind)
			continue;
		const struct projectile *t = shot_type(s, a, sh);
		int im = cast_image(s, a, sh->owner, a->step[shot_step(s, a, sh)].frame);
		if (im < 0)
			continue;
		if (t->blend == OPAQUE)
			sprite(out, a, im, sh->x, GROUND + sh->y, sh->face < 0, OPAQUE, 255);
		else
			effect(out, a, im, sh->x, GROUND + sh->y, sh->face < 0, t->blend,
			       t->alpha, &budget);
	}
	/* Drawn after the sprites so none hides the boxes. */
	for (int i = 0; s->boxes && i < 2; i++) {
		const struct fighter *p = &s->p[i];
		const struct step *st = &a->step[fight_pose(s, a, i)];
		for (unsigned k = 0; k < (unsigned)st->hurt + st->hit; k++)
			outline(out, fight_box(&a->box[st->box + k], p->x, p->y, p->face),
				k < st->hurt ? 0x07e0 : 0xf800, k < st->hurt ? 0 : 96, 1);
		rect(out, p->x - 3, GROUND + p->y, 7, 1, 0xffff, OPAQUE, 255);
	}
	for (int k = 0; s->boxes && k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &s->shot[k];
		if (!sh->kind)
			continue;
		const struct step *st = &a->step[shot_step(s, a, sh)];
		for (unsigned h = 0; h < st->hit; h++)
			outline(out, fight_box(&a->box[st->box + h], sh->x, sh->y, sh->face),
				0xf800, 96, 1);
	}
	/* Stress modes: 8, 24 or 48 overlapping animated alpha/additive effects. */
	int n = s->stress == 1 ? 8 : s->stress == 2 ? 24 : s->stress == 3 ? 48 : 0;
	for (int i = 0; i < n && a->effects; i++) {
		int x = (int)((s->tick * 3 + (unsigned)i * 71) % 560) - 80;
		int y = 200 + (i * 23) % 120;
		int image = a->effect[(s->tick / 4 + (unsigned)i) % a->effects];
		effect(out, a, image, x, y, i % 2, i % 3 ? ALPHA : ADD, 160, &budget);
	}
	rect(out, 16, 16, 204, 13, 0, OPAQUE, 255);
	rect(out, 260, 16, 204, 13, 0, OPAQUE, 255);
	rect(out, 18, 18, s->p[0].hp / 5, 9, 0x07ed, OPAQUE, 255);
	rect(out, 262, 18, s->p[1].hp / 5, 9, 0xfbe0, OPAQUE, 255);
	const char *n0 = who(s, a, 0)->name, *n1 = who(s, a, 1)->name;
	text(out, 16, 34, n0, 0xffff);
	text(out, 464 - text_width(n1), 34, n1, 0xffff);
	/* Stage x/y of the ladder under the mode indicator. */
	char line[8];
	number(line, s->stage);
	size_t len = strlen(line);
	line[len++] = '/';
	number(line + len, count(opponents(a, s->chr[0])));
	text(out, 240 - text_width(line) / 2, 34, line, 0xffff);
	if (s->automatic)
		text(out, 224, 48, "AUTO", 0x8410);
	/* Keep labels short: each glyph row run is a bounded command. */
	if (s->paused)
		text(out, 220, 80, "PAUSE", 0xffff);
	else if (s->ended)
		text(out, 232, 80, "KO", 0xffff);
	/* Mode indicators don't depend on the text command budget. */
	rect(out, 220, 16, 40, 13, (uint16_t)(0x001f + s->stress * 0x3800), OPAQUE,
	     255);
}
/* ---- state words ---- */
#define FIGHTER_WORDS 26
#define SHOT_WORDS 6
#define GLOBAL_WORDS 19
#define SHOTS_AT (GLOBAL_WORDS + 2 * FIGHTER_WORDS)
#define TAIL_AT (SHOTS_AT + MAX_PROJECTILES * SHOT_WORDS)
#define USED_WORDS (TAIL_AT + 7)
static void fighter_fields(struct fighter *p, int32_t *v[FIGHTER_WORDS])
{
	int32_t *f[] = {&p->x,	   &p->y,     &p->vy,	     &p->hp,	&p->face,  &p->walk,
			&p->block, &p->crouch, &p->move,     &p->elapsed, &p->hit,   &p->stun,
			&p->freeze, &p->slide, &p->keys,     &p->corner, &p->squat, &p->buffer,
			&p->age,   &p->plan,   &p->plan_tick};
	for (int k = 0; k < 21; k++)
		v[k] = f[k];
	v[21] = (int32_t *)(void *)&p->charge;
	for (int k = 0; k < HISTORY / 8; k++)
		v[22 + k] = (int32_t *)(void *)&p->history[k];
}
static void global_fields(struct fight_state *s, uint32_t *v[GLOBAL_WORDS])
{
	uint32_t *f[GLOBAL_WORDS] = {&s->tick,  &s->rng,    &s->previous, &s->paused, &s->stress,
				     &s->boxes, &s->automatic, &s->round, &s->mode,	&s->timer,
				     &s->beaten, &s->stage, &s->run,	  &s->wins[0], &s->wins[1],
				     &s->chr[0], &s->chr[1], &s->pal[0],   &s->pal[1]};
	memcpy(v, f, sizeof(f));
}
static void tail_fields(struct fight_state *s, uint32_t *v[7])
{
	uint32_t *f[7] = {&s->tone, &s->phase, &s->ended, &s->hits, &s->guards, &s->specials,
			  &s->clashes};
	memcpy(v, f, sizeof(f));
}
void fight_encode(const struct fight_state *s, uint32_t w[STATE_WORDS])
{
	struct fight_state t = *s;
	uint32_t *g[GLOBAL_WORDS], *e[7];
	memset(w, 0, STATE_WORDS * sizeof(*w));
	global_fields(&t, g);
	for (int k = 0; k < GLOBAL_WORDS; k++)
		w[k] = *g[k];
	for (int i = 0; i < 2; i++) {
		int32_t *v[FIGHTER_WORDS];
		fighter_fields(&t.p[i], v);
		for (int k = 0; k < FIGHTER_WORDS; k++)
			w[GLOBAL_WORDS + i * FIGHTER_WORDS + k] = (uint32_t)*v[k];
	}
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		const struct shot *sh = &t.shot[k];
		const int32_t v[SHOT_WORDS] = {sh->kind, sh->owner, sh->x, sh->y, sh->face, sh->age};
		for (int j = 0; j < SHOT_WORDS; j++)
			w[SHOTS_AT + k * SHOT_WORDS + j] = (uint32_t)v[j];
	}
	tail_fields(&t, e);
	for (int k = 0; k < 7; k++)
		w[TAIL_AT + k] = *e[k];
}
static int valid(const struct fighter *p, const struct assets *a, const struct character *c)
{
	if (p->x < WALL_LEFT || p->x > WALL_RIGHT || p->y < -MAX_HEIGHT || p->y > 0 ||
	    p->vy < -48 || p->vy > 60 || p->hp < 0 || p->hp > 1000 ||
	    (p->face != 1 && p->face != -1) || p->walk < -1 || p->walk > 1 ||
	    p->block < 0 || p->block > 2 || p->crouch < 0 || p->crouch > 1 ||
	    (p->crouch && p->y) || p->move < 0 || p->move > (int)c->moves || p->elapsed < 0 ||
	    p->hit < 0 || p->stun < 0 || p->stun > MAX_STUN || p->freeze < 0 ||
	    p->freeze > MAX_HITSTOP || p->slide < -MAX_PUSH || p->slide > MAX_PUSH ||
	    p->keys < 0 || p->keys > 0xffff || p->corner < 0 || p->corner > 1 ||
	    (p->corner && !p->slide) || p->squat < 0 || p->squat > JUMP_SQUAT ||
	    p->buffer < 0 || p->buffer > (int)c->moves || p->age < 0 ||
	    p->age > BUFFER_TICKS || (!p->buffer && p->age) || p->plan < 0 ||
	    p->plan > (int)c->moves || (p->plan && p->plan <= MOVES) || p->plan_tick < 0 ||
	    p->plan_tick > CHARGE_TICKS + 3 || (!p->plan && p->plan_tick))
		return 0;
	for (int k = 0; k < 2; k++) {
		unsigned held = charge_byte(p->charge, 2 * k), grace = charge_byte(p->charge, 2 * k + 1);
		if (grace > CHARGE_GRACE || (grace && held))
			return 0;
	}
	if (p->squat && (!grounded(p) || p->move || p->stun))
		return 0;
	if (p->move) {
		const struct move *m = &c->move[p->move - 1];
		if (p->elapsed >= m->anim.total || p->hit > m->groups || p->stun || p->block ||
		    p->vy || p->y != height(a, m, (uint32_t)p->elapsed))
			return 0;
	} else if (p->elapsed > 255 || p->hit)
		return 0;
	return (p->block != 1 || p->stun) && (p->block != 2 || !p->stun);
}
int fight_decode(struct fight_state *s, const struct assets *a,
		 const uint32_t w[STATE_WORDS])
{
	struct fight_state t;
	memset(&t, 0, sizeof(t));
	uint32_t *g[GLOBAL_WORDS], *e[7];
	global_fields(&t, g);
	for (int k = 0; k < GLOBAL_WORDS; k++)
		*g[k] = w[k];
	tail_fields(&t, e);
	for (int k = 0; k < 7; k++)
		*e[k] = w[TAIL_AT + k];
	for (int i = USED_WORDS; i < STATE_WORDS; i++)
		if (w[i])
			return 0;
	unsigned n = a->characters;
	uint32_t all = n >= 32 ? 0xffffffffu : (1u << n) - 1u;
	if (t.paused > 1 || t.stress > 3 || t.boxes > 1 || t.automatic > 1 || t.mode >= MODES ||
	    t.timer > 255 || (t.beaten & ~all) || t.stage > n || t.wins[0] >= ROUNDS_TO_WIN ||
	    t.wins[1] >= ROUNDS_TO_WIN || t.chr[0] >= n || t.chr[1] >= n || t.pal[0] > 1 ||
	    t.pal[1] > 1 || t.tone > 2400 || t.phase >= 48000 || t.ended > KO_TICKS ||
	    (t.ended && t.mode != MODE_FIGHT))
		return 0;
	if (t.mode == MODE_FIGHT) {
		uint32_t opp = opponents(a, t.chr[0]);
		/* A mirror is always a valid fight (tests and versus set one up). */
		if (t.pal[0] || t.pal[1] != (t.chr[0] == t.chr[1]) ||
		    (!((opp >> t.chr[1]) & 1) && t.chr[0] != t.chr[1]) ||
		    (t.beaten & ~opp) || ((t.beaten >> t.chr[1]) & 1) ||
		    t.stage != count(t.beaten) + 1)
			return 0;
	}
	for (int i = 0; i < 2; i++) {
		int32_t *v[FIGHTER_WORDS];
		fighter_fields(&t.p[i], v);
		for (int k = 0; k < FIGHTER_WORDS; k++)
			*v[k] = (int32_t)w[GLOBAL_WORDS + i * FIGHTER_WORDS + k];
		if (!valid(&t.p[i], a, &a->chr[t.chr[i]]))
			return 0;
	}
	int owned[2] = {0, 0};
	for (int k = 0; k < MAX_PROJECTILES; k++) {
		struct shot *sh = &t.shot[k];
		const uint32_t *d = w + SHOTS_AT + k * SHOT_WORDS;
		*sh = (struct shot){(int32_t)d[0], (int32_t)d[1], (int32_t)d[2],
				    (int32_t)d[3], (int32_t)d[4], (int32_t)d[5]};
		if (!sh->kind) {
			if (sh->owner || sh->x || sh->y || sh->face || sh->age)
				return 0;
			continue;
		}
		if (sh->owner < 0 || sh->owner > 1 || owned[sh->owner]++ || t.mode != MODE_FIGHT ||
		    sh->kind < 0 || sh->kind > (int)a->chr[t.chr[sh->owner]].projectiles ||
		    (sh->face != 1 && sh->face != -1) || sh->x < -SHOT_MARGIN ||
		    sh->x > FW + SHOT_MARGIN || sh->y < -MAX_SIDE - MAX_HEIGHT || sh->y > MAX_SIDE ||
		    sh->age < 0 ||
		    sh->age >= a->chr[t.chr[sh->owner]].projectile[sh->kind - 1].lifetime)
			return 0;
	}
	*s = t;
	return 1;
}
