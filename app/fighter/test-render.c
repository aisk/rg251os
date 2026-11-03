/* Renderer and pack-decoder regression for the fighter core, linked into the
 * callback harness (test.c). It writes its own FIGHTP04 packs from known
 * bitmaps, checks that malformed or truncated packs are rejected, and
 * compares fight_render pixel for pixel with an independent per-pixel
 * reference (straight formulas, divisions, no spans) on fixed edge scenes,
 * seeded random scenes and real game scenes (with projectiles and the
 * select and ladder screens). It also rejects malformed palette, frame,
 * name, box, step, motion, move, special and projectile data, checks the
 * loader's derived frame data, the cast (resident images follow the
 * state's fighters and palettes), that boxes mirror with the sprite pixel
 * for pixel, the L box overlay and the effect pixel budget. FIGHTER_BENCH=1
 * also prints host render times per stress scene; host numbers only show
 * relative change. */
#include "fighter.h"
#include "test-render.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- independent pack reading: frames as bitmaps (-1 = transparent) ---- */
struct bitmap {
	int w, h, ax, ay;
	int16_t *px;
};
struct decoded {
	unsigned palettes, frames;
	uint16_t pal[MAX_PALETTES][256];
	unsigned pal_n[MAX_PALETTES];
	struct bitmap frame[MAX_FRAMES];
};
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t get32le(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
/* Only for packs fight_assets already accepted; asserts instead of checks. */
static void decode(const uint8_t *p, size_t size, struct decoded *d)
{
	d->palettes = get32le(p + 20);
	d->frames = get32le(p + 24);
	size_t at = 36;
	for (unsigned i = 0; i < d->palettes; i++) {
		d->pal_n[i] = get16(p + at + 2);
		at += 4;
		for (unsigned k = 0; k < d->pal_n[i]; k++, at += 2)
			d->pal[i][k] = (uint16_t)get16(p + at);
	}
	for (unsigned f = 0; f < d->frames; f++) {
		struct bitmap *b = &d->frame[f];
		at += 2; /* sheet */
		b->w = (int)get16(p + at);
		b->h = (int)get16(p + at + 2);
		b->ax = (int16_t)get16(p + at + 4);
		b->ay = (int16_t)get16(p + at + 6);
		uint32_t spans = get32le(p + at + 8);
		const uint8_t *counts = p + at + 12, *span = counts + 2 * b->h,
			      *index = span + 4 * spans;
		b->px = malloc(sizeof(int16_t) * (size_t)b->w * (size_t)b->h);
		assert(b->px);
		for (int i = 0; i < b->w * b->h; i++)
			b->px[i] = -1;
		for (int y = 0; y < b->h; y++)
			for (unsigned k = get16(counts + 2 * y); k; k--, span += 4) {
				unsigned x = get16(span), len = get16(span + 2);
				for (unsigned i = 0; i < len; i++)
					b->px[y * b->w + (int)(x + i)] = *index++;
			}
		at = (size_t)(index - p);
		assert(at <= size);
	}
}
static void release(struct decoded *d)
{
	for (unsigned f = 0; f < d->frames; f++)
		free(d->frame[f].px);
}

/* ---- per-pixel reference renderer ---- */
static uint16_t ref_pixel(uint16_t dst, uint16_t src, unsigned a, int blend)
{
	unsigned s[3] = {src >> 11, (src >> 5) & 63u, src & 31u},
		 d[3] = {dst >> 11, (dst >> 5) & 63u, dst & 31u}, max[3] = {31, 63, 31};
	if (blend != ADD && a == 255)
		return src;
	for (int i = 0; i < 3; i++) {
		if (blend == ADD) {
			d[i] = d[i] + s[i] * a / 255;
			if (d[i] > max[i])
				d[i] = max[i];
		} else
			d[i] = (s[i] * a + d[i] * (255 - a) + 127) / 255;
	}
	return (uint16_t)(d[0] << 11 | d[1] << 5 | d[2]);
}
static void reference(const struct assets *a, const struct decoded *dec,
		      const struct scene *s, uint16_t *fb)
{
	for (unsigned k = 0; k < s->count; k++) {
		const struct command *c = &s->cmd[k];
		const struct bitmap *b = NULL;
		const uint16_t *pal = NULL;
		int w = c->w, h = c->h;
		if (c->op == SPRITE) {
			if (c->image < 0 || (unsigned)c->image >= a->images)
				continue;
			const struct image *im = &a->image[c->image];
			b = &dec->frame[im->frame];
			pal = dec->pal[im->palette];
			w = b->w;
			h = b->h;
		}
		int y0 = c->y > 0 ? c->y : 0, x0 = c->x > 0 ? c->x : 0;
		for (int y = y0; y < FH && y < c->y + h; y++)
			for (int x = x0; x < FW && x < c->x + w; x++) {
				int lx = x - c->x, ly = y - c->y;
				if (!c->alpha)
					continue;
				uint16_t src = c->color;
				if (b) {
					int v = b->px[ly * w + (c->flip ? w - 1 - lx : lx)];
					if (v < 0)
						continue;
					src = pal[v];
				}
				fb[y * FW + x] = ref_pixel(fb[y * FW + x], src, c->alpha, c->blend);
			}
	}
}

/* ---- deterministic test data ---- */
static uint32_t seed = 0x2468aceu;
static uint32_t rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}
static void noise(uint16_t *fb)
{
	for (int i = 0; i < FW * FH; i++)
		fb[i] = (uint16_t)rnd();
}
static int compare(const struct assets *a, const struct decoded *dec,
		   const struct scene *s, const char *what)
{
	static uint16_t base[FW * FH], fast[FW * FH], slow[FW * FH];
	noise(base);
	memcpy(fast, base, sizeof(base));
	memcpy(slow, base, sizeof(base));
	fight_render(a, s, fast);
	reference(a, dec, s, slow);
	for (int i = 0; i < FW * FH; i++)
		if (fast[i] != slow[i]) {
			fprintf(stderr, "render mismatch in %s at %d,%d: %04x != %04x\n",
				what, i % FW, i / FW, fast[i], slow[i]);
			return 0;
		}
	return 1;
}

/* ---- synthetic pack writer with fault offsets ---- */
/* Offsets of fields the malformed-pack cases rewrite. */
enum field {
	F_FLAGS, F_PALETTES, F_PAL_SHEET, F_PAL_N, F_FRAME_SHEET, F_FRAME_W, F_FRAME_H,
	F_ROW_COUNT, F_SPAN_X, F_SPAN_LEN, F_SPAN2_X, F_INDEX, F_CHARACTERS, F_NAME_LEN,
	F_NAME_CHAR, F_CHAR_PAL0, F_CHAR_PAL1, F_CHAR_STEPS, F_CHAR_BOXES, F_CHAR_MOVES,
	F_CHAR_PROJECTILES, F_ACTION_COUNT, F_ACTION_FIRST, F_MOVE_KIND, F_MOVE_TRIGGER,
	F_MOVE_BUTTONS, F_MOVE_FIRST, F_MOVE_COUNT, F_MOVE_DAMAGE, F_MOVE_CHIP,
	F_MOVE_HITSTUN, F_MOVE_BLOCKSTUN, F_MOVE_HITSTOP, F_MOVE_PUSHBACK,
	F_MOVE_BLOCK_PUSH, F_MOVE_GUARD, F_MOVE_CANCEL, F_SPECIAL_KIND, F_SPECIAL_MOTION,
	F_SPECIAL_BUTTONS, F_PROJ_FIRST, F_PROJ_COUNT, F_PROJ_SPEED, F_PROJ_LIFETIME,
	F_PROJ_X, F_PROJ_Y, F_PROJ_DAMAGE, F_PROJ_CHIP, F_PROJ_HITSTUN, F_PROJ_BLOCKSTUN,
	F_PROJ_HITSTOP, F_PROJ_PUSHBACK, F_PROJ_BLOCK_PUSH, F_PROJ_GUARD, F_PROJ_BLEND,
	F_PROJ_ALPHA, F_STEP_FRAME, F_STEP_TICKS, F_STEP_BOX, F_STEP_HURT, F_ACTION_STEP_HIT,
	F_ACTION_STEP_VX, F_ACTION_STEP_SPAWN, F_MOVE_STEP_TICKS, F_RISE_VY, F_LAND_VY,
	F_RISE_VX, F_RISE_SPAWN, F_PROJ_STEP_HURT, F_PROJ_STEP_HIT, F_PROJ_STEP_VX,
	F_BOX_X, F_BOX_Y, F_BOX_W, F_BOX_H, F_EFFECT_FRAME, F_EFFECT_PALETTE, FIELDS
};
struct pack {
	uint8_t data[1 << 16];
	size_t len;
	size_t at[FIELDS];
};
static void put(struct pack *p, unsigned v, int bytes)
{
	for (int i = 0; i < bytes; i++)
		p->data[p->len++] = (uint8_t)(v >> (8 * i));
}
static void set(struct pack *p, size_t at, unsigned v, int bytes)
{
	for (int i = 0; i < bytes; i++)
		p->data[at + (size_t)i] = (uint8_t)(v >> (8 * i));
}
static void seal(struct pack *p)
{
	uint32_t h = 2166136261u;
	set(p, 8, (unsigned)p->len, 4);
	for (size_t i = 16; i < p->len; i++)
		h = (h ^ p->data[i]) * 16777619u;
	set(p, 12, h, 4);
}
#define SYN_FRAMES 5
static struct bitmap syn[SYN_FRAMES];
static int16_t syn_px[SYN_FRAMES][4096];
static const int syn_size[SYN_FRAMES][4] = {
    {13, 7, 6, 7}, {1, 1, 0, 1}, {40, 30, 20, 30}, {600, 3, 300, 3}, {17, 64, -5, 70}};
static void make_bitmaps(void)
{
	for (int f = 0; f < SYN_FRAMES; f++) {
		struct bitmap *b = &syn[f];
		b->w = syn_size[f][0];
		b->h = syn_size[f][1];
		b->ax = syn_size[f][2];
		b->ay = syn_size[f][3];
		b->px = syn_px[f];
		for (int y = 0; y < b->h; y++)
			for (int x = 0; x < b->w; x++) {
				int v = (x * 7 + y * 3 + f) % 16;
				/* Holes, an asymmetric notch and a ring exercise spans. */
				if ((f == 0 && ((x * 3 + y * 5) % 7 == 0 || (y == 2 && x > 8))) ||
				    (f == 2 && (x - 20) * (x - 20) + (y - 15) * (y - 15) < 60) ||
				    (f == 3 && x % 50 > 44) || (f == 4 && x == 16 && y < 63))
					v = -1;
				b->px[y * b->w + x] = (int16_t)v;
			}
	}
}
static const uint16_t syn_pal[2][16] = {
    {0x0000, 0xffff, 0xf800, 0x07e0, 0x001f, 0x8410, 0x7bef, 0x0821, 0xfffe, 0x841f,
     0xf81f, 0x07ff, 0x1234, 0xabcd, 0x5555, 0xaaaa},
    {0xffff, 0x0001, 0x0020, 0x0800, 0xf7de, 0x39e7, 0xc618, 0x4208, 0x8000, 0x0400,
     0x0010, 0xfbe0, 0x2945, 0x6b6d, 0x9cd3, 0xe71c}};
#define MARK(f) (p->at[f] = p->len)
/* One character "SYN": steps 0..7 for the actions, 8..11 for every normal
 * (9 and 10 hit), 12..13 for one 236+P special that rises 4 px for 2 ticks,
 * lands, hits on 13 and launches the projectile on 12; 14..15 are the
 * projectile's (hitboxes only). Two palettes on sheet 0, plus a third on
 * sheet 1 that no frame uses. */
#define SYN_STEPS 16
static int syn_hits(unsigned i) { return i == 9 || i == 10 || i >= 13; }
static int syn_hurts(unsigned i) { return i < 14; }
static void build(struct pack *p, int automatic)
{
	memset(p, 0, sizeof(*p));
	memcpy(p->data, "FIGHTP04", 8);
	p->len = 16;
	MARK(F_FLAGS);
	put(p, (unsigned)automatic, 4);
	MARK(F_PALETTES);
	put(p, 3, 4);
	put(p, SYN_FRAMES, 4);
	MARK(F_CHARACTERS);
	put(p, 1, 4); /* characters */
	put(p, 3, 4); /* effects */
	for (int c = 0; c < 3; c++) {
		if (!c)
			MARK(F_PAL_SHEET);
		put(p, c == 2, 2);
		if (!c)
			MARK(F_PAL_N);
		put(p, 16, 2);
		for (int i = 0; i < 16; i++)
			put(p, syn_pal[c & 1][i], 2);
	}
	for (int f = 0; f < SYN_FRAMES; f++) {
		const struct bitmap *b = &syn[f];
		if (f == 1)
			MARK(F_FRAME_SHEET);
		put(p, 0, 2);
		if (!f) {
			MARK(F_FRAME_W);
			p->at[F_FRAME_H] = p->len + 2;
		}
		put(p, (unsigned)b->w, 2);
		put(p, (unsigned)b->h, 2);
		put(p, (unsigned)(uint16_t)b->ax, 2);
		put(p, (unsigned)(uint16_t)b->ay, 2);
		size_t spans_at = p->len;
		put(p, 0, 4);
		unsigned total = 0, counts[64];
		for (int y = 0; y < b->h; y++) {
			counts[y] = 0;
			for (int x = 0; x < b->w; x++)
				if (b->px[y * b->w + x] >= 0 &&
				    (!x || b->px[y * b->w + x - 1] < 0))
					counts[y]++;
			if (!f && !y)
				MARK(F_ROW_COUNT);
			put(p, counts[y], 2);
			total += counts[y];
		}
		set(p, spans_at, total, 4);
		unsigned n = 0;
		for (int y = 0; y < b->h; y++)
			for (int x = 0; x < b->w;) {
				if (b->px[y * b->w + x] < 0) {
					x++;
					continue;
				}
				int start = x;
				while (x < b->w && b->px[y * b->w + x] >= 0)
					x++;
				if (!f && n == 0)
					MARK(F_SPAN_X), p->at[F_SPAN_LEN] = p->len + 2;
				if (!f && n == 1)
					MARK(F_SPAN2_X);
				put(p, (unsigned)start, 2);
				put(p, (unsigned)(x - start), 2);
				n++;
			}
		if (!f)
			MARK(F_INDEX);
		for (int i = 0; i < b->w * b->h; i++)
			if (b->px[i] >= 0)
				put(p, (unsigned)b->px[i], 1);
	}
	unsigned boxes = 0;
	for (unsigned i = 0; i < SYN_STEPS; i++)
		boxes += (unsigned)syn_hurts(i) + (unsigned)syn_hits(i);
	MARK(F_NAME_LEN);
	put(p, 3, 1);
	put(p, 'S', 1);
	MARK(F_NAME_CHAR);
	put(p, 'Y', 1);
	put(p, 'N', 1);
	MARK(F_CHAR_PAL0);
	put(p, 0, 2);
	MARK(F_CHAR_PAL1);
	put(p, 1, 2);
	MARK(F_CHAR_STEPS);
	put(p, SYN_STEPS, 2);
	MARK(F_CHAR_BOXES);
	put(p, boxes, 2);
	MARK(F_CHAR_MOVES);
	put(p, MOVES + 1, 2);
	MARK(F_CHAR_PROJECTILES);
	put(p, 1, 2);
	for (int k = 0; k < ACTIONS; k++) {
		if (k == HURT) {
			MARK(F_ACTION_FIRST);
			p->at[F_ACTION_COUNT] = p->len + 2;
		}
		put(p, (unsigned)(k % 6), 2);
		put(p, 1 + (unsigned)(k % 3), 2);
	}
	for (unsigned t = 0; t <= MOVES; t++) {
		/* kind, trigger, buttons, first, count, damage, chip, hitstun,
		 * blockstun, hitstop, pushback, block pushback, guard, cancel */
		const unsigned v[14] = {t == MOVES,
					t == MOVES ? MOTION_236 : t,
					t == MOVES ? BUTTON_LP | BUTTON_HP : 0,
					t == MOVES ? 12 : 8,
					t == MOVES ? 2 : 4,
					10 + t, t, 5 + t, 3 + t, 2, 6, 8,
					t % GUARD_TYPES, t & CANCEL_MASK};
		if (t == STAND_HK)
			for (int j = 0; j < 14; j++)
				p->at[F_MOVE_KIND + j] = p->len + 2 * (size_t)j;
		if (t == MOVES)
			for (int j = 0; j < 3; j++)
				p->at[F_SPECIAL_KIND + j] = p->len + 2 * (size_t)j;
		for (int j = 0; j < 14; j++)
			put(p, v[j], 2);
	}
	/* first, count, speed, lifetime, x, y, damage, chip, hitstun,
	 * blockstun, hitstop, pushback, block pushback, guard, blend, alpha */
	const unsigned proj[16] = {14, 2, 5, 50, 10, (uint16_t)-5, 40, 9, 12, 9, 4, 10, 12,
				   GUARD_LOW, ALPHA, 200};
	for (int j = 0; j < 16; j++) {
		p->at[F_PROJ_FIRST + j] = p->len;
		put(p, proj[j], 2);
	}
	for (unsigned i = 0, box = 0; i < SYN_STEPS; i++) {
		unsigned hit = (unsigned)syn_hits(i), hurt = (unsigned)syn_hurts(i);
		int vx = i == 12 ? 3 : 0, vy = i == 12 ? -2 : i == 13 ? 2 : 0;
		if (i == 4) {
			MARK(F_STEP_FRAME);
			p->at[F_STEP_TICKS] = p->len + 2;
			p->at[F_STEP_BOX] = p->len + 4;
			p->at[F_STEP_HURT] = p->len + 6;
		}
		if (i == 3) {
			p->at[F_ACTION_STEP_HIT] = p->len + 7;
			p->at[F_ACTION_STEP_VX] = p->len + 8;
			p->at[F_ACTION_STEP_SPAWN] = p->len + 10;
		}
		if (i == 11)
			p->at[F_MOVE_STEP_TICKS] = p->len + 2;
		if (i == 12) {
			p->at[F_RISE_VX] = p->len + 8;
			p->at[F_RISE_VY] = p->len + 9;
			p->at[F_RISE_SPAWN] = p->len + 10;
		}
		if (i == 13)
			p->at[F_LAND_VY] = p->len + 9;
		if (i == 14) {
			p->at[F_PROJ_STEP_HURT] = p->len + 6;
			p->at[F_PROJ_STEP_HIT] = p->len + 7;
			p->at[F_PROJ_STEP_VX] = p->len + 8;
		}
		put(p, i % SYN_FRAMES, 2);
		put(p, i >= 12 ? 2 : 1 + i % 4, 2);
		put(p, box, 2);
		put(p, hurt, 1);
		put(p, hit, 1);
		put(p, (unsigned)(uint8_t)vx, 1);
		put(p, (unsigned)(uint8_t)vy, 1);
		put(p, i == 12, 1);
		box += hurt + hit;
	}
	for (unsigned i = 0; i < SYN_STEPS; i++) {
		if (i == 2) {
			MARK(F_BOX_X);
			p->at[F_BOX_Y] = p->len + 2;
			p->at[F_BOX_W] = p->len + 4;
			p->at[F_BOX_H] = p->len + 6;
		}
		if (syn_hurts(i)) {
			put(p, (unsigned)(uint16_t)-10, 2);
			put(p, (unsigned)(uint16_t)(-20 - (int)i), 2);
			put(p, 20, 2);
			put(p, 20, 2);
		}
		if (syn_hits(i)) {
			put(p, 5, 2);
			put(p, (unsigned)(uint16_t)-15, 2);
			put(p, 12 + i, 2);
			put(p, 6, 2);
		}
	}
	static const unsigned effects[3][2] = {{2, 1}, {3, 0}, {4, 1}};
	for (int i = 0; i < 3; i++) {
		if (i == 2) {
			MARK(F_EFFECT_FRAME);
			p->at[F_EFFECT_PALETTE] = p->len + 2;
		}
		put(p, effects[i][0], 2);
		put(p, effects[i][1], 2);
	}
	seal(p);
}
static struct assets assets_a, assets_b;
static int load(struct assets *a, const struct pack *p)
{
	int automatic = -1;
	return fight_assets(a, p->data, p->len, &automatic);
}
static void malformed(void)
{
	static struct pack good, bad;
	build(&good, 0);
	assert(load(&assets_a, &good));
	struct {
		int field;
		unsigned value;
		int bytes;
	} cases[] = {
	    {F_FLAGS, 2, 4},		    {F_PALETTES, 0, 4},
	    {F_PALETTES, MAX_PALETTES + 1, 4}, {F_PAL_N, 0, 2},
	    {F_PAL_N, 257, 2},		    {F_PAL_N, 15, 2}, /* index 15 is used */
	    {F_PAL_SHEET, 1, 2},	    {F_FRAME_SHEET, 1, 2},
	    {F_FRAME_W, 0, 2},		    {F_FRAME_H, MAX_SIDE + 1, 2},
	    {F_ROW_COUNT, 9, 2},	    {F_SPAN_LEN, 0, 2},
	    {F_SPAN_X, 12, 2},		    {F_SPAN2_X, 0, 2},
	    {F_INDEX, 16, 1},		    {F_ACTION_COUNT, 0, 2},
	    {F_ACTION_FIRST, SYN_STEPS, 2},    {F_STEP_FRAME, SYN_FRAMES, 2},
	    {F_STEP_TICKS, 0, 2},	    {F_EFFECT_FRAME, SYN_FRAMES, 2},
	    {F_EFFECT_PALETTE, 3, 2},	    {F_EFFECT_PALETTE, 2, 2},
	    /* Characters and names. */
	    {F_CHARACTERS, 0, 4},	    {F_CHARACTERS, MAX_CHARACTERS + 1, 4},
	    {F_CHARACTERS, 2, 4},	    {F_NAME_LEN, 0, 1},
	    {F_NAME_LEN, MAX_NAME + 1, 1},  {F_NAME_CHAR, 'y', 1},
	    {F_NAME_CHAR, '_', 1},	    {F_CHAR_PAL0, 3, 2},
	    {F_CHAR_PAL1, 2, 2},	    {F_CHAR_STEPS, 0, 2},
	    {F_CHAR_STEPS, MAX_CHARACTER_STEPS + 1, 2}, {F_CHAR_BOXES, 99, 2},
	    {F_CHAR_MOVES, MOVES - 1, 2},   {F_CHAR_MOVES, MAX_MOVES + 1, 2},
	    {F_CHAR_MOVES, MOVES, 2},	    {F_CHAR_PROJECTILES, 0, 2},
	    {F_CHAR_PROJECTILES, MAX_PROJECTILE_TYPES + 1, 2},
	    /* Normals and specials. */
	    {F_MOVE_KIND, 1, 2},	    {F_MOVE_TRIGGER, MOVES, 2},
	    {F_MOVE_TRIGGER, STAND_LP, 2},  {F_MOVE_BUTTONS, 1, 2},
	    {F_MOVE_FIRST, SYN_STEPS - 3, 2}, {F_MOVE_COUNT, 0, 2},
	    {F_MOVE_DAMAGE, MAX_DAMAGE + 1, 2}, {F_MOVE_CHIP, 14, 2},
	    {F_MOVE_HITSTUN, 0, 2},	    {F_MOVE_HITSTUN, MAX_STUN + 1, 2},
	    {F_MOVE_BLOCKSTUN, 0, 2},	    {F_MOVE_BLOCKSTUN, MAX_STUN + 1, 2},
	    {F_MOVE_HITSTOP, MAX_HITSTOP + 1, 2}, {F_MOVE_PUSHBACK, MAX_PUSH + 1, 2},
	    {F_MOVE_BLOCK_PUSH, MAX_PUSH + 1, 2}, {F_MOVE_GUARD, GUARD_TYPES, 2},
	    {F_MOVE_CANCEL, CANCEL_MASK + 1, 2}, {F_SPECIAL_KIND, 0, 2},
	    {F_SPECIAL_MOTION, MOTIONS, 2},   {F_SPECIAL_BUTTONS, 0, 2},
	    {F_SPECIAL_BUTTONS, 16, 2},
	    /* Projectiles. */
	    {F_PROJ_FIRST, SYN_STEPS - 1, 2}, {F_PROJ_COUNT, 0, 2},
	    {F_PROJ_FIRST, 12, 2},	    {F_PROJ_SPEED, 0, 2},
	    {F_PROJ_SPEED, MAX_VELOCITY + 1, 2}, {F_PROJ_LIFETIME, 0, 2},
	    {F_PROJ_LIFETIME, MAX_LIFETIME + 1, 2}, {F_PROJ_X, MAX_SIDE + 1, 2},
	    {F_PROJ_Y, (uint16_t)-(MAX_SIDE + 1), 2}, {F_PROJ_DAMAGE, MAX_DAMAGE + 1, 2},
	    {F_PROJ_CHIP, 41, 2},	    {F_PROJ_HITSTUN, 0, 2},
	    {F_PROJ_BLOCKSTUN, MAX_STUN + 1, 2}, {F_PROJ_HITSTOP, MAX_HITSTOP + 1, 2},
	    {F_PROJ_PUSHBACK, MAX_PUSH + 1, 2}, {F_PROJ_BLOCK_PUSH, MAX_PUSH + 1, 2},
	    {F_PROJ_GUARD, GUARD_TYPES, 2},   {F_PROJ_BLEND, BLENDS, 2},
	    {F_PROJ_ALPHA, 0, 2},	    {F_PROJ_ALPHA, 256, 2},
	    /* Steps, motion and boxes. */
	    {F_STEP_BOX, 99, 2},	    {F_STEP_HURT, MAX_STEP_BOXES + 1, 1},
	    {F_STEP_BOX, 19, 2},	    {F_ACTION_STEP_HIT, 1, 1},
	    {F_ACTION_STEP_VX, 1, 1},	    {F_ACTION_STEP_SPAWN, 1, 1},
	    {F_MOVE_STEP_TICKS, MAX_MOVE_TICKS - 5, 2}, {F_RISE_VY, 2, 1},
	    {F_RISE_VY, (uint8_t)-1, 1},    {F_LAND_VY, 1, 1},
	    {F_RISE_VY, (uint8_t)-(MAX_VELOCITY + 1), 1}, {F_RISE_VX, MAX_VELOCITY + 1, 1},
	    {F_RISE_SPAWN, 2, 1},	    {F_PROJ_STEP_HURT, 1, 1},
	    {F_PROJ_STEP_HIT, 0, 1},	    {F_PROJ_STEP_VX, 1, 1},
	    {F_BOX_X, MAX_SIDE + 1, 2},	    {F_BOX_Y, (uint16_t)-(MAX_SIDE + 1), 2},
	    {F_BOX_W, 0, 2},		    {F_BOX_H, MAX_SIDE + 1, 2},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bad = good;
		set(&bad, good.at[cases[i].field], cases[i].value, cases[i].bytes);
		seal(&bad);
		if (load(&assets_a, &bad)) {
			fprintf(stderr, "malformed pack case %zu accepted\n", i);
			abort();
		}
	}
	/* Every truncation, with and without a consistent size and hash. */
	for (size_t n = 0; n < good.len; n++) {
		bad = good;
		bad.len = n;
		assert(!load(&assets_a, &bad));
		if (n >= 16) {
			seal(&bad);
			assert(!load(&assets_a, &bad));
		}
	}
	bad = good;
	bad.data[bad.len++] = 0;
	seal(&bad);
	assert(!load(&assets_a, &bad));
	for (char v = '1'; v <= '3'; v++) {
		bad = good;
		bad.data[7] = (uint8_t)v; /* FIGHTP01..03: retired formats */
		assert(!load(&assets_a, &bad));
	}
	bad = good;
	bad.data[good.at[F_INDEX] + 3] ^= 1;
	assert(!load(&assets_a, &bad));
	build(&bad, 1);
	int automatic = -1;
	assert(fight_assets(&assets_a, bad.data, bad.len, &automatic) && automatic == 1);
}
/* Resident images must reproduce the source bitmaps through the palettes. */
static void roundtrip(const struct assets *a, const struct decoded *dec,
		      const struct bitmap *src)
{
	for (unsigned i = 0; i < a->images; i++) {
		const struct image *im = &a->image[i];
		const struct bitmap *b = &dec->frame[im->frame];
		assert(im->w == b->w && im->h == b->h && im->ax == b->ax && im->ay == b->ay);
		if (src)
			assert(!memcmp(b->px, src[im->frame].px,
				       sizeof(int16_t) * (size_t)(b->w * b->h)));
		uint32_t count = 0;
		for (int y = 0; y < im->h; y++) {
			int x = 0;
			for (uint32_t k = a->row[im->first_row + y];
			     k < a->row[im->first_row + y + 1]; k++) {
				const struct span *s = &a->span[k];
				for (; x < s->x; x++)
					assert(b->px[y * b->w + x] < 0);
				for (unsigned j = 0; j < s->len; j++, x++, count++)
					assert(a->pixel[im->pixels + s->pixel + j] ==
					       dec->pal[im->palette][b->px[y * b->w + x]]);
			}
			for (; x < im->w; x++)
				assert(b->px[y * b->w + x] < 0);
		}
		assert(count == im->count);
	}
}
static void add(struct scene *s, int op, int x, int y, int w, int h, int image, int flip,
		int blend, uint16_t color, int alpha)
{
	assert(s->count < COMMANDS);
	s->cmd[s->count++] =
	    (struct command){op, x, y, w, h, image, flip, blend, color, (uint8_t)alpha};
}
static void sprite_at(struct scene *s, const struct assets *a, int image, int x, int y,
		      int flip, int blend, int alpha)
{
	add(s, SPRITE, x, y, a->image[image].w, a->image[image].h, image, flip, blend, 0,
	    alpha);
}
static void fixed_scenes(const struct assets *a, const struct decoded *dec)
{
	static struct scene s;
	static const int blends[][2] = {{OPAQUE, 255}, {OPAQUE, 128}, {ALPHA, 1},
					{ALPHA, 254},  {ALPHA, 0},    {ADD, 255},
					{ADD, 77},     {ADD, 1}};
	for (unsigned b = 0; b < sizeof(blends) / sizeof(blends[0]); b++)
		for (unsigned i = 0; i < a->images; i++) {
			const struct image *im = &a->image[i];
			s.count = 0;
			/* Every edge and corner, both orientations, partly and fully off. */
			const int xs[] = {-im->w + 1, -3, 0, FW / 2, FW - im->w, FW - 2, FW,
					  -im->w};
			const int ys[] = {-im->h + 1, -2, FH / 3, FH - 1, FH - im->h, FH};
			for (unsigned xi = 0; xi < 8; xi++)
				for (unsigned yi = 0; yi < 6; yi++)
					sprite_at(&s, a, (int)i, xs[xi], ys[yi], (xi + yi) & 1,
						  blends[b][0], blends[b][1]);
			add(&s, RECT, -5, -5, 20, 20, 0, 0, blends[b][0], 0xffff, blends[b][1]);
			add(&s, RECT, FW - 7, FH - 9, 30, 30, 0, 0, blends[b][0], 0xf81f,
			    blends[b][1]);
			add(&s, RECT, -100, 50, FW + 200, 3, 0, 0, blends[b][0], 0x07e0,
			    blends[b][1]);
			add(&s, RECT, 40, -50, 5, FH + 100, 0, 0, blends[b][0], 0x8410,
			    blends[b][1]);
			add(&s, RECT, 10, 10, 0, 5, 0, 0, blends[b][0], 0xffff, blends[b][1]);
			add(&s, RECT, 10, 10, 5, -5, 0, 0, blends[b][0], 0xffff, blends[b][1]);
			assert(compare(a, dec, &s, "fixed scene"));
		}
	/* Invalid images draw nothing in either renderer. */
	s.count = 0;
	add(&s, SPRITE, 10, 10, 5, 5, -1, 0, OPAQUE, 0, 255);
	add(&s, SPRITE, 10, 10, 5, 5, (int)a->images, 0, OPAQUE, 0, 255);
	assert(compare(a, dec, &s, "invalid images"));
}
static void random_scenes(const struct assets *a, const struct decoded *dec, int scenes,
			  const char *what)
{
	static struct scene s;
	for (int n = 0; n < scenes; n++) {
		s.count = 0;
		int commands = 1 + (int)(rnd() % 40);
		for (int k = 0; k < commands; k++) {
			int blend = (int)(rnd() % 3), alpha = (int)(rnd() % 4 ? rnd() % 256 : 255);
			if (rnd() % 3 == 0)
				add(&s, RECT, (int)(rnd() % 700) - 110, (int)(rnd() % 500) - 90,
				    (int)(rnd() % 300), (int)(rnd() % 200), 0, 0, blend,
				    (uint16_t)rnd(), alpha);
			else
				sprite_at(&s, a, (int)(rnd() % a->images),
					  (int)(rnd() % 1300) - 650, (int)(rnd() % 600) - 200,
					  (int)(rnd() & 1), blend, alpha);
		}
		assert(compare(a, dec, &s, what));
	}
}
/* Real scenes from game states: Auto ladder runs (fights, projectiles,
 * result and clear screens) at every stress level, the select screen,
 * flips and the KO. */
static void game_scenes(struct assets *a, const struct decoded *dec)
{
	static struct scene s;
	struct fight_state st;
	unsigned modes = 0, shots = 0;
	fight_reset(&st, 0);
	fight_cast(a, &st);
	fight_scene(&st, a, &s);
	assert(compare(a, dec, &s, "select scene"));
	fight_reset(&st, 1);
	for (unsigned t = 0; t < 12000; t++) {
		fight_step(&st, a, 0, 0, 0);
		fight_cast(a, &st);
		int shot = 0;
		for (int k = 0; k < MAX_PROJECTILES; k++)
			shot |= st.shot[k].kind != 0;
		if (t % 150 == 7 || (shot && shots < 4) || !((modes >> st.mode) & 1)) {
			shots += (unsigned)shot;
			modes |= 1u << st.mode;
			st.boxes = (t / 150) & 1;
			fight_scene(&st, a, &s);
			assert(compare(a, dec, &s, "game scene"));
		}
	}
	assert(shots >= 4 && (modes & (1u << MODE_FIGHT)) &&
	       (modes & (1u << MODE_RESULT | 1u << MODE_CLEAR)));
	fight_reset(&st, 0);
	fight_start(&st, 0, 0);
	fight_cast(a, &st);
	st.p[1].hp = 10;
	st.p[0].x = 240;
	st.p[1].x = 300;
	fight_step(&st, a, 1u << 1, 0, 1); /* Y: light punch, KO on the hit */
	for (int t = 0; t < 40; t++)
		fight_step(&st, a, 0, 0, 1);
	assert(st.ended && !st.p[1].hp);
	st.stress = 3;
	fight_scene(&st, a, &s);
	assert(compare(a, dec, &s, "KO scene"));
}
/* Moves, steps, boxes and projectile of the synthetic pack as the loader
 * derives them, and the cast it expands. */
static void synthetic_moves(struct assets *a)
{
	const struct character *ch = &a->chr[0];
	assert(a->characters == 1 && !strcmp(ch->name, "SYN") && ch->moves == MOVES + 1 &&
	       ch->projectiles == 1 && ch->palette[0] == 0 && ch->palette[1] == 1);
	/* Action HURT (5) is steps 5..7 (k % 6, 1 + k % 3), 2 + 3 + 4 ticks. */
	assert(ch->action[HURT].count == 3 && ch->action[HURT].total == 2 + 3 + 4);
	for (int t = 0; t < MOVES; t++) {
		const struct move *m = &ch->move[t];
		/* Steps 8..11 last 1, 2, 3, 4 ticks; 9 and 10 hit. */
		assert(m->anim.count == 4 && m->anim.total == 10 && m->startup == 1 &&
		       m->active == 5 && m->recovery == 4 && m->groups == 1 &&
		       m->reach == 5 + 12 + 10 && m->hit.damage == 10 + t && m->hit.chip == t &&
		       m->hit.hitstun == 5 + t && m->hit.blockstun == 3 + t &&
		       m->hit.hitstop == 2 && m->hit.pushback == 6 &&
		       m->hit.block_pushback == 8 && m->hit.guard == t % GUARD_TYPES &&
		       m->cancel == (t & CANCEL_MASK) && !m->special && !m->spawn);
		const struct step *st = &a->step[m->anim.first + 1];
		assert(st->hurt == 1 && st->hit == 1 && st->group == 1);
		const struct box *b = &a->box[st->box];
		assert(b[0].x == -10 && b[0].y == -29 && b[0].w == 20 && b[0].h == 20);
		assert(b[1].x == 5 && b[1].y == -15 && b[1].w == 21 && b[1].h == 6);
		assert(!a->step[m->anim.first].group && !a->step[m->anim.first + 3].group);
	}
	const struct move *sp = &ch->move[MOVES];
	const struct step *rise = &a->step[sp->anim.first];
	assert(sp->special && sp->motion == MOTION_236 &&
	       sp->buttons == (BUTTON_LP | BUTTON_HP) && sp->anim.total == 4 &&
	       sp->startup == 2 && sp->active == 2 && !sp->recovery && sp->spawn == 1 &&
	       rise->vx == 3 && rise->vy == -2 && rise->spawn == 1 && rise[1].vy == 2);
	const struct projectile *pr = &ch->projectile[0];
	assert(pr->anim.count == 2 && pr->anim.total == 4 && pr->speed == 5 &&
	       pr->lifetime == 50 && pr->x == 10 && pr->y == -5 && pr->hit.damage == 40 &&
	       pr->hit.chip == 9 && pr->hit.guard == GUARD_LOW && pr->blend == ALPHA &&
	       pr->alpha == 200 && !a->step[pr->anim.first].hurt);
	/* The cast follows the state: normal and alternate palette images of
	 * every frame the character uses (all five), recast only on change. */
	struct fight_state s;
	fight_reset(&s, 0);
	assert(a->cast[0][0] == -1 && fight_cast(a, &s) && !fight_cast(a, &s));
	assert(a->images == a->effect_images + 2 * SYN_FRAMES &&
	       a->cast_pixels[0] == ch->pixels && a->cast_pixels[1] == ch->pixels);
	for (int i = 0; i < 2; i++)
		for (unsigned f = 0; f < SYN_FRAMES; f++) {
			const struct image *im = &a->image[a->look[i][f]];
			assert(a->look[i][f] >= 0 && im->frame == f && im->palette == (unsigned)i);
		}
	s.pal[1] = 0;
	assert(fight_cast(a, &s) && a->image[a->look[1][0]].palette == 0);
	/* Mirroring: right-facing box at anchor 100 covers 90..109; left-facing
	 * mirrors around the anchor pixel boundary like the sprite does. */
	const struct box b = {-10, -29, 20, 20}, hit = {5, -15, 21, 6};
	struct area r = fight_box(&b, 100, -7, 1), l = fight_box(&b, 100, -7, -1),
		    hr = fight_box(&hit, 100, 0, 1), hl = fight_box(&hit, 100, 0, -1);
	assert(r.x == 90 && r.y == GROUND - 36 && r.w == 20 && r.h == 20);
	assert(l.x == 90 && l.y == r.y);
	assert(hr.x == 105 && hl.x == 100 - 5 - 21 && hl.w == 21 && hl.y == GROUND - 15);
}
/* Every real step's boxes cover the same sprite pixels in both facings.
 * The idle step is pointed at each frame in turn so fight_scene places the
 * sprite itself; opaque pixels are those that match on two backgrounds. */
static void box_flip(struct assets *a)
{
	static struct scene s;
	static uint16_t fb[2][2][FW * FH];
	struct fight_state st;
	fight_reset(&st, 0);
	fight_start(&st, 0, 0);
	fight_cast(a, &st);
	unsigned checked = 0, idle = a->chr[0].action[IDLE].first;
	uint16_t saved = a->step[idle].frame;
	const struct character *c = &a->chr[0];
	for (unsigned k = c->first_step; k < c->first_step + c->steps; k++) {
		const struct step *step = &a->step[k];
		if (!step->hurt && !step->hit)
			continue;
		a->step[idle].frame = step->frame;
		for (int f = 0; f < 2; f++) {
			fight_reset(&st, 0);
			fight_start(&st, 0, 0);
			st.p[0].x = 240;
			st.p[0].face = f ? -1 : 1;
			assert(fight_pose(&st, a, 0) == idle);
			fight_scene(&st, a, &s);
			unsigned cmd = 0;
			while (cmd < s.count && s.cmd[cmd].op != SPRITE)
				cmd++;
			assert(cmd < s.count && s.cmd[cmd].image == a->look[0][step->frame] &&
			       s.cmd[cmd].flip == f);
			s.cmd[0] = s.cmd[cmd];
			s.count = 1;
			for (int bg = 0; bg < 2; bg++) {
				for (int i = 0; i < FW * FH; i++)
					fb[f][bg][i] = bg ? 0xffff : 0x0000;
				fight_render(a, &s, fb[f][bg]);
			}
		}
		unsigned covered = 0;
		for (unsigned b = 0; b < (unsigned)step->hurt + step->hit; b++) {
			struct area r = fight_box(&a->box[step->box + b], 240, 0, 1),
				    l = fight_box(&a->box[step->box + b], 240, 0, -1);
			assert(r.w == l.w && r.h == l.h && r.y == l.y);
			for (int y = r.y; y < r.y + r.h; y++)
				for (int x = r.x; x < r.x + r.w; x++) {
					int mx = l.x + (r.x + r.w - 1 - x);
					if (x < 0 || x >= FW || mx < 0 || mx >= FW || y < 0 ||
					    y >= FH)
						continue;
					int i = y * FW + x, j = y * FW + mx;
					int opaque = fb[0][0][i] == fb[0][1][i];
					assert(opaque == (fb[1][0][j] == fb[1][1][j]));
					if (opaque)
						assert(fb[0][0][i] == fb[1][0][j]);
					covered += (unsigned)opaque;
				}
			checked++;
		}
		/* Hurtboxes sit on the body, projectile hitboxes on the effect:
		 * some sprite pixels in every pose. */
		assert(covered);
	}
	a->step[idle].frame = saved;
	assert(checked > 200);
}
/* The L overlay draws every hurt and hit box of the shown steps. */
static void overlay(const struct assets *a)
{
	static struct scene s, plain;
	struct fight_state st;
	fight_reset(&st, 0);
	fight_start(&st, 0, 0);
	st.p[0].x = 200;
	st.p[1].x = 260;
	for (int t = 0; t < 4; t++)
		fight_step(&st, a, t ? 0 : 1u << 1, 0, 1); /* stand LP, elapsed 3 */
	assert(st.p[0].move == STAND_LP + 1 && st.p[0].freeze);
	fight_scene(&st, a, &plain);
	st.boxes = 1;
	fight_scene(&st, a, &s);
	unsigned want = 0, red = 0;
	for (int i = 0; i < 2; i++) {
		const struct step *step = &a->step[fight_pose(&st, a, i)];
		want += 4u * step->hurt + 5u * step->hit + 1;
		if (!i)
			assert(step->hit == 1);
	}
	for (unsigned c = 0; c < s.count; c++)
		red += s.cmd[c].op == RECT && s.cmd[c].color == 0xf800;
	assert(s.count == plain.count + want && red == 5);
	/* Stand LP's hitbox [28, -89, 26, 15] from the manifest, facing right. */
	const struct step *lp = &a->step[fight_pose(&st, a, 0)];
	struct area h = fight_box(&a->box[lp->box + lp->hurt], 200, 0, 1);
	assert(h.x == 228 && h.y == GROUND - 89 && h.w == 26 && h.h == 15);
}
/* Effects beyond the per-frame pixel budget are dropped, in order. */
static void effect_cap(const struct assets *a)
{
	static struct scene s;
	struct fight_state st;
	fight_reset(&st, 0);
	fight_start(&st, 0, 0);
	static const unsigned want[4] = {0, 8, 24, 48};
	for (uint32_t tick = 0; tick < 64; tick += 7)
		for (unsigned m = 0; m < 4; m++) {
			st.stress = m;
			st.tick = tick;
			fight_scene(&st, a, &s);
			uint32_t pixels = 0;
			unsigned blended = 0;
			for (unsigned c = 0; c < s.count; c++)
				if (s.cmd[c].op == SPRITE && s.cmd[c].blend != OPAQUE) {
					pixels += a->image[s.cmd[c].image].count;
					blended++;
				}
			assert(s.effects_requested == want[m] && s.effects_drawn == blended);
			assert(pixels <= EFFECT_PIXEL_BUDGET);
			if (m == 1)
				assert(blended == 8);
			if (m >= 2)
				assert(blended >= 10 && blended < want[m]);
		}
}
static void bench(const struct assets *a)
{
	static struct scene s;
	static uint16_t fb[FW * FH];
	struct fight_state st;
	fight_reset(&st, 0);
	fight_start(&st, 0, 0);
	for (int i = 0; i < 300; i++)
		fight_step(&st, a, 0, 0, 0);
	for (unsigned m = 0; m < 4; m++) {
		st.stress = m;
		fight_scene(&st, a, &s);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < 2000; i++)
			fight_render(a, &s, fb);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("fighter-bench: scene %u render %.1f us/frame (%u commands)\n", m,
		       ((double)(t1.tv_sec - t0.tv_sec) * 1e9 +
			(double)(t1.tv_nsec - t0.tv_nsec)) /
			   2000 / 1000,
		       s.count);
	}
}
void render_tests(const uint8_t *real, size_t real_len)
{
	static struct pack syn_pack;
	static struct decoded dec;
	make_bitmaps();
	malformed();
	build(&syn_pack, 0);
	assert(load(&assets_b, &syn_pack));
	synthetic_moves(&assets_b);
	decode(syn_pack.data, syn_pack.len, &dec);
	roundtrip(&assets_b, &dec, syn);
	fixed_scenes(&assets_b, &dec);
	random_scenes(&assets_b, &dec, 300, "random synthetic scene");
	release(&dec);
	int automatic;
	assert(fight_assets(&assets_a, real, real_len, &automatic));
	struct fight_state st;
	fight_reset(&st, 0);
	fight_start(&st, 0, 0);
	assert(fight_cast(&assets_a, &st));
	decode(real, real_len, &dec);
	roundtrip(&assets_a, &dec, NULL);
	random_scenes(&assets_a, &dec, 100, "random real scene");
	game_scenes(&assets_a, &dec);
	release(&dec);
	box_flip(&assets_a);
	overlay(&assets_a);
	effect_cap(&assets_a);
	if (getenv("FIGHTER_BENCH"))
		bench(&assets_a);
}
