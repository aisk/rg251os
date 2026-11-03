/* Deterministic two-fighter POC: movement, strikes, guard, AI and test scenes.
 * Coordinates are integer pixels, jumps use quarter-pixel vertical velocity.
 * This is a test ruleset, not a recreation of any commercial game's moves. */
#include "fighter.h"
#include "libretro.h"
#include <stdlib.h>
#include <string.h>
#define KEY(k) (1u << RETRO_DEVICE_ID_JOYPAD_##k)
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
	s->p[0] = (struct fighter){140, 0, 0, 1000, 0, 0, 1, 0};
	s->p[1] = (struct fighter){340, 0, 0, 1000, 0, 0, -1, 0};
}
static uint32_t ai(struct fight_state *s, int i)
{
	int d = s->p[1 - i].x - s->p[i].x;
	uint32_t key = abs(d) > 88 ? (d > 0 ? KEY(RIGHT) : KEY(LEFT)) : 0;
	if ((s->tick + (unsigned)i * 17) % 43 == 0)
		key |= KEY(A);
	if ((random_next(s) & 255) == 0)
		key |= KEY(UP);
	return key;
}
void fight_step(struct fight_state *s, uint32_t input, uint32_t second, int human)
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
	uint32_t keys[2] = {s->automatic ? ai(s, 0) : input, human ? second : ai(s, 1)};

	int damage[2] = {0, 0}, guard[2] = {0, 0};
	for (int i = 0; i < 2; i++) {
		struct fighter *p = &s->p[i];
		p->face = s->p[1 - i].x >= p->x ? 1 : -1;
		if (p->stun)
			p->stun--;
		if (p->attack)
			p->attack--;
		if (p->y || p->vy) {
			p->y += p->vy / 4;
			p->vy += 3;
			if (p->y >= 0)
				p->y = p->vy = 0;
		}
		if (!p->stun && !p->attack) {
			if (keys[i] & KEY(LEFT))
				p->x -= 2;
			if (keys[i] & KEY(RIGHT))
				p->x += 2;
			if ((keys[i] & KEY(UP)) && !p->y)
				p->vy = -48;
			if (keys[i] & (KEY(A) | KEY(B) | KEY(X) | KEY(Y))) {
				p->attack = 24;
				p->hit = 0;
			}
		}
		p->x = clamp(p->x, 50, 430);
	}
	/* Resolve simultaneous hits from the same state before changing either fighter.
	 */
	for (int i = 0; i < 2; i++) {
		struct fighter *p = &s->p[i], *q = &s->p[1 - i];
		if (p->attack == 17 && !p->hit && abs(p->x - q->x) < 112 &&
		    abs(p->y - q->y) < 65) {
			p->hit = 1;
			guard[1 - i] =
			    !q->attack && !q->stun &&
			    (keys[1 - i] & (q->face > 0 ? KEY(LEFT) : KEY(RIGHT)));
			damage[1 - i] = guard[1 - i] ? 8 : 65;
		}
	}
	for (int i = 0; i < 2; i++)
		if (damage[i]) {
			struct fighter *p = &s->p[i];
			p->hp = clamp(p->hp - damage[i], 0, 1000);
			p->stun = guard[i] ? 8 : 14;
			p->x = clamp(p->x - p->face * 8, 50, 430);
			p->attack = 0;
			s->tone = 2400;
			s->hits++;
			s->guards += guard[i];
		}
	if (!s->p[0].y && !s->p[1].y && abs(s->p[0].x - s->p[1].x) < 45) {
		int mid = (s->p[0].x + s->p[1].x) / 2;
		s->p[0].x = clamp(mid - s->p[0].face * 23, 50, 430);
		s->p[1].x = clamp(mid + s->p[0].face * 23, 50, 430);
	}
	if (!s->p[0].hp || !s->p[1].hp)
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
void fight_scene(const struct fight_state *s, struct scene *out)
{
	out->count = 0;
	/* Background is deliberately drawn in software, not supplied by the host. */
	rect(out, 0, 0, FW, FH, 0x10c6, OPAQUE, 255);
	for (int i = 0; i < 8; i++)
		rect(out, i * 64, 100 + (i % 3) * 15, 46, 140, 0x212b, OPAQUE, 255);
	rect(out, 0, 260, FW, 60, 0x39a8, OPAQUE, 255);
	rect(out, 0, 260, FW, 2, 0x8c50, OPAQUE, 255);
	for (int i = 0; i < 2; i++) {
		const struct fighter *p = &s->p[i];
		int pose = p->stun     ? 6
			   : p->attack ? (p->attack > 18   ? 3
					  : p->attack > 12 ? 4
							   : 5)
			   : p->y      ? 2
				       : (s->tick / 10) % 2;
		if (!p->hp)
			pose = 7;
		rect(out, p->x - 30, 256, 60, 6, 0, ALPHA, 120);
		if (out->count < COMMANDS)
			out->cmd[out->count++] = (struct command){
			    SPRITE,	      p->x - 64,   100 + p->y, SW, SH,
			    i * POSES + pose, p->face < 0, ALPHA,      0,  255};
		if (s->boxes) {
			rect(out, p->x - 22, 134 + p->y, 44, 126, 0x07e0, ALPHA, 50);
			if (p->attack > 12 && p->attack < 19)
				rect(out, p->face > 0 ? p->x : p->x - 100, 150 + p->y,
				     100, 28, 0xf800, ALPHA, 100);
		}
	}
	/* Stress modes: 8, 24 or 48 overlapping 128x160 alpha/additive sprites. */
	int n = s->stress == 1 ? 8 : s->stress == 2 ? 24 : s->stress == 3 ? 48 : 0;
	for (int i = 0; i < n && out->count < COMMANDS; i++) {
		int x = (int)((s->tick * 3 + (unsigned)i * 71) % 560) - 80;
		int y = 40 + (i * 23) % 120;
		out->cmd[out->count++] = (struct command){
		    SPRITE, x, y, SW, SH, i % SPRITES, i % 2, i % 3 ? ALPHA : ADD,
		    0,	    80};
	}
	rect(out, 16, 16, 204, 13, 0, OPAQUE, 255);
	rect(out, 260, 16, 204, 13, 0, OPAQUE, 255);
	rect(out, 18, 18, s->p[0].hp / 5, 9, 0x07ed, OPAQUE, 255);
	rect(out, 262, 18, s->p[1].hp / 5, 9, 0xfbe0, OPAQUE, 255);
	text(out, 16, 36, "POC", 0xffff);
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
void fight_encode(const struct fight_state *s, uint32_t w[STATE_WORDS])
{
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
		uint32_t *d = w + 8 + i * 8;
		d[0] = (uint32_t)p->x;
		d[1] = (uint32_t)p->y;
		d[2] = (uint32_t)p->vy;
		d[3] = (uint32_t)p->hp;
		d[4] = (uint32_t)p->attack;
		d[5] = (uint32_t)p->stun;
		d[6] = (uint32_t)p->face;
		d[7] = (uint32_t)p->hit;
	}
	w[24] = s->tone;
	w[25] = s->phase;
	w[26] = s->ended;
	w[27] = s->hits;
	w[28] = s->guards;
	w[29] = w[30] = w[31] = 0;
}
int fight_decode(struct fight_state *s, const uint32_t w[STATE_WORDS])
{
	struct fight_state t = {0};
	if (w[3] > 1 || w[4] > 3 || w[5] > 1 || w[6] > 1 || w[24] > 2400 ||
	    w[25] >= 48000 || w[26] > 180 || w[29] || w[30] || w[31])
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
		const uint32_t *d = w + 8 + i * 8;
		struct fighter *p = &t.p[i];
		p->x = (int32_t)d[0];
		p->y = (int32_t)d[1];
		p->vy = (int32_t)d[2];
		p->hp = (int32_t)d[3];
		p->attack = (int32_t)d[4];
		p->stun = (int32_t)d[5];
		p->face = (int32_t)d[6];
		p->hit = (int32_t)d[7];
		if (p->x < 50 || p->x > 430 || p->y < -300 || p->y > 0 || p->vy < -48 ||
		    p->vy > 60 || p->hp < 0 || p->hp > 1000 || p->attack < 0 ||
		    p->attack > 24 || p->stun < 0 || p->stun > 14 ||
		    (p->face != 1 && p->face != -1) || p->hit < 0 || p->hit > 1)
			return 0;
	}
	t.tone = w[24];
	t.phase = w[25];
	t.ended = w[26];
	t.hits = w[27];
	t.guards = w[28];
	*s = t;
	return 1;
}
