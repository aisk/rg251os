/* Renderer and pack-decoder regression for the fighter core, linked into the
 * callback harness (test.c). It writes its own FIGHTP08 packs from known
 * bitmaps, two system profiles, two spark anims and a three-layer stage,
 * checks that malformed or truncated packs (including bad systems, stats,
 * classes, class defaults, sparks, walls, effect sets, slash and home
 * fields and stages, layers and stage anims) are rejected, that stats
 * resolve from the system plus overrides, that class defaults fill
 * hitstun/blockstun/hitstop and the damage scale applies, and compares
 * fight_render pixel for pixel with an independent per-pixel reference
 * (straight formulas, divisions, no spans; stage layer images through
 * their own edge rules) on fixed edge scenes, seeded random scenes and real
 * game scenes (with stages, sparks, projectiles and the select and ladder
 * screens). The synthetic stage is also drawn per screen pixel from its
 * layers (parallax, every edge rule, anims) for many cameras, shakes and
 * ticks. It also rejects malformed palette, frame, name, box, step,
 * motion, move, special, follow-up and projectile data, checks the loader's
 * derived frame data, the render cache (its regions, the select screen
 * wanting side 0 only, resident images following the state's fighters,
 * palettes and stage; a lazily filled cache with a per-run budget and
 * on-demand frames matches the fully expanded one pixel for pixel over
 * select cursor moves and a long Auto run, never exceeds its slice and is
 * complete when the versus card ends), that boxes mirror with the sprite
 * pixel for pixel, the L box overlay and the effect pixel budget.
 * FIGHTER_BENCH=1 also prints host render times and written pixels per
 * stress scene and per stage and the cost of a full cast; host numbers only
 * show relative change. */
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
	size_t at = 48;
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
/* Edge rule applied to layer coordinate v of an axis n long, written with
 * loops instead of the loader's modulo: -1 transparent, -2 fill colour. */
static int ref_edge(int v, int n, int edge)
{
	if (v >= 0 && v < n)
		return v;
	switch (edge) {
	case EDGE_EXTEND:
		return v < 0 ? 0 : n - 1;
	case EDGE_REPEAT:
		while (v < 0)
			v += n;
		while (v >= n)
			v -= n;
		return v;
	case EDGE_MIRROR:
		while (v < 0)
			v += 2 * n;
		while (v >= 2 * n)
			v -= 2 * n;
		return v < n ? v : 2 * n - 1 - v;
	case EDGE_COLOR:
		return -2;
	default:
		return -1;
	}
}
/* Colour of layer l at layer pixel (u, v), -1 where transparent. The frame
 * sits at minus its anchor inside the layer rectangle. */
static int ref_layer(const struct decoded *dec, const struct layer *l, int u, int v)
{
	int x = ref_edge(u, l->w, l->edge[0]), y = ref_edge(v, l->h, l->edge[1]);
	if (x == -1 || y == -1)
		return -1;
	if (x == -2 || y == -2)
		return l->color;
	const struct bitmap *b = &dec->frame[l->frame];
	x += b->ax;
	y += b->ay;
	if (x < 0 || x >= b->w || y < 0 || y >= b->h || b->px[y * b->w + x] < 0)
		return -1;
	return dec->pal[l->palette][b->px[y * b->w + x]];
}
/* The cast stage layer whose expanded image is image, or -1. */
static int layer_of(const struct assets *a, int image)
{
	if (a->cast_stage < 0)
		return -1;
	const struct stage *st = &a->stage[a->cast_stage];
	for (unsigned i = 0; i < st->layers; i++)
		if (a->layer_image[i] == image)
			return (int)i;
	return -1;
}
static void reference(const struct assets *a, const struct decoded *dec,
		      const struct scene *s, uint16_t *fb)
{
	for (unsigned k = 0; k < s->count; k++) {
		const struct command *c = &s->cmd[k];
		const struct bitmap *b = NULL;
		const struct layer *l = NULL;
		const uint16_t *pal = NULL;
		int w = c->w, h = c->h, layer = -1;
		if (c->op == SPRITE) {
			if (c->image < 0 || (unsigned)c->image >= a->images)
				continue;
			const struct image *im = &a->image[c->image];
			/* An empty cache slot draws nothing. */
			if (!im->w)
				continue;
			layer = layer_of(a, c->image);
			if (layer >= 0) {
				l = &a->layer[a->stage[a->cast_stage].first_layer + (unsigned)layer];
				w = im->w;
				h = im->h;
			} else {
				b = &dec->frame[im->frame];
				pal = dec->pal[im->palette];
				w = b->w;
				h = b->h;
			}
		}
		int y0 = c->y > 0 ? c->y : 0, x0 = c->x > 0 ? c->x : 0;
		for (int y = y0; y < FH && y < c->y + h; y++)
			for (int x = x0; x < FW && x < c->x + w; x++) {
				int lx = x - c->x, ly = y - c->y;
				if (!c->alpha)
					continue;
				uint16_t src = c->color;
				int sx = c->flip ? w - 1 - lx : lx;
				if (l) {
					int v = ref_layer(dec, l, a->layer_x0[layer] + sx,
							  a->layer_y0[layer] + ly);
					if (v < 0)
						continue;
					src = (uint16_t)v;
				} else if (b) {
					int v = b->px[ly * w + sx];
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
	F_MOVE_BLOCK_PUSH, F_MOVE_GUARD, F_MOVE_CANCEL, F_MOVE_CLASS, F_MOVE_DEFAULTS,
	F_SPECIAL_KIND, F_SPECIAL_MOTION,
	F_SPECIAL_BUTTONS, F_MOVE_AFTER, F_SPECIAL_AFTER, F_SPECIAL_OPEN, F_FOLLOW_MOTION,
	F_MOVE_NAME, F_SPECIAL_NAME, F_SPECIAL_NAME_CHAR,
	F_FOLLOW_AFTER, F_FOLLOW_OPEN, F_FOLLOW_CLOSE, F_FOLLOW_CONTACT, F_PROJ_FIRST, F_PROJ_COUNT, F_PROJ_SPEED, F_PROJ_LIFETIME,
	F_PROJ_X, F_PROJ_Y, F_PROJ_DAMAGE, F_PROJ_CHIP, F_PROJ_HITSTUN, F_PROJ_BLOCKSTUN,
	F_PROJ_HITSTOP, F_PROJ_PUSHBACK, F_PROJ_BLOCK_PUSH, F_PROJ_GUARD, F_PROJ_BLEND,
	F_PROJ_ALPHA, F_PROJ_CLASS, F_PROJ_DEFAULTS, F_STEP_FRAME, F_STEP_TICKS, F_STEP_BOX, F_STEP_HURT, F_ACTION_STEP_HIT,
	F_ACTION_STEP_VX, F_ACTION_STEP_SPAWN, F_MOVE_STEP_TICKS, F_RISE_VY, F_LAND_VY,
	F_RISE_VX, F_RISE_SPAWN, F_PROJ_STEP_HURT, F_PROJ_STEP_HIT, F_PROJ_STEP_VX,
	F_BOX_X, F_BOX_Y, F_BOX_W, F_BOX_H, F_EFFECT_FRAME, F_EFFECT_PALETTE,
	/* Systems (the first one), stats and class defaults. */
	F_SYSTEMS, F_SYS_NAME_LEN, F_SYS_NAME_CHAR, F_SYS_SCALE, F_SYS_LIFE, F_SYS_DEFENSE,
	F_SYS_WALK, F_SYS_WALK_BACK, F_SYS_JUMP_VY, F_SYS_GRAVITY, F_SYS_VX, F_SYS_VX_BACK,
	F_SYS_PREJUMP, F_SYS_WEIGHT, F_SYS_HEIGHT, F_SYS_PUSH, F_SYS_GUARD_RANGE, F_SYS_BUFFER,
	F_SYS_QUARTER, F_SYS_DRAGON, F_SYS_CHARGE, F_SYS_GRACE, F_SYS_RELEASE,
	F_SYS_LIGHT_HITSTUN, F_SYS_LIGHT_BLOCKSTUN, F_SYS_LIGHT_HITSTOP, F_SYS_LIGHT_HAS,
	F_SYS_THROW_HITSTUN, F_SYS_THROW_HAS, F_CHAR_SYSTEM, F_STATS_MASK, F_STAT_LIFE,
	F_STAT_WALK, F_STAT_HEIGHT, F_DEF_HITSTUN, F_DEF_CLASS, F_DEF_DEFAULTS,
	/* Sparks (the first one), walls and effect sets, slash and home. */
	F_SPARKS, F_STAGES, F_SPARK_BLEND, F_SPARK_ALPHA, F_SPARK_STEPS, F_SPARK_FRAME,
	F_SPARK_PALETTE, F_SPARK_TICKS, F_SYS_MARGIN, F_SYS_PLAY, F_SYS_EFFECT, F_CHAR_SLASH,
	F_CHAR_HOME,
	/* The stage, its first layer and that layer's first anim step. */
	F_STAGE_NAME_LEN, F_STAGE_NAME_CHAR, F_STAGE_WIDTH, F_STAGE_GROUND, F_STAGE_SYSTEM,
	F_STAGE_LAYERS, F_LAYER_FRAME, F_LAYER_PALETTE, F_LAYER_W, F_LAYER_H, F_LAYER_X,
	F_LAYER_Y, F_LAYER_NUM, F_LAYER_DEN, F_LAYER_EX, F_LAYER_EY, F_LAYER_COLOR,
	F_LAYER_ANIMS, F_ANIM_STEPS, F_ASTEP_FRAME, F_ASTEP_X, F_ASTEP_Y, F_ASTEP_TICKS, FIELDS
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
/* Frames 0..4 are the character's, 5 the stage layer art (at 2,3 inside
 * its layers) and 6 a stage anim frame. */
#define SYN_FRAMES 5
#define SYN_ALL 7
static struct bitmap syn[SYN_ALL];
static int16_t syn_px[SYN_ALL][4096];
static const int syn_size[SYN_ALL][4] = {{13, 7, 6, 7},	   {1, 1, 0, 1},  {40, 30, 20, 30},
					 {600, 3, 300, 3}, {17, 64, -5, 70}, {48, 40, -2, -3},
					 {8, 6, 0, 0}};
static void make_bitmaps(void)
{
	for (int f = 0; f < SYN_ALL; f++) {
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
				    (f == 3 && x % 50 > 44) || (f == 4 && x == 16 && y < 63) ||
				    (f == 5 && ((x + y) % 9 == 0 || (x > 30 && y > 25))) ||
				    (f == 6 && x == y))
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
/* Two systems: "kof" (scale 1000, KOF 2002 numbers) and "ss" (scale 800).
 * Per system: damage scale, the 11 stats, push, guard range, the six input
 * windows and the light .. throw class rows (hitstun, blockstun, hitstop,
 * has); command has no hitstop, throw nothing. */
static const unsigned syn_sys[2][1 + 11 + 8] = {
    {1000, 1000, 1000, 7, 6, (uint16_t)-48, 3, 8, 8, 3, 1000, HEIGHT_MID, 46, 160, 4, 15, 20, 30,
     10, 4},
    {800, 1000, 1000, 6, 5, (uint16_t)-42, 2, 7, 7, 4, 1000, HEIGHT_MID, 46, 160, 4, 15, 20, 30, 10,
     4}};
static const unsigned syn_class[2][CLASSES - 1][4] = {
    {{11, 9, 6, 3}, {19, 17, 9, 3}, {19, 17, 0, 1}, {19, 17, 10, 3}, {19, 17, 12, 3}, {0, 0, 0, 0}},
    {{12, 10, 7, 3}, {22, 20, 12, 3}, {20, 18, 11, 3}, {20, 18, 11, 3}, {22, 20, 14, 3}, {0, 0, 0, 0}}};
/* Walls (margin, play width) and effect sets: spark + 1 per FX_ key. */
static const unsigned syn_fx[2][FX_KEYS] = {{1, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0},
					    {0, 0, 0, 0, 2, 2, 2, 1, 2, 2, 2}};
static void systems(struct pack *p)
{
	for (int k = 0; k < 2; k++) {
		if (!k)
			MARK(F_SYS_NAME_LEN);
		put(p, k ? 2 : 3, 1);
		if (!k)
			MARK(F_SYS_NAME_CHAR);
		for (const char *c = k ? "ss" : "kof"; *c; c++)
			put(p, (unsigned char)*c, 1);
		for (int j = 0; j < 20; j++) {
			if (!k)
				p->at[F_SYS_SCALE + j] = p->len;
			put(p, syn_sys[k][j], 2);
		}
		for (int c = 0; c < CLASSES - 1; c++)
			for (int j = 0; j < 4; j++) {
				if (!k && !c)
					p->at[F_SYS_LIGHT_HITSTUN + j] = p->len;
				if (!k && c == CLASS_THROW - 1 && (j == 0 || j == 3))
					p->at[j ? F_SYS_THROW_HAS : F_SYS_THROW_HITSTUN] = p->len;
				put(p, syn_class[k][c][j], 2);
			}
		if (!k)
			MARK(F_SYS_MARGIN), p->at[F_SYS_PLAY] = p->len + 2;
		put(p, k ? 16 : 24, 2);
		put(p, k ? 400 : FW, 2);
		for (int j = 0; j < FX_KEYS; j++) {
			if (!k && !j)
				MARK(F_SYS_EFFECT);
			put(p, syn_fx[k][j], 2);
		}
	}
}
/* One character "SYN" on system 0 with its own walk 10 and height high:
 * steps 0..7 for the actions, 8..11 for every normal (9 and 10 hit; crouch
 * HK is class heavy and takes hitstun, blockstun and hitstop from it),
 * 12..13 for one 236+P special that rises 4 px for 2 ticks, lands, hits on
 * 13 and launches the projectile on 12; 14..15 are the projectile's
 * (hitboxes only, class special with its hitstop from the class). Weapon
 * moves are the LP-button normals and the special; its home stage is DOJO.
 * Two palettes on sheet 0, plus a third on sheet 1 that no frame uses. */
#define SYN_STEPS 16
/* Spark 0 opaque: frame 0 palette 0 for 2 ticks, frame 1 palette 1 for 3;
 * spark 1 additive 200: frame 4 for 4 ticks. */
static void sparks(struct pack *p)
{
	MARK(F_SPARK_BLEND);
	put(p, OPAQUE, 1);
	MARK(F_SPARK_ALPHA);
	put(p, 255, 1);
	MARK(F_SPARK_STEPS);
	put(p, 2, 2);
	MARK(F_SPARK_FRAME);
	put(p, 0, 2);
	MARK(F_SPARK_PALETTE);
	put(p, 0, 2);
	MARK(F_SPARK_TICKS);
	put(p, 2, 2);
	put(p, 1, 2);
	put(p, 1, 2);
	put(p, 3, 2);
	put(p, ADD, 1);
	put(p, 200, 1);
	put(p, 1, 2);
	put(p, 4, 2);
	put(p, 0, 2);
	put(p, 4, 2);
}
/* Stage "DOJO", 600 wide, ground row 200, walls from system "ss". Layers
 * (frame 5 each): 0 at y -20 moving cam/3, repeated across and filled
 * 0x1234 above and below, with a two-step anim (frame 6 at 4,5 for 3
 * ticks, then nothing for 2); 1 at x 100, y 60 fixed to the world,
 * extended across and clipped vertically, palette 1; 2 at x -30, y 100
 * moving against the camera (-cam/2), mirrored across and extended
 * vertically, with frame 6 at -3,40 always. */
struct syn_layer {
	unsigned palette, w, h;
	int x, y, num;
	unsigned den, ex, ey, color, anims;
};
static const struct syn_layer syn_layers[3] = {
    {0, 52, 45, 0, -20, 1, 3, EDGE_REPEAT, EDGE_COLOR, 0x1234, 1},
    {1, 50, 44, 100, 60, 1, 1, EDGE_EXTEND, EDGE_NONE, 0, 0},
    {0, 50, 43, -30, 100, -1, 2, EDGE_MIRROR, EDGE_EXTEND, 0, 1}};
static void stage(struct pack *p)
{
	MARK(F_STAGE_NAME_LEN);
	put(p, 4, 1);
	MARK(F_STAGE_NAME_CHAR);
	for (const char *c = "DOJO"; *c; c++)
		put(p, (unsigned char)*c, 1);
	MARK(F_STAGE_WIDTH);
	put(p, 600, 2);
	MARK(F_STAGE_GROUND);
	put(p, 200, 2);
	MARK(F_STAGE_SYSTEM);
	put(p, 1, 2);
	MARK(F_STAGE_LAYERS);
	put(p, 3, 2);
	for (int i = 0; i < 3; i++) {
		const struct syn_layer *l = &syn_layers[i];
		const unsigned v[12] = {5,
					l->palette,
					l->w,
					l->h,
					(uint16_t)l->x,
					(uint16_t)l->y,
					(uint16_t)l->num,
					l->den,
					l->ex,
					l->ey,
					l->color,
					l->anims};
		for (int j = 0; j < 12; j++) {
			if (!i)
				p->at[F_LAYER_FRAME + j] = p->len;
			put(p, v[j], j == 8 || j == 9 ? 1 : 2);
		}
		if (!l->anims)
			continue;
		if (!i)
			MARK(F_ANIM_STEPS);
		put(p, i ? 1 : 2, 2);
		if (!i)
			MARK(F_ASTEP_FRAME), p->at[F_ASTEP_X] = p->len + 2,
					     p->at[F_ASTEP_Y] = p->len + 4,
					     p->at[F_ASTEP_TICKS] = p->len + 6;
		put(p, 6, 2);
		put(p, (uint16_t)(i ? -3 : 4), 2);
		put(p, i ? 40 : 5, 2);
		put(p, i ? 5 : 3, 2);
		if (i)
			continue;
		put(p, 0xffff, 2);
		put(p, 0, 2);
		put(p, 0, 2);
		put(p, 2, 2);
	}
}
static int syn_hits(unsigned i) { return i == 9 || i == 10 || i >= 13; }
static int syn_hurts(unsigned i) { return i < 14; }
static void build(struct pack *p, int automatic)
{
	memset(p, 0, sizeof(*p));
	memcpy(p->data, "FIGHTP08", 8);
	p->len = 16;
	MARK(F_FLAGS);
	put(p, (unsigned)automatic, 4);
	MARK(F_PALETTES);
	put(p, 3, 4);
	put(p, SYN_ALL, 4);
	MARK(F_CHARACTERS);
	put(p, 1, 4); /* characters */
	put(p, 3, 4); /* effects */
	MARK(F_SYSTEMS);
	put(p, 2, 4);
	MARK(F_SPARKS);
	put(p, 2, 4);
	MARK(F_STAGES);
	put(p, 1, 4);
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
	for (int f = 0; f < SYN_ALL; f++) {
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
	sparks(p);
	systems(p);
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
	MARK(F_CHAR_SYSTEM);
	put(p, 0, 2);
	MARK(F_STATS_MASK);
	put(p, 1u << 2 | 1u << 10, 2); /* walk, height */
	for (int k = 0; k < 11; k++) {
		if (k == 0)
			MARK(F_STAT_LIFE);
		if (k == 2)
			MARK(F_STAT_WALK);
		if (k == 10)
			MARK(F_STAT_HEIGHT);
		put(p, k == 2 ? 10 : k == 10 ? HEIGHT_HIGH : 0, 2);
	}
	MARK(F_CHAR_STEPS);
	put(p, SYN_STEPS, 2);
	MARK(F_CHAR_BOXES);
	put(p, boxes, 2);
	MARK(F_CHAR_MOVES);
	put(p, MOVES + 2, 2);
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
	for (unsigned t = 0; t <= MOVES + 1; t++) {
		/* kind, trigger, buttons, first, count, damage, chip, hitstun,
		 * blockstun, hitstop, pushback, block pushback, guard, cancel,
		 * class, defaults, after, open, close, contact; crouch HK takes
		 * its stuns from class heavy. The second special is a follow-up
		 * of the first (forward + HK during its ticks 1..2, on contact). */
		int heavy = t == CROUCH_HK, follow = t == MOVES + 1;
		const unsigned v[20] = {t >= MOVES,
					follow	    ? HOLD_6
					: t == MOVES ? MOTION_236
						     : t,
					follow	    ? BUTTON_HK
					: t == MOVES ? BUTTON_LP | BUTTON_HP
						     : 0,
					t >= MOVES ? 12 : 8,
					t >= MOVES ? 2 : 4,
					10 + t, t, heavy ? 0 : 5 + t, heavy ? 0 : 3 + t, heavy ? 0 : 2, 6, 8,
					t % GUARD_TYPES, t & CANCEL_MASK,
					heavy ? CLASS_HEAVY : t >= MOVES ? CLASS_SPECIAL : CLASS_NONE,
					heavy ? 7 : 0, follow ? MOVES + 1 : 0, follow, follow * 2, follow};
		if (t == STAND_HK) {
			for (int j = 0; j < 16; j++)
				p->at[F_MOVE_KIND + j] = p->len + 2 * (size_t)j;
			p->at[F_MOVE_AFTER] = p->len + 32;
		}
		if (t == MOVES) {
			for (int j = 0; j < 3; j++)
				p->at[F_SPECIAL_KIND + j] = p->len + 2 * (size_t)j;
			p->at[F_SPECIAL_AFTER] = p->len + 32;
			p->at[F_SPECIAL_OPEN] = p->len + 34;
		}
		if (follow) {
			p->at[F_FOLLOW_MOTION] = p->len + 2;
			for (int j = 0; j < 4; j++)
				p->at[F_FOLLOW_AFTER + j] = p->len + 32 + 2 * (size_t)j;
		}
		if (heavy) {
			p->at[F_DEF_HITSTUN] = p->len + 14;
			p->at[F_DEF_CLASS] = p->len + 28;
			p->at[F_DEF_DEFAULTS] = p->len + 30;
		}
		for (int j = 0; j < 20; j++)
			put(p, v[j], 2);
		/* Names: none for normals, "SP-1" and "FU 2" for the specials. */
		if (t == STAND_HK)
			MARK(F_MOVE_NAME);
		if (t == MOVES)
			MARK(F_SPECIAL_NAME);
		put(p, t >= MOVES ? 4 : 0, 1);
		for (int j = 0; t >= MOVES && j < 4; j++) {
			if (t == MOVES && j == 2)
				MARK(F_SPECIAL_NAME_CHAR);
			put(p, (unsigned char)(follow ? "FU 2" : "SP-1")[j], 1);
		}
	}
	/* first, count, speed, lifetime, x, y, damage, chip, hitstun,
	 * blockstun, hitstop (from class special), pushback, block pushback,
	 * guard, blend, alpha, class, defaults */
	const unsigned proj[18] = {14, 2, 5, 50, 10, (uint16_t)-5, 40, 9, 12, 9, 0, 10, 12,
				   GUARD_LOW, ALPHA, 200, CLASS_SPECIAL, DEFAULT_HITSTOP};
	for (int j = 0; j < 18; j++) {
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
	MARK(F_CHAR_SLASH);
	put(p, 1u | 1u << 4, 2); /* LP-button normals and the special */
	MARK(F_CHAR_HOME);
	put(p, 1, 2);
	static const unsigned effects[3][2] = {{2, 1}, {3, 0}, {4, 1}};
	for (int i = 0; i < 3; i++) {
		if (i == 2) {
			MARK(F_EFFECT_FRAME);
			p->at[F_EFFECT_PALETTE] = p->len + 2;
		}
		put(p, effects[i][0], 2);
		put(p, effects[i][1], 2);
	}
	stage(p);
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
	    {F_ACTION_FIRST, SYN_STEPS, 2},    {F_STEP_FRAME, SYN_ALL, 2},
	    {F_STEP_TICKS, 0, 2},	    {F_EFFECT_FRAME, SYN_ALL, 2},
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
	    /* Follow-ups: only specials, never their own parent, held directions
	     * only with a parent, the window inside the parent's 4 ticks. */
	    {F_MOVE_AFTER, MOVES + 1, 2},   {F_SPECIAL_AFTER, MOVES + 1, 2},
	    {F_SPECIAL_OPEN, 1, 2},	    {F_SPECIAL_MOTION, MOTION_ANY, 2},
	    {F_FOLLOW_MOTION, MOTIONS, 2},  {F_FOLLOW_AFTER, 0, 2},
	    {F_MOVE_NAME, 1, 1},	    {F_SPECIAL_NAME, 0, 1},
	    {F_SPECIAL_NAME, MAX_MOVE_NAME + 1, 1}, {F_SPECIAL_NAME_CHAR, 'a', 1},
	    {F_SPECIAL_NAME_CHAR, '.', 1},
	    {F_FOLLOW_AFTER, MOVES + 3, 2}, {F_FOLLOW_AFTER, MOVES + 2, 2},
	    {F_FOLLOW_OPEN, 3, 2},	    {F_FOLLOW_CLOSE, 4, 2},
	    {F_FOLLOW_CLOSE, MAX_MOVE_TICKS, 2}, {F_FOLLOW_CONTACT, 2, 2},
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
	    /* Systems. */
	    {F_SYSTEMS, 0, 4},		    {F_SYSTEMS, MAX_SYSTEMS + 1, 4},
	    {F_SYSTEMS, 1, 4},		    {F_SYSTEMS, 3, 4},
	    {F_SYS_NAME_LEN, 0, 1},	    {F_SYS_NAME_LEN, MAX_NAME + 1, 1},
	    {F_SYS_NAME_CHAR, 'K', 1},	    {F_SYS_SCALE, MIN_SCALE - 1, 2},
	    {F_SYS_SCALE, MAX_PERMILLE + 1, 2}, {F_SYS_LIFE, 0, 2},
	    {F_SYS_LIFE, MAX_LIFE + 1, 2},  {F_SYS_DEFENSE, MIN_DEFENSE - 1, 2},
	    {F_SYS_WALK, MAX_WALK + 1, 2},  {F_SYS_WALK_BACK, 0x8000, 2},
	    {F_SYS_JUMP_VY, 0, 2},	    {F_SYS_JUMP_VY, 5, 2},
	    {F_SYS_JUMP_VY, (uint16_t)-(MAX_JUMP_VY + 1), 2},
	    {F_SYS_JUMP_VY, (uint16_t)-200, 2}, /* rises past MAX_HEIGHT */
	    {F_SYS_GRAVITY, 0, 2},	    {F_SYS_GRAVITY, MAX_GRAVITY + 1, 2},
	    {F_SYS_VX, MAX_WALK + 1, 2},    {F_SYS_VX_BACK, MAX_WALK + 1, 2},
	    {F_SYS_PREJUMP, 0, 2},	    {F_SYS_PREJUMP, MAX_PREJUMP + 1, 2},
	    {F_SYS_WEIGHT, MIN_WEIGHT - 1, 2}, {F_SYS_WEIGHT, MAX_PERMILLE + 1, 2},
	    {F_SYS_HEIGHT, HEIGHTS, 2},	    {F_SYS_HEIGHT, 256 + HEIGHT_MID, 2},
	    {F_SYS_PUSH, 1, 2},		    {F_SYS_PUSH, MAX_PUSH_DISTANCE + 1, 2},
	    {F_SYS_GUARD_RANGE, FW + 1, 2}, {F_SYS_BUFFER, 0, 2},
	    {F_SYS_BUFFER, MAX_BUFFER + 1, 2}, {F_SYS_QUARTER, 2, 2},
	    {F_SYS_QUARTER, HISTORY + 1, 2}, {F_SYS_DRAGON, HISTORY + 1, 2},
	    {F_SYS_CHARGE, 0, 2},	    {F_SYS_CHARGE, MAX_CHARGE + 1, 2},
	    {F_SYS_GRACE, MAX_GRACE + 1, 2}, {F_SYS_RELEASE, 0, 2},
	    {F_SYS_RELEASE, HISTORY + 1, 2}, {F_SYS_LIGHT_HITSTUN, 0, 2},
	    {F_SYS_LIGHT_HITSTUN, MAX_STUN + 1, 2}, {F_SYS_LIGHT_BLOCKSTUN, 0, 2},
	    {F_SYS_LIGHT_HITSTOP, MAX_HITSTOP + 1, 2}, {F_SYS_LIGHT_HAS, 4, 2},
	    {F_SYS_LIGHT_HAS, 2, 2},	    {F_SYS_THROW_HITSTUN, 5, 2},
	    {F_SYS_THROW_HAS, 1, 2},
	    /* Character stats. */
	    {F_CHAR_SYSTEM, 2, 2},	    {F_STATS_MASK, 1u << 11, 2},
	    {F_STAT_LIFE, 5, 2},	    {F_STAT_WALK, MAX_WALK + 1, 2},
	    {F_STAT_HEIGHT, HEIGHTS, 2},
	    /* Classes and class defaults. */
	    {F_MOVE_CLASS, CLASSES, 2},	    {F_MOVE_DEFAULTS, DEFAULT_HITSTUN, 2},
	    {F_DEF_HITSTUN, 5, 2},	    {F_DEF_CLASS, CLASS_THROW, 2},
	    {F_DEF_CLASS, CLASS_COMMAND, 2}, {F_DEF_CLASS, CLASS_NONE, 2},
	    {F_DEF_DEFAULTS, 8, 2},	    {F_PROJ_CLASS, CLASSES, 2},
	    {F_PROJ_DEFAULTS, DEFAULT_HITSTOP | DEFAULT_HITSTUN, 2},
	    {F_PROJ_CLASS, CLASS_NONE, 2},
	    /* Sparks, walls, effect sets, slash and home. */
	    {F_SPARKS, MAX_SPARKS + 1, 4},  {F_SPARKS, 3, 4},
	    {F_SPARKS, 1, 4},		    {F_STAGES, MAX_STAGES + 1, 4},
	    {F_STAGES, 2, 4},		    {F_STAGES, 0, 4}, /* SYN's home */
	    {F_SPARK_BLEND, BLENDS, 1},	    {F_SPARK_ALPHA, 0, 1},
	    {F_SPARK_STEPS, 0, 2},	    {F_SPARK_STEPS, MAX_SPARK_STEPS + 1, 2},
	    {F_SPARK_FRAME, SYN_ALL, 2},    {F_SPARK_PALETTE, 3, 2},
	    {F_SPARK_PALETTE, 2, 2},	    {F_SPARK_TICKS, 0, 2},
	    {F_SPARK_TICKS, 256, 2},	    {F_SYS_MARGIN, MIN_MARGIN - 1, 2},
	    {F_SYS_MARGIN, MAX_MARGIN + 1, 2}, {F_SYS_PLAY, MIN_PLAY_WIDTH - 1, 2},
	    {F_SYS_PLAY, FW + 1, 2},	    {F_SYS_EFFECT, 3, 2},
	    {F_CHAR_SLASH, 32, 2},	    {F_CHAR_HOME, 2, 2},
	    /* Stages, layers and stage anims. */
	    {F_STAGE_NAME_LEN, 0, 1},	    {F_STAGE_NAME_LEN, MAX_NAME + 1, 1},
	    {F_STAGE_NAME_CHAR, 'd', 1},    {F_STAGE_WIDTH, FW - 1, 2},
	    {F_STAGE_WIDTH, MAX_STAGE_WIDTH + 1, 2}, {F_STAGE_GROUND, MAX_SIDE + 1, 2},
	    {F_STAGE_SYSTEM, 2, 2},	    {F_STAGE_LAYERS, 0, 2},
	    {F_STAGE_LAYERS, MAX_LAYERS + 1, 2}, {F_STAGE_LAYERS, 2, 2},
	    {F_STAGE_LAYERS, 4, 2},	    {F_LAYER_FRAME, SYN_ALL, 2},
	    {F_LAYER_FRAME, 0, 2}, /* anchor outside the layer */
	    {F_LAYER_PALETTE, 3, 2},	    {F_LAYER_PALETTE, 2, 2},
	    {F_LAYER_W, 0, 2},		    {F_LAYER_W, 49, 2}, /* frame spills right */
	    {F_LAYER_W, MAX_SIDE + 1, 2},   {F_LAYER_H, 42, 2},
	    {F_LAYER_X, MAX_SIDE + 1, 2},   {F_LAYER_Y, (uint16_t)-(MAX_SIDE + 1), 2},
	    {F_LAYER_NUM, MAX_SIDE + 1, 2}, {F_LAYER_NUM, MAX_SIDE, 2}, /* cover too wide */
	    {F_LAYER_DEN, 0, 2},	    {F_LAYER_DEN, MAX_STAGE_WIDTH + 1, 2},
	    {F_LAYER_EX, EDGES, 1},	    {F_LAYER_EY, EDGES, 1},
	    {F_LAYER_ANIMS, MAX_LAYER_ANIMS + 1, 2}, {F_ANIM_STEPS, 0, 2},
	    {F_ANIM_STEPS, MAX_ANIM_STEPS + 1, 2}, {F_ASTEP_FRAME, SYN_ALL, 2},
	    {F_ASTEP_X, MAX_SIDE + 1, 2},   {F_ASTEP_Y, (uint16_t)-(MAX_SIDE + 1), 2},
	    {F_ASTEP_TICKS, 0, 2},
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
	for (char v = '1'; v <= '7'; v++) {
		bad = good;
		bad.data[7] = (uint8_t)v; /* FIGHTP01..07: retired formats */
		assert(!load(&assets_a, &bad));
	}
	bad = good;
	bad.data[good.at[F_INDEX] + 3] ^= 1;
	assert(!load(&assets_a, &bad));
	build(&bad, 1);
	int automatic = -1;
	assert(fight_assets(&assets_a, bad.data, bad.len, &automatic) && automatic == 1);
}
/* Resident images must reproduce the source bitmaps through the palettes
 * (stage layer images are checked against their layers instead). */
static void roundtrip(const struct assets *a, const struct decoded *dec,
		      const struct bitmap *src)
{
	for (unsigned i = 0; i < a->images; i++) {
		const struct image *im = &a->image[i];
		int layer = layer_of(a, (int)i);
		if (!im->w) {
			/* An empty cache slot: only past the effects and sparks. */
			assert(i >= a->effect_images && !im->h && !im->count);
			continue;
		}
		if (layer >= 0) {
			const struct layer *l =
			    &a->layer[a->stage[a->cast_stage].first_layer + (unsigned)layer];
			uint32_t count = 0;
			for (int y = 0; y < im->h; y++) {
				int x = 0;
				for (uint32_t k = a->row[im->first_row + y];
				     k < a->row[im->first_row + y + 1]; k++) {
					const struct span *s = &a->span[k];
					for (; x < s->x; x++)
						assert(ref_layer(dec, l, a->layer_x0[layer] + x,
								 a->layer_y0[layer] + y) < 0);
					for (unsigned j = 0; j < s->len; j++, x++, count++)
						assert(a->pixel[im->pixels + s->pixel + j] ==
						       ref_layer(dec, l, a->layer_x0[layer] + x,
								 a->layer_y0[layer] + y));
				}
				for (; x < im->w; x++)
					assert(ref_layer(dec, l, a->layer_x0[layer] + x,
							 a->layer_y0[layer] + y) < 0);
			}
			assert(count == im->count);
			continue;
		}
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
	fight_start(&st, a, 0, 0);
	fight_cast(a, &st);
	st.p[1].hp = 10;
	st.p[0].x = QX(240);
	st.p[1].x = QX(300);
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
	assert(a->characters == 1 && !strcmp(ch->name, "SYN") && ch->moves == MOVES + 2 &&
	       ch->projectiles == 1 && ch->palette[0] == 0 && ch->palette[1] == 1);
	/* Action HURT (5) is steps 5..7 (k % 6, 1 + k % 3), 2 + 3 + 4 ticks. */
	assert(ch->action[HURT].count == 3 && ch->action[HURT].total == 2 + 3 + 4);
	for (int t = 0; t < MOVES; t++) {
		const struct move *m = &ch->move[t];
		/* Steps 8..11 last 1, 2, 3, 4 ticks; 9 and 10 hit. */
		/* Crouch HK: class heavy of system "kof" gives 19/17 and hitstop 9. */
		int heavy = t == CROUCH_HK;
		assert(m->anim.count == 4 && m->anim.total == 10 && m->startup == 1 &&
		       m->active == 5 && m->recovery == 4 && m->groups == 1 &&
		       m->reach == 5 + 12 + 10 && m->hit.damage == 10 + t && m->hit.chip == t &&
		       m->hit.hitstun == (heavy ? 19 : 5 + t) &&
		       m->hit.blockstun == (heavy ? 17 : 3 + t) &&
		       m->hit.hitstop == (heavy ? 9 : 2) &&
		       m->hit.cls == (heavy ? CLASS_HEAVY : CLASS_NONE) && m->hit.pushback == 6 &&
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
	       rise->vx == 3 && rise->vy == -2 && rise->spawn == 1 && rise[1].vy == 2 &&
	       !sp->after && !sp->open && !sp->close && !sp->contact && !strcmp(sp->name, "SP-1") &&
	       !ch->move[STAND_HK].name[0]);
	const struct move *fu = &ch->move[MOVES + 1];
	assert(fu->special && fu->motion == HOLD_6 && fu->buttons == BUTTON_HK &&
	       fu->after == MOVES + 1 && fu->open == 1 && fu->close == 2 && fu->contact == 1 &&
	       fu->anim.total == 4 && !ch->move[STAND_HK].after && !strcmp(fu->name, "FU 2"));
	const struct projectile *pr = &ch->projectile[0];
	assert(pr->anim.count == 2 && pr->anim.total == 4 && pr->speed == 5 &&
	       pr->lifetime == 50 && pr->x == 10 && pr->y == -5 && pr->hit.damage == 40 &&
	       pr->hit.chip == 9 && pr->hit.hitstun == 12 && pr->hit.hitstop == 10 &&
	       pr->hit.cls == CLASS_SPECIAL && pr->hit.guard == GUARD_LOW && pr->blend == ALPHA &&
	       pr->alpha == 200 && !a->step[pr->anim.first].hurt);
	/* Stats: system "kof" with SYN's own walk 10 and height high; the jump
	 * -48/3 peaks at 96 px and a fall from MAX_HEIGHT reaches vy 87. */
	const struct stats *st = &ch->stats;
	assert(a->systems == 2 && ch->system == 0 && !strcmp(a->sys[0].name, "kof") &&
	       !strcmp(a->sys[1].name, "ss") && a->sys[1].damage_scale == 800);
	assert(st->life == 1000 && st->defense == 1000 && st->weight == 1000 && st->walk == 10 &&
	       st->walk_back == 6 && st->height == HEIGHT_HIGH && st->jump.vy == -48 &&
	       st->jump.gravity == 3 && st->jump.vx == 8 && st->jump.vx_back == 8 &&
	       st->jump.prejump == 3 && st->jump.peak == 96 && st->jump.fall == 87 &&
	       st->push == 46 && st->guard_range == 160 && st->buffer == 4 && st->quarter == 15 &&
	       st->dragon == 20 && st->charge == 30 && st->grace == 10 && st->release == 4);
	assert(a->sys[0].stats.walk == 7 && a->sys[0].stats.height == HEIGHT_MID &&
	       a->sys[0].cls[CLASS_COMMAND].has == 1 && !a->sys[0].cls[CLASS_THROW].has);
	/* The cache follows the state: the select screen only P1 (normal
	 * palette); a fight both sides (normal and alternate palette images of
	 * every frame the character uses, all five) and the stage in their own
	 * regions, recast only on change. */
	struct fight_state s;
	fight_reset(&s, 0);
	assert(a->cast[0][0] == -1 && a->cast_stage == -1 &&
	       a->images == a->effect_images + a->stage[0].images + 2 * SYN_FRAMES &&
	       a->char_images == SYN_FRAMES && a->char_pixels == ch->pixels);
	assert(fight_cast(a, &s) && !fight_cast(a, &s) && fight_ready(a));
	assert(a->wanted == 1 && a->cast[0][0] == 0 && a->cast[1][0] == -1 &&
	       a->cast_stage == -1 && a->cast_pixels[0] == ch->pixels);
	fight_start(&s, a, 0, 0);
	assert(fight_cast(a, &s) && !fight_cast(a, &s) && a->wanted == 7);
	assert(a->cast_stage == 0 && a->stage_pixels == a->stage[0].pixels &&
	       a->stage_used == a->stage[0].images && a->cast_images[0] == SYN_FRAMES &&
	       a->cast_images[1] == SYN_FRAMES && a->cast_pixels[0] == ch->pixels &&
	       a->cast_pixels[1] == ch->pixels);
	for (int i = 0; i < 2; i++)
		for (unsigned f = 0; f < SYN_FRAMES; f++) {
			const struct image *im = &a->image[a->look[i][f]];
			assert(a->look[i][f] >= (int)a->region_image[1 + i] &&
			       a->look[i][f] < (int)(a->region_image[1 + i] + SYN_FRAMES) &&
			       im->frame == f && im->palette == (unsigned)i);
		}
	s.pal[1] = 0;
	assert(fight_cast(a, &s) == 1 && a->image[a->look[1][0]].palette == 0 &&
	       a->image[a->look[0][0]].palette == 0);
	/* Back on the select screen side 1 and the stage keep what they hold. */
	s.mode = MODE_SELECT;
	assert(!fight_cast(a, &s) && a->wanted == 1 && a->cast[1][1] == 0 && a->cast_stage == 0);
	/* Mirroring: right-facing box at anchor 100 covers 90..109; left-facing
	 * mirrors around the anchor pixel boundary like the sprite does. */
	const struct box b = {-10, -29, 20, 20}, hit = {5, -15, 21, 6};
	struct area r = fight_box(&b, 100, -7, 1), l = fight_box(&b, 100, -7, -1),
		    hr = fight_box(&hit, 100, 0, 1), hl = fight_box(&hit, 100, 0, -1);
	assert(r.x == 90 && r.y == GROUND - 36 && r.w == 20 && r.h == 20);
	assert(l.x == 90 && l.y == r.y);
	assert(hr.x == 105 && hl.x == 100 - 5 - 21 && hl.w == 21 && hl.y == GROUND - 15);
}
/* The synthetic sparks, walls, effect sets, slash mask, home and stage as
 * loaded, then the stage drawn from its scene commands against the same
 * stage computed per screen pixel from the layers for many cameras, shakes
 * and anim ticks. */
static void synthetic_stage(struct assets *a, const struct decoded *dec)
{
	const struct character *ch = &a->chr[0];
	assert(a->sparks == 2 && a->spark[0].count == 2 && a->spark[0].total == 5 &&
	       a->spark[0].blend == OPAQUE && a->spark[0].alpha == 255 &&
	       a->spark[1].count == 1 && a->spark[1].total == 4 && a->spark[1].blend == ADD &&
	       a->spark[1].alpha == 200);
	const struct image *s0 = &a->image[a->spark_step[a->spark[0].first + 1].image],
			   *s1 = &a->image[a->spark_step[a->spark[1].first].image];
	assert(s0->frame == 1 && s0->palette == 1 && s1->frame == 4 && s1->palette == 0 &&
	       a->spark_step[a->spark[0].first].ticks == 2);
	assert(a->sys[0].margin == 24 && a->sys[0].play_width == FW && a->sys[1].margin == 16 &&
	       a->sys[1].play_width == 400);
	for (int k = 0; k < FX_KEYS; k++)
		assert(a->sys[0].effect[k] == syn_fx[0][k] && a->sys[1].effect[k] == syn_fx[1][k]);
	assert(ch->slash == 0x11 && ch->home == 1 && ch->move[MOVES].slash);
	for (int t = 0; t < MOVES; t++)
		assert(ch->move[t].slash == (t % 4 == 0));
	const struct stage *st = &a->stage[0];
	assert(a->stages == 1 && !strcmp(st->name, "DOJO") && st->width == 600 &&
	       st->ground == 200 && st->system == 1 && st->layers == 3 && a->layers == 3 &&
	       a->anims == 2 && a->stage_steps == 3 && st->images == 3 + 2);
	for (int i = 0; i < 3; i++) {
		const struct layer *l = &a->layer[st->first_layer + (unsigned)i];
		const struct syn_layer *w = &syn_layers[i];
		assert(l->frame == 5 && l->palette == w->palette && l->w == w->w && l->h == w->h &&
		       l->x == w->x && l->y == w->y && l->num == w->num && l->den == w->den &&
		       l->edge[0] == w->ex && l->edge[1] == w->ey && l->color == w->color &&
		       l->anims == w->anims);
	}
	/* Parallax rounds down, also moving against the camera. */
	const struct layer *l0 = &a->layer[st->first_layer], *l2 = l0 + 2;
	assert(fight_layer_x(l0, 2) == 0 && fight_layer_x(l0, 3) == -1 &&
	       fight_layer_x(l2, 1) == -29 && fight_layer_x(l2, 4) == -28);
	/* Walls come from the stage's system "ss": a 400 wide play area with
	 * 16 px margins. */
	struct fight_state s;
	fight_reset(&s, 0);
	fight_start(&s, a, 0, 0);
	assert(s.arena == 0 && fight_width(&s, a) == 600);
	int lo, hi;
	s.cam = 50;
	fight_walls(&s, a, &lo, &hi);
	assert(lo == 50 + 40 + 16 && hi == 50 + FW - 40 - 16);
	static struct scene sc;
	static uint16_t fb[FW * FH], want[FW * FH];
	static const int cams[] = {0, 1, 2, 3, 37, 59, 60, 119, 120};
	static const struct {
		uint32_t shake;
		int dy;
	} shakes[] = {{0, 0}, {1, -2}, {2, 2}, {9, -SHAKE_MAX}, {10, SHAKE_MAX}};
	unsigned checked = 0;
	for (unsigned ci = 0; ci < sizeof(cams) / sizeof(cams[0]); ci++)
		for (unsigned si = 0; si < sizeof(shakes) / sizeof(shakes[0]); si++)
			for (uint32_t tick = 0; tick < 6; tick++) {
				s.cam = (uint32_t)cams[ci];
				s.shake = shakes[si].shake;
				s.flash = 0;
				s.tick = tick;
				fight_cast(a, &s);
				assert(a->cast_stage == 0 && a->stage_pixels == st->pixels);
				fight_scene(&s, a, &sc);
				/* Three layers, the first anim showing 3 of its 5 ticks. */
				assert(sc.layers == 3 + 1 + (tick % 5 < 3));
				sc.count = sc.layers;
				noise(fb);
				memcpy(want, fb, sizeof(fb));
				fight_render(a, &sc, fb);
				int dy = shakes[si].dy, cam = cams[ci];
				for (int i = 0; i < 3; i++) {
					const struct layer *l = &a->layer[st->first_layer + (unsigned)i];
					int m = cam * l->num, q = m / (int)l->den;
					if (q * (int)l->den > m)
						q--;
					int ox = l->x - q, oy = GROUND - st->ground + l->y + dy;
					for (int y = 0; y < FH; y++)
						for (int x = 0; x < FW; x++) {
							int c = ref_layer(dec, l, x - ox, y - oy);
							if (c >= 0)
								want[y * FW + x] = (uint16_t)c;
						}
					if (!l->anims)
						continue;
					const struct stage_anim *an = &a->anim[l->first_anim];
					unsigned k = an->first;
					for (uint32_t t = tick % an->total; t >= a->stage_step[k].ticks;)
						t -= a->stage_step[k++].ticks;
					const struct stage_step *sp = &a->stage_step[k];
					if (sp->frame == 0xffff)
						continue;
					const struct bitmap *b = &dec->frame[sp->frame];
					for (int y = 0; y < b->h; y++)
						for (int x = 0; x < b->w; x++) {
							int X = ox + sp->x - b->ax + x,
							    Y = oy + sp->y - b->ay + y, v = b->px[y * b->w + x];
							if (v >= 0 && X >= 0 && X < FW && Y >= 0 && Y < FH)
								want[Y * FW + X] = dec->pal[l->palette][v];
						}
				}
				for (int i = 0; i < FW * FH; i++)
					if (fb[i] != want[i]) {
						fprintf(stderr,
							"stage mismatch cam %d dy %d tick %u at %d,%d: "
							"%04x != %04x\n",
							cam, dy, tick, i % FW, i / FW, fb[i], want[i]);
						abort();
					}
				checked++;
			}
	assert(checked == 9 * 5 * 6);
	/* A full scene on the stage (fighters, then a spark of each kind) also
	 * matches the command-level reference. */
	s.cam = 60;
	s.shake = 0;
	s.spark[0] = (struct spark_slot){1, 300, 200, 3, 1};
	s.spark[1] = (struct spark_slot){2, 320, 180, 1, 0};
	fight_scene(&s, a, &sc);
	assert(sc.sparks == 2 && compare(a, dec, &sc, "synthetic stage scene"));
}
/* Every real step's boxes cover the same sprite pixels in both facings.
 * The idle step is pointed at each frame in turn so fight_scene places the
 * sprite itself; opaque pixels are those that match on two backgrounds. */
/* The attacker is drawn over the defender whichever side it is on. */
static void attacker_front(struct assets *a)
{
	static struct scene s;
	struct fight_state st;
	for (int who = 0; who < 2; who++) {
		fight_reset(&st, 0);
		fight_start(&st, a, 0, 0);
		fight_cast(a, &st);
		st.p[0].x = QX(230);
		st.p[1].x = QX(250);
		st.p[who].move = 1;
		st.p[!who].stun = 10;
		fight_scene(&st, a, &s);
		int last = -1;
		for (unsigned k = 0; k < s.count; k++)
			if (s.cmd[k].op == SPRITE && s.cmd[k].blend == OPAQUE)
				last = (int)k;
		int frame = a->step[fight_pose(&st, a, who)].frame;
		assert(last >= 0 && s.cmd[last].image == a->look[who][frame]);
	}
}
static void box_flip(struct assets *a)
{
	static struct scene s;
	static uint16_t fb[2][2][FW * FH];
	struct fight_state st;
	fight_reset(&st, 0);
	fight_start(&st, a, 0, 0);
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
			fight_start(&st, a, 0, 0);
			st.p[0].x = QX(240);
			st.p[0].face = f ? -1 : 1;
			st.cam = 0; /* world and screen columns agree */
			assert(fight_pose(&st, a, 0) == idle);
			fight_scene(&st, a, &s);
			unsigned cmd = s.layers; /* the stage comes first */
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
	fight_start(&st, a, 0, 0);
	st.p[0].x = QX(200);
	st.p[1].x = QX(260);
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
	fight_start(&st, a, 0, 0);
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
/* Pixels the commands of a scene write (each command alone on two
 * backgrounds, so overdraw counts). */
static uint32_t written(const struct assets *a, const struct scene *s)
{
	static struct scene one;
	static uint16_t fb[2][FW * FH];
	uint32_t n = 0;
	for (unsigned k = 0; k < s->count; k++) {
		one.count = 1;
		one.cmd[0] = s->cmd[k];
		for (int b = 0; b < 2; b++) {
			for (int i = 0; i < FW * FH; i++)
				fb[b][i] = b ? 0xffff : 0;
			fight_render(a, &one, fb[b]);
		}
		for (int i = 0; i < FW * FH; i++)
			n += fb[0][i] != 0 || fb[1][i] != 0xffff;
	}
	return n;
}
static void time_scene(const struct assets *a, const struct scene *s, const char *what)
{
	static uint16_t fb[FW * FH];
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < 2000; i++)
		fight_render(a, s, fb);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("fighter-bench: %s render %.1f us/frame (%u commands, %u layers, %u sparks, "
	       "%u px written)\n",
	       what,
	       ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / 2000 /
		   1000,
	       s->count, s->layers, s->sparks, written(a, s));
}
/* The four stress scenes, then a fight on every stage (with the camera
 * where the rules put it) and on the flat fallback. */
static void bench(struct assets *a)
{
	static struct scene s;
	char what[64];
	struct fight_state st;
	fight_reset(&st, 0);
	fight_start(&st, a, 0, 0);
	for (int i = 0; i < 300; i++)
		fight_step(&st, a, 0, 0, 0);
	fight_cast(a, &st);
	for (unsigned m = 0; m < 4; m++) {
		st.stress = m;
		fight_scene(&st, a, &s);
		snprintf(what, sizeof(what), "scene %u (%s)", m, a->stage[st.arena].name);
		time_scene(a, &s, what);
	}
	unsigned stages = a->stages;
	for (unsigned k = 0; k <= stages; k++) {
		fight_reset(&st, 0);
		if (k == stages)
			a->stages = 0;
		fight_start(&st, a, 0, 0);
		st.arena = k < stages ? k : 0;
		for (int i = 0; i < 300; i++)
			fight_step(&st, a, 0, 0, 0);
		fight_cast(a, &st);
		fight_scene(&st, a, &s);
		snprintf(what, sizeof(what), "stage %s cam %u", k < stages ? a->stage[k].name : "FLAT",
			 st.cam);
		time_scene(a, &s, what);
		a->stages = stages;
	}
}
/* The cache filled a budget at a time, as libretro.c does (prefetch, then
 * what the scene still misses), draws exactly what a fully expanded cache
 * draws on every tick, and no call expands much more than its budget: the
 * select screen with the cursor moving every other tick, the versus card
 * (after which the stage and both fighters are complete, so a fight never
 * waits for the stage), fights, result and clear screens of Auto ladders. */
static void incremental_cache(struct assets *full, struct assets *lazy)
{
	static struct scene s, t;
	static uint16_t fb[2][FW * FH];
	const uint32_t budget = 49152;
	uint32_t biggest = 0, worst = 0, demanded = 0;
	for (unsigned f = 0; f < lazy->frames; f++)
		biggest = lazy->frame[f].count > biggest ? lazy->frame[f].count : biggest;
	biggest = biggest > MAX_COVER ? biggest : MAX_COVER;
	struct fight_state st;
	unsigned intros = 0, moves = 0;
	for (int automatic = 0; automatic < 2; automatic++) {
		fight_reset(&st, automatic);
		for (unsigned n = 0; n < (automatic ? 9000u : 120u); n++) {
			uint32_t keys = !automatic && n % 2 == 0 ? 1u << 7 : 0; /* RIGHT */
			uint32_t before = st.chr[0];
			fight_step(&st, lazy, keys, 0, 0);
			moves += st.mode == MODE_SELECT && st.chr[0] != before;
			fight_want(lazy, &st);
			uint32_t done = fight_prefetch(lazy, budget);
			assert(done < budget + biggest);
			fight_scene(&st, lazy, &s);
			assert(!s.stage_missing);
			if (s.misses) {
				assert(s.misses <= SCENE_MISSES);
				uint32_t more = fight_demand(lazy, &s);
				assert(more <= s.misses * biggest);
				demanded += s.misses;
				done += more;
				fight_scene(&st, lazy, &s);
				assert(!s.misses);
			}
			worst = done > worst ? done : worst;
			if (st.mode == MODE_FIGHT && st.timer == INTRO_TICKS - 1)
				intros++;
			if (st.mode == MODE_FIGHT && st.timer == INTRO_TICKS)
				assert(fight_ready(lazy));
			fight_cast(full, &st);
			fight_scene(&st, full, &t);
			noise(fb[0]);
			memcpy(fb[1], fb[0], sizeof(fb[0]));
			fight_render(lazy, &s, fb[0]);
			fight_render(full, &t, fb[1]);
			if (memcmp(fb[0], fb[1], sizeof(fb[0]))) {
				fprintf(stderr, "incremental cache: tick %u mode %u timer %u differs\n",
					st.tick, st.mode, st.timer);
				abort();
			}
		}
	}
	assert(moves >= 50 && intros >= 3 && demanded > 0);
	if (!getenv("FIGHTER_BENCH"))
		return;
	printf("fighter-bench: cache worst run %u px (budget %u, largest frame or row %u), %u "
	       "frames on demand\n",
	       worst, budget, biggest, demanded);
	/* A whole cast at once (the heaviest character mirrored on the first
	 * stage), as every select cursor move used to cost. */
	unsigned heavy = 0;
	for (unsigned c = 0; c < lazy->characters; c++)
		heavy = lazy->chr[c].pixels > lazy->chr[heavy].pixels ? c : heavy;
	struct timespec t0, t1;
	double us = 0;
	uint32_t px = 0;
	for (int r = 0; r < 20; r++) {
		fight_reset(&st, 0);
		fight_start(&st, lazy, heavy ? 0 : 1, heavy ? 0 : 1);
		st.arena = 1 % lazy->stages;
		fight_want(lazy, &st);
		fight_start(&st, lazy, heavy, heavy);
		st.arena = 0;
		fight_want(lazy, &st);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		px = fight_prefetch(lazy, UINT32_MAX);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		us += ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) /
		      1000 / 20;
	}
	printf("fighter-bench: full cast %s mirror on %s %u px in %.0f us (%.2f ns/px)\n",
	       lazy->chr[heavy].name, lazy->stage[0].name, px, us, us * 1000 / px);
}
/* SYN moved to system "ss": damage and chip scaled by 800 per mille
 * (rounded half up), crouch HK and the projectile take ss's class values,
 * the stats ss's defaults except SYN's own walk and height. */
static void scaled_system(struct pack *p)
{
	static struct pack ss;
	ss = *p;
	set(&ss, ss.at[F_CHAR_SYSTEM], 1, 2);
	seal(&ss);
	assert(load(&assets_a, &ss));
	const struct character *ch = &assets_a.chr[0];
	assert(ch->system == 1);
	for (unsigned t = 0; t < MOVES; t++) {
		const struct hit *h = &ch->move[t].hit;
		assert(h->damage == ((10 + t) * 800 + 500) / 1000 &&
		       h->chip == (t * 800 + 500) / 1000);
		if (t == CROUCH_HK)
			assert(h->hitstun == 22 && h->blockstun == 20 && h->hitstop == 12);
		else
			assert(h->hitstun == 5 + t && h->hitstop == 2);
	}
	assert(ch->move[STAND_LP].hit.damage == 8 && ch->move[CROUCH_HK].hit.damage == 14 &&
	       ch->move[STAND_HP].hit.chip == 1);
	const struct hit *ph = &ch->projectile[0].hit;
	assert(ph->damage == 32 && ph->chip == 7 && ph->hitstop == 11);
	const struct stats *st = &ch->stats;
	assert(st->walk == 10 && st->walk_back == 5 && st->height == HEIGHT_HIGH &&
	       st->jump.vy == -42 && st->jump.gravity == 2 && st->jump.prejump == 4 &&
	       st->jump.peak == 110);
	/* A character's own life overrides the system's. */
	ss = *p;
	set(&ss, ss.at[F_STATS_MASK], 1u | 1u << 2 | 1u << 10, 2);
	set(&ss, ss.at[F_STAT_LIFE], 1200, 2);
	seal(&ss);
	assert(load(&assets_a, &ss) && assets_a.chr[0].stats.life == 1200);
}
void render_tests(const uint8_t *real, size_t real_len)
{
	static struct pack syn_pack;
	static struct decoded dec;
	make_bitmaps();
	malformed();
	build(&syn_pack, 0);
	scaled_system(&syn_pack);
	assert(load(&assets_b, &syn_pack));
	synthetic_moves(&assets_b);
	decode(syn_pack.data, syn_pack.len, &dec);
	roundtrip(&assets_b, &dec, syn);
	synthetic_stage(&assets_b, &dec);
	fixed_scenes(&assets_b, &dec);
	random_scenes(&assets_b, &dec, 300, "random synthetic scene");
	release(&dec);
	int automatic;
	assert(fight_assets(&assets_a, real, real_len, &automatic));
	struct fight_state st;
	fight_reset(&st, 0);
	fight_start(&st, &assets_a, 0, 0);
	assert(fight_cast(&assets_a, &st));
	decode(real, real_len, &dec);
	roundtrip(&assets_a, &dec, NULL);
	random_scenes(&assets_a, &dec, 100, "random real scene");
	game_scenes(&assets_a, &dec);
	release(&dec);
	box_flip(&assets_a);
	attacker_front(&assets_a);
	overlay(&assets_a);
	effect_cap(&assets_a);
	assert(fight_assets(&assets_b, real, real_len, &automatic));
	incremental_cache(&assets_a, &assets_b);
	if (getenv("FIGHTER_BENCH"))
		bench(&assets_a);
}
