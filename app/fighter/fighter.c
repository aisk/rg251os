/* Deterministic two-fighter rules: movement, jumps, crouching, eight
 * data-driven normals, hurt/hit boxes from the pack, hitstop, hitstun,
 * blockstun, high/low/overhead guard, pushback with corner transfer, KO,
 * AI and the stress scenes, plus the draw-command list (L overlays the real
 * boxes). Rules pick an action or move and its elapsed ticks; the pack's
 * steps map that onto frames and boxes, so what is drawn is what collides.
 * Coordinates are integer pixels, jumps use quarter-pixel vertical velocity.
 *
 * Timing, in ticks (one fight_step each): a move pressed on tick T shows
 * elapsed 0 on T and ends on tick T + total, when the fighter may act
 * again. A contact on tick C freezes both fighters for hitstop ticks
 * (C+1 .. C+hitstop); the defender is then stunned and acts again on tick
 * C + hitstop + hitstun (or blockstun). Pushback slides the defender
 * ceil(rest / 4) pixels per unfrozen tick; what a wall stops moves the
 * attacker back instead. */
#include "fighter.h"
#include "libretro.h"
#include <stdlib.h>
#include <string.h>
#define KEY(k) (1u << RETRO_DEVICE_ID_JOYPAD_##k)
/* Normals by strength, strongest first; see libretro.c for the layout. */
static const uint32_t buttons[4] = {KEY(A), KEY(X), KEY(B), KEY(Y)};
static const int strength[4] = {STAND_HK, STAND_HP, STAND_LK, STAND_LP};
/* Holding back while the opponent's move is this close shows the guard. */
#define GUARD_RANGE 160
static int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static uint32_t random_next(struct fight_state *s)
{
	s->rng ^= s->rng << 13;
	s->rng ^= s->rng >> 17;
	s->rng ^= s->rng << 5;
	return s->rng;
}
void fight_reset(struct fight_state *s, int automatic)
{
	memset(s, 0, sizeof(*s));
	s->rng = 0x351f123u;
	s->automatic = automatic;
	s->p[0].x = 140;
	s->p[1].x = 340;
	s->p[0].hp = s->p[1].hp = 1000;
	s->p[0].face = 1;
	s->p[1].face = -1;
}
static int grounded(const struct fighter *p) { return !p->y && !p->vy; }
static uint32_t back_key(const struct fighter *p)
{
	return p->face > 0 ? KEY(LEFT) : KEY(RIGHT);
}
/* Step index for an animation after t ticks; looping wraps, others hold. */
static unsigned step_at(const struct assets *a, const struct anim *an, uint32_t t, int loop)
{
	t = loop ? t % an->total : t >= an->total ? an->total - 1u : t;
	for (unsigned i = 0; i < an->count; i++) {
		const struct step *st = &a->step[an->first + i];
		if (t < st->ticks)
			return an->first + i;
		t -= st->ticks;
	}
	return an->first + an->count - 1u;
}
unsigned fight_pose(const struct fight_state *s, const struct assets *a, int i)
{
	const struct fighter *p = &s->p[i];
	const struct character *c = &a->chr[i];
	if (!p->hp)
		return step_at(a, &c->action[KO], s->ended, 0);
	if (p->move)
		return step_at(a, &c->move[p->move - 1].anim, (uint32_t)p->elapsed, 0);
	int low = p->crouch && grounded(p);
	if (p->stun)
		return step_at(a,
			       &c->action[p->block ? (low ? CROUCH_GUARD : GUARD)
						   : (low ? CROUCH_HURT : HURT)],
			       (uint32_t)p->elapsed, 0);
	if (!grounded(p))
		return step_at(a, &c->action[JUMP], (uint32_t)(p->vy + 48) / 3, 0);
	if (p->block)
		return step_at(a, &c->action[low ? CROUCH_GUARD : GUARD],
			       (uint32_t)p->elapsed, 0);
	if (p->crouch)
		return step_at(a, &c->action[CROUCH], s->tick, 1);
	if (p->walk)
		return step_at(a, &c->action[p->walk > 0 ? WALK_FORWARD : WALK_BACK],
			       s->tick, 1);
	return step_at(a, &c->action[IDLE], s->tick + (unsigned)i * 17, 1);
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
static uint32_t ai(struct fight_state *s, const struct assets *a, int i)
{
	const struct fighter *p = &s->p[i], *q = &s->p[1 - i];
	int d = q->x - p->x, dist = abs(d);
	uint32_t toward = d > 0 ? KEY(RIGHT) : KEY(LEFT),
		 away = d > 0 ? KEY(LEFT) : KEY(RIGHT);
	uint32_t r = random_next(s);
	/* Guard about two swings in three, at the wrong height one time in
	 * five; the choice is fixed for the whole swing. */
	if (q->move && dist < GUARD_RANGE) {
		const struct move *m = &a->chr[1 - i].move[q->move - 1];
		uint32_t h = ((s->tick - (uint32_t)q->elapsed) * 2654435761u + (unsigned)i) >> 27;
		if (h < 22) {
			int low = m->guard == GUARD_LOW;
			if (h < 4)
				low = !low;
			return away | (low ? KEY(DOWN) : 0u);
		}
	}
	/* Each 29-tick window plans one normal: walk in until its hitbox can
	 * reach the opponent's body, then press it once. */
	unsigned window = (s->tick + (unsigned)i * 17) / 29, plan = window * 2654435761u >> 24;
	int button = (int)(plan & 3), low = (int)(plan >> 2) & 1;
	const struct move *m = &a->chr[i].move[strength[button] + (low ? CROUCH_LP : 0)];
	uint32_t key = dist > m->reach + 20 ? toward : 0;
	if (!key && !p->move && !p->stun && !(r & 7))
		key = buttons[button] | (low ? KEY(DOWN) : 0u);
	if ((r & 255) == 0)
		key |= KEY(UP);
	return key;
}
static void age(struct fighter *p)
{
	if (p->elapsed < 255)
		p->elapsed++;
}
/* Pushback: ceil(rest / 4) pixels this tick; a wall passes the remainder
 * to the other fighter (the attacker) in the opposite direction. */
static void slide(struct fighter *p, struct fighter *q)
{
	int dir = p->slide > 0 ? 1 : -1, d = (abs(p->slide) + 3) / 4;
	p->slide -= dir * d;
	int x = clamp(p->x + dir * d, WALL_LEFT, WALL_RIGHT), rest = d - abs(x - p->x);
	p->x = x;
	if (rest)
		q->x = clamp(q->x - dir * rest, WALL_LEFT, WALL_RIGHT);
}
/* One fighter's tick before contacts; returns 1 if it was frozen. */
static int update(struct fight_state *s, const struct assets *a, int i, uint32_t keys,
		  int threat)
{
	struct fighter *p = &s->p[i], *q = &s->p[1 - i];
	uint32_t edge = keys & ~(uint32_t)p->keys;
	p->keys = (int32_t)(keys & 0xffffu);
	if (p->freeze) {
		p->freeze--;
		return 1;
	}
	if (p->move && ++p->elapsed >= a->chr[i].move[p->move - 1].anim.total)
		p->move = p->elapsed = p->hit = 0;
	if (p->stun) {
		age(p);
		if (!--p->stun)
			p->block = p->elapsed = 0;
	}
	if (p->slide)
		slide(p, q);
	if (!grounded(p)) {
		p->y += p->vy / 4;
		p->vy += 3;
		if (p->y >= 0)
			p->y = p->vy = 0;
	}
	p->walk = 0;
	if (!p->move && !p->stun) {
		int ground = grounded(p), trigger = -1;
		if (ground)
			p->face = q->x >= p->x ? 1 : -1;
		p->crouch = ground && (keys & KEY(DOWN));
		for (int k = 0; k < 4 && ground; k++)
			if (edge & buttons[k]) {
				trigger = strength[k] + (p->crouch ? CROUCH_LP : 0);
				break;
			}
		if (trigger >= 0) {
			p->move = trigger + 1;
			p->elapsed = p->hit = p->block = 0;
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
					p->vy = -48;
			}
		}
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
/* Can the defender block this move with these keys? */
static int blocks(const struct fighter *q, uint32_t keys, const struct move *m)
{
	if (q->move || (q->stun && q->block != 1) || !grounded(q) || !(keys & back_key(q)))
		return 0;
	int low = (keys & KEY(DOWN)) != 0;
	return m->guard == GUARD_HIGH || (m->guard == GUARD_LOW) == low;
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
	if (s->ended) {
		/* Airborne fighters still land while the KO plays. */
		for (int i = 0; i < 2; i++) {
			struct fighter *p = &s->p[i];
			if (!grounded(p)) {
				p->y += p->vy / 4;
				p->vy += 3;
				if (p->y >= 0)
					p->y = p->vy = 0;
			}
			p->walk = 0;
		}
		s->tick++;
		if (++s->ended > 180 || (pressed & (KEY(A) | KEY(B)))) {
			unsigned mode = s->automatic, round = s->round + 1,
				 stress = s->stress, boxes = s->boxes;
			uint32_t tick = s->tick;
			fight_reset(s, mode);
			s->round = round;
			s->stress = stress;
			s->boxes = boxes;
			s->tick = tick;
			s->previous = input;
		}
		return;
	}
	uint32_t keys[2] = {s->automatic ? ai(s, a, 0) : input, human ? second : ai(s, a, 1)};
	int threat[2], frozen[2];
	/* Both fighters see the other's state from the start of the tick. */
	for (int i = 0; i < 2; i++)
		threat[i] = s->p[1 - i].move && abs(s->p[1 - i].x - s->p[i].x) < GUARD_RANGE;
	for (int i = 0; i < 2; i++)
		frozen[i] = update(s, a, i, keys[i], threat[i]);
	/* Resolve both contacts from the same state before changing either. */
	const struct move *move[2] = {NULL, NULL};
	int guarded[2] = {0, 0}, group[2] = {0, 0};
	for (int i = 0; i < 2; i++)
		if (!frozen[i] && contact(s, a, i)) {
			move[i] = &a->chr[i].move[s->p[i].move - 1];
			group[i] = a->step[fight_pose(s, a, i)].group;
			guarded[i] = blocks(&s->p[1 - i], keys[1 - i], move[i]);
		}
	for (int i = 0; i < 2; i++) {
		const struct move *m = move[i];
		if (!m)
			continue;
		struct fighter *p = &s->p[i], *q = &s->p[1 - i];
		q->hp = clamp(q->hp - (guarded[i] ? m->chip : m->damage), 0, 1000);
		q->move = q->hit = 0;
		q->stun = guarded[i] ? m->blockstun : m->hitstun;
		q->block = guarded[i];
		q->elapsed = 0;
		if (guarded[i])
			q->crouch = (keys[1 - i] & KEY(DOWN)) != 0;
		q->slide = p->face * (guarded[i] ? m->block_pushback : m->pushback);
		if (p->move)
			p->hit = group[i];
		if (p->freeze < m->hitstop)
			p->freeze = m->hitstop;
		if (q->freeze < m->hitstop)
			q->freeze = m->hitstop;
		s->tone = 2400;
		s->hits++;
		s->guards += (unsigned)guarded[i];
	}
	struct fighter *p0 = &s->p[0], *p1 = &s->p[1];
	if (grounded(p0) && grounded(p1) && abs(p0->x - p1->x) < 45) {
		int mid = (p0->x + p1->x) / 2, f = p0->x <= p1->x ? 1 : -1;
		p0->x = clamp(mid - f * 23, WALL_LEFT, WALL_RIGHT);
		p1->x = clamp(mid + f * 23, WALL_LEFT, WALL_RIGHT);
	}
	if (!p0->hp || !p1->hp)
		s->ended = 1;
	s->tick++;
}
static void rect(struct scene *s, int x, int y, int w, int h, uint16_t color, int blend,
		 int alpha)
{
	if (s->count < COMMANDS)
		s->cmd[s->count++] = (struct command){
		    RECT, x, y, w, h, 0, 0, blend, color, (uint8_t)alpha};
}
/* Compact original 3x5 glyphs: digits, then the limited POC HUD alphabet. */
static const char alphabet[] = "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ:-";
static const uint16_t glyph[] = {31599, 9362,  29671, 29647, 23497, 31183, 31215, 29257,
				 31727, 31695, 0,     23530, 27566, 31015, 27502, 31207,
				 31204, 31087, 23533, 29847, 4687,  23469, 18727, 24557,
				 24573, 31599, 31716, 31611, 31725, 31183, 29842, 23407,
				 23402, 23423, 23213, 23186, 29351, 1040,  448};
static void text(struct scene *s, int x, int y, const char *str, uint16_t color)
{
	for (; *str; str++, x += 8) {
		const char *p = strchr(alphabet, *str);
		if (!p)
			continue;
		unsigned bits = glyph[p - alphabet];
		for (int r = 0; r < 5; r++)
			for (int c = 0; c < 3; c++)
				if (bits & (1u << (14 - r * 3 - c)))
					rect(s, x + c * 2, y + r * 2, 2, 2, color,
					     OPAQUE, 255);
	}
}
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
/* Box overlay: a one-pixel opaque outline, hitboxes also tinted. */
static void outline(struct scene *s, struct area r, uint16_t color, int alpha)
{
	if (alpha)
		rect(s, r.x, r.y, r.w, r.h, color, ALPHA, alpha);
	rect(s, r.x, r.y, r.w, 1, color, OPAQUE, 255);
	rect(s, r.x, r.y + r.h - 1, r.w, 1, color, OPAQUE, 255);
	rect(s, r.x, r.y, 1, r.h, color, OPAQUE, 255);
	rect(s, r.x + r.w - 1, r.y, 1, r.h, color, OPAQUE, 255);
}
void fight_scene(const struct fight_state *s, const struct assets *a, struct scene *out)
{
	out->count = out->effects_requested = out->effects_drawn = 0;
	/* Background is deliberately drawn in software, not supplied by the host. */
	rect(out, 0, 0, FW, FH, 0x10c6, OPAQUE, 255);
	for (int i = 0; i < 8; i++)
		rect(out, i * 64, 100 + (i % 3) * 15, 46, 140, 0x212b, OPAQUE, 255);
	rect(out, 0, GROUND, FW, FH - GROUND, 0x39a8, OPAQUE, 255);
	rect(out, 0, GROUND, FW, 2, 0x8c50, OPAQUE, 255);
	for (int i = 0; i < 2; i++) {
		const struct fighter *p = &s->p[i];
		rect(out, p->x - 30, GROUND - 4, 60, 6, 0, ALPHA, 120);
		/* Sheet art faces right; the foot anchor stands on the ground. */
		const struct step *st = &a->step[fight_pose(s, a, i)];
		sprite(out, a, st->image, p->x, GROUND + p->y, p->face < 0, OPAQUE, 255);
	}
	/* Drawn after both fighters so neither sprite hides the other's boxes. */
	for (int i = 0; s->boxes && i < 2; i++) {
		const struct fighter *p = &s->p[i];
		const struct step *st = &a->step[fight_pose(s, a, i)];
		for (unsigned k = 0; k < (unsigned)st->hurt + st->hit; k++)
			outline(out, fight_box(&a->box[st->box + k], p->x, p->y, p->face),
				k < st->hurt ? 0x07e0 : 0xf800, k < st->hurt ? 0 : 96);
		rect(out, p->x - 3, GROUND + p->y, 7, 1, 0xffff, OPAQUE, 255);
	}
	/* Stress modes: 8, 24 or 48 overlapping animated alpha/additive effects. */
	int n = s->stress == 1 ? 8 : s->stress == 2 ? 24 : s->stress == 3 ? 48 : 0;
	uint32_t budget = EFFECT_PIXEL_BUDGET;
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
	text(out, 16, 36, "P0", 0xffff);
	text(out, 360, 36, s->automatic ? "AUTO" : "PLAY", 0xffff);
	/* Keep labels short: each illuminated glyph pixel is a bounded command. */
	if (s->paused)
		text(out, 220, 80, "PAUSE", 0xffff);
	else if (s->ended)
		text(out, 232, 80, "KO", 0xffff);
	/* Mode indicators don't depend on the text command budget. */
	rect(out, 220, 16, 40, 13, (uint16_t)(0x001f + s->stress * 0x3800), OPAQUE,
	     255);
}
#define FIGHTER_WORDS 15
void fight_encode(const struct fight_state *s, uint32_t w[STATE_WORDS])
{
	memset(w, 0, STATE_WORDS * sizeof(*w));
	w[0] = s->tick;
	w[1] = s->rng;
	w[2] = s->previous;
	w[3] = s->paused;
	w[4] = s->stress;
	w[5] = s->boxes;
	w[6] = s->automatic;
	w[7] = s->round;
	for (int i = 0; i < 2; i++) {
		const struct fighter *p = &s->p[i];
		const int32_t v[FIGHTER_WORDS] = {
		    p->x,    p->y,	p->vy,	   p->hp,  p->face,
		    p->walk, p->block,	p->crouch, p->move, p->elapsed,
		    p->hit,  p->stun,	p->freeze, p->slide, p->keys};
		for (int k = 0; k < FIGHTER_WORDS; k++)
			w[8 + i * FIGHTER_WORDS + k] = (uint32_t)v[k];
	}
	w[38] = s->tone;
	w[39] = s->phase;
	w[40] = s->ended;
	w[41] = s->hits;
	w[42] = s->guards;
}
static int valid(const struct fighter *p, const struct character *c)
{
	if (p->x < WALL_LEFT || p->x > WALL_RIGHT || p->y < -300 || p->y > 0 ||
	    p->vy < -48 || p->vy > 60 || p->hp < 0 || p->hp > 1000 ||
	    (p->face != 1 && p->face != -1) || p->walk < -1 || p->walk > 1 ||
	    p->block < 0 || p->block > 2 || p->crouch < 0 || p->crouch > 1 ||
	    (p->crouch && p->y) || p->move < 0 || p->move > MOVES || p->elapsed < 0 ||
	    p->hit < 0 || p->stun < 0 || p->stun > MAX_STUN || p->freeze < 0 ||
	    p->freeze > MAX_HITSTOP || p->slide < -MAX_PUSH || p->slide > MAX_PUSH ||
	    p->keys < 0 || p->keys > 0xffff)
		return 0;
	if (p->move) {
		const struct move *m = &c->move[p->move - 1];
		if (p->elapsed >= m->anim.total || p->hit > m->groups || p->stun || p->block)
			return 0;
	} else if (p->elapsed > 255 || p->hit)
		return 0;
	return (p->block != 1 || p->stun) && (p->block != 2 || !p->stun);
}
int fight_decode(struct fight_state *s, const struct assets *a,
		 const uint32_t w[STATE_WORDS])
{
	struct fight_state t = {0};
	if (w[3] > 1 || w[4] > 3 || w[5] > 1 || w[6] > 1 || w[38] > 2400 ||
	    w[39] >= 48000 || w[40] > 180)
		return 0;
	for (int i = 43; i < STATE_WORDS; i++)
		if (w[i])
			return 0;
	t.tick = w[0];
	t.rng = w[1];
	t.previous = w[2];
	t.paused = w[3];
	t.stress = w[4];
	t.boxes = w[5];
	t.automatic = w[6];
	t.round = w[7];
	for (int i = 0; i < 2; i++) {
		const uint32_t *d = w + 8 + i * FIGHTER_WORDS;
		struct fighter *p = &t.p[i];
		int32_t *v[FIGHTER_WORDS] = {
		    &p->x,    &p->y,	 &p->vy,     &p->hp,	&p->face,
		    &p->walk, &p->block, &p->crouch, &p->move,	&p->elapsed,
		    &p->hit,  &p->stun,	 &p->freeze, &p->slide, &p->keys};
		for (int k = 0; k < FIGHTER_WORDS; k++)
			*v[k] = (int32_t)d[k];
		if (!valid(p, &a->chr[i]))
			return 0;
	}
	t.tone = w[38];
	t.phase = w[39];
	t.ended = w[40];
	t.hits = w[41];
	t.guards = w[42];
	*s = t;
	return 1;
}
