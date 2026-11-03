/* Renderer and pack-decoder regression for the fighter core, linked into the
 * callback harness (test.c). It writes its own FIGHTP03 packs from known
 * bitmaps, checks that malformed or truncated packs are rejected, and
 * compares fight_render pixel for pixel with an independent per-pixel
 * reference (straight formulas, divisions, no spans) on fixed edge scenes,
 * seeded random scenes and real game scenes. It also rejects malformed
 * box, step and move data, checks the loader's derived frame data, that
 * boxes mirror with the sprite pixel for pixel, the L box overlay and the
 * effect pixel budget. FIGHTER_BENCH=1 also prints
 * host render times per stress scene; host numbers only show relative change. */
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
		d->pal_n[i] = get16(p + at);
		at += 2;
		for (unsigned k = 0; k < d->pal_n[i]; k++, at += 2)
			d->pal[i][k] = (uint16_t)get16(p + at);
	}
	for (unsigned f = 0; f < d->frames; f++) {
		struct bitmap *b = &d->frame[f];
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
struct pack {
	uint8_t data[1 << 16];
	size_t len;
	/* Offsets of fields the malformed-pack cases rewrite. */
	size_t flags, palettes, pal_n, frame_w, frame_h, row_count, span_x, span_len,
	    span2_x, index, characters, char_steps, char_boxes, char_moves, action_count,
	    action_first, move_trigger, move_first, move_count, move_damage, move_chip,
	    move_hitstun, move_blockstun, move_hitstop, move_pushback, move_block_push,
	    move_guard, move_cancel, step_frame, step_ticks, step_box, step_hurt,
	    action_step_hit, move_step_ticks, box_x, box_y, box_w, box_h, player_char,
	    player_palette, effect_frame;
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
#define SYN_STEPS 12
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
static void build(struct pack *p, int automatic)
{
	memset(p, 0, sizeof(*p));
	memcpy(p->data, "FIGHTP03", 8);
	p->len = 16;
	p->flags = p->len;
	put(p, (unsigned)automatic, 4);
	p->palettes = p->len;
	put(p, 2, 4);
	put(p, SYN_FRAMES, 4);
	p->characters = p->len;
	put(p, 1, 4); /* characters */
	put(p, 3, 4); /* effects */
	for (int c = 0; c < 2; c++) {
		if (!c)
			p->pal_n = p->len;
		put(p, 16, 2);
		for (int i = 0; i < 16; i++)
			put(p, syn_pal[c][i], 2);
	}
	for (int f = 0; f < SYN_FRAMES; f++) {
		const struct bitmap *b = &syn[f];
		if (!f) {
			p->frame_w = p->len;
			p->frame_h = p->len + 2;
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
				p->row_count = p->len;
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
					p->span_x = p->len, p->span_len = p->len + 2;
				if (!f && n == 1)
					p->span2_x = p->len;
				put(p, (unsigned)start, 2);
				put(p, (unsigned)(x - start), 2);
				n++;
			}
		if (!f)
			p->index = p->len;
		for (int i = 0; i < b->w * b->h; i++)
			if (b->px[i] >= 0)
				put(p, (unsigned)b->px[i], 1);
	}
	/* One character: 12 steps with a hurtbox each, steps 9 and 10 also
	 * carry a hitbox; actions use steps 0..7, every move steps 8..11. Moves
	 * are written in reverse trigger order. */
	p->char_steps = p->len;
	put(p, SYN_STEPS, 2);
	p->char_boxes = p->len;
	put(p, SYN_STEPS + 2, 2);
	p->char_moves = p->len;
	put(p, MOVES, 2);
	for (int k = 0; k < ACTIONS; k++) {
		if (k == HURT) {
			p->action_first = p->len;
			p->action_count = p->len + 2;
		}
		put(p, (unsigned)(k % 6), 2);
		put(p, 1 + (unsigned)(k % 3), 2);
	}
	for (int t = MOVES - 1; t >= 0; t--) {
		const unsigned v[12] = {(unsigned)t, 8, 4, 10 + (unsigned)t, (unsigned)t,
					5 + (unsigned)t, 3 + (unsigned)t, 2, 6, 8,
					(unsigned)t % GUARD_TYPES, (unsigned)t & CANCEL_MASK};
		if (t == STAND_HK) {
			size_t *at[12] = {&p->move_trigger, &p->move_first, &p->move_count,
					  &p->move_damage, &p->move_chip, &p->move_hitstun,
					  &p->move_blockstun, &p->move_hitstop, &p->move_pushback,
					  &p->move_block_push, &p->move_guard, &p->move_cancel};
			for (int j = 0; j < 12; j++)
				*at[j] = p->len + 2 * (size_t)j;
		}
		for (int j = 0; j < 12; j++)
			put(p, v[j], 2);
	}
	for (unsigned i = 0, box = 0; i < SYN_STEPS; i++) {
		unsigned hit = i == 9 || i == 10;
		if (i == 4)
			p->step_frame = p->len, p->step_ticks = p->len + 2,
			p->step_box = p->len + 4, p->step_hurt = p->len + 6;
		if (i == 3)
			p->action_step_hit = p->len + 7;
		if (i == 11)
			p->move_step_ticks = p->len + 2;
		put(p, i % SYN_FRAMES, 2);
		put(p, 1 + i % 4, 2);
		put(p, box, 2);
		put(p, 1, 1);
		put(p, hit, 1);
		box += 1 + hit;
	}
	for (unsigned i = 0; i < SYN_STEPS; i++) {
		if (i == 2)
			p->box_x = p->len, p->box_y = p->len + 2, p->box_w = p->len + 4,
			p->box_h = p->len + 6;
		put(p, (unsigned)(uint16_t)-10, 2);
		put(p, (unsigned)(uint16_t)(-20 - (int)i), 2);
		put(p, 20, 2);
		put(p, 20, 2);
		if (i == 9 || i == 10) {
			put(p, 5, 2);
			put(p, (unsigned)(uint16_t)-15, 2);
			put(p, 12 + i, 2);
			put(p, 6, 2);
		}
	}
	for (int c = 0; c < 2; c++) {
		if (c == 1)
			p->player_char = p->len, p->player_palette = p->len + 2;
		put(p, 0, 2);
		put(p, (unsigned)c, 2);
	}
	static const unsigned effects[3][2] = {{2, 1}, {3, 0}, {4, 1}};
	for (int i = 0; i < 3; i++) {
		if (i == 2)
			p->effect_frame = p->len;
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
#define AT(field) offsetof(struct pack, field)
	struct {
		size_t field;
		unsigned value;
		int bytes;
	} cases[] = {
	    {AT(flags), 2, 4},		    {AT(palettes), 0, 4},
	    {AT(palettes), MAX_PALETTES + 1, 4}, {AT(pal_n), 0, 2},
	    {AT(pal_n), 257, 2},	    {AT(frame_w), 0, 2},
	    {AT(frame_h), MAX_SIDE + 1, 2},	    {AT(row_count), 9, 2},
	    {AT(span_len), 0, 2},	    {AT(span_x), 12, 2},
	    {AT(span2_x), 0, 2},	    {AT(index), 16, 1},
	    {AT(action_count), 0, 2},	    {AT(action_first), SYN_STEPS, 2},
	    {AT(step_frame), SYN_FRAMES, 2},	    {AT(step_ticks), 0, 2},
	    {AT(effect_frame), SYN_FRAMES, 2},
	    /* Characters, moves, steps, boxes and players. */
	    {AT(characters), 0, 4},	    {AT(characters), MAX_CHARACTERS + 1, 4},
	    {AT(characters), 2, 4},	    {AT(char_steps), 0, 2},
	    {AT(char_steps), MAX_STEPS + 1, 2}, {AT(char_boxes), SYN_STEPS + 1, 2},
	    {AT(char_moves), MOVES - 1, 2},	    {AT(char_moves), MOVES + 1, 2},
	    {AT(move_trigger), MOVES, 2},	    {AT(move_trigger), STAND_LP, 2},
	    {AT(move_first), SYN_STEPS - 3, 2},  {AT(move_count), 0, 2},
	    {AT(move_damage), MAX_DAMAGE + 1, 2}, {AT(move_chip), 14, 2},
	    {AT(move_hitstun), 0, 2},	    {AT(move_hitstun), MAX_STUN + 1, 2},
	    {AT(move_blockstun), 0, 2},	    {AT(move_blockstun), MAX_STUN + 1, 2},
	    {AT(move_hitstop), MAX_HITSTOP + 1, 2}, {AT(move_pushback), MAX_PUSH + 1, 2},
	    {AT(move_block_push), MAX_PUSH + 1, 2}, {AT(move_guard), GUARD_TYPES, 2},
	    {AT(move_cancel), CANCEL_MASK + 1, 2}, {AT(step_box), SYN_STEPS + 2, 2},
	    {AT(step_hurt), MAX_STEP_BOXES + 1, 1}, {AT(step_hurt), 11, 1},
	    {AT(action_step_hit), 1, 1},	    {AT(move_step_ticks), MAX_MOVE_TICKS - 5, 2},
	    {AT(box_x), MAX_SIDE + 1, 2},	    {AT(box_y), (uint16_t)-(MAX_SIDE + 1), 2},
	    {AT(box_w), 0, 2},		    {AT(box_h), MAX_SIDE + 1, 2},
	    {AT(player_char), 1, 2},	    {AT(player_palette), 2, 2},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bad = good;
		size_t offset;
		memcpy(&offset, (const char *)&good + cases[i].field, sizeof(offset));
		set(&bad, offset, cases[i].value, cases[i].bytes);
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
	bad = good;
	bad.data[7] = '1'; /* FIGHTP01, the retired fixed-sprite format */
	assert(!load(&assets_a, &bad));
	bad = good;
	bad.data[7] = '2'; /* FIGHTP02, frames without boxes or moves */
	assert(!load(&assets_a, &bad));
	bad = good;
	bad.data[good.index + 3] ^= 1;
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
/* Real scenes from game states: every stress level, flips and the KO. */
static void game_scenes(const struct assets *a, const struct decoded *dec)
{
	static struct scene s;
	struct fight_state st;
	fight_reset(&st, 1);
	for (unsigned t = 0; t < 2400; t++) {
		fight_step(&st, a, 0, 0, 0);
		if (t % 150 == 7) {
			st.boxes = (t / 150) & 1;
			fight_scene(&st, a, &s);
			assert(compare(a, dec, &s, "game scene"));
		}
	}
	fight_reset(&st, 0);
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
/* Moves, steps and boxes of the synthetic pack as the loader derives them. */
static void synthetic_moves(const struct assets *a)
{
	for (int c = 0; c < 2; c++) {
		const struct character *ch = &a->chr[c];
		/* Action HURT (5) is steps 5..7 (k % 6, 1 + k % 3), 2 + 3 + 4 ticks. */
		assert(ch->action[HURT].count == 3 && ch->action[HURT].total == 2 + 3 + 4);
		for (int t = 0; t < MOVES; t++) {
			const struct move *m = &ch->move[t];
			/* Steps 8..11 last 1, 2, 3, 4 ticks; 9 and 10 hit. */
			assert(m->anim.count == 4 && m->anim.total == 10 && m->startup == 1 &&
			       m->active == 5 && m->recovery == 4 && m->groups == 1 &&
			       m->reach == 5 + 12 + 10 && m->damage == 10 + t && m->chip == t &&
			       m->hitstun == 5 + t && m->blockstun == 3 + t && m->hitstop == 2 &&
			       m->pushback == 6 && m->block_pushback == 8 &&
			       m->guard == t % GUARD_TYPES && m->cancel == (t & CANCEL_MASK));
			const struct step *st = &a->step[m->anim.first + 1];
			assert(st->hurt == 1 && st->hit == 1 && st->group == 1);
			const struct box *b = &a->box[st->box];
			assert(b[0].x == -10 && b[0].y == -29 && b[0].w == 20 && b[0].h == 20);
			assert(b[1].x == 5 && b[1].y == -15 && b[1].w == 21 && b[1].h == 6);
			assert(!a->step[m->anim.first].group && !a->step[m->anim.first + 3].group);
		}
	}
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
 * The idle step is pointed at each image in turn so fight_scene places the
 * sprite itself; opaque pixels are those that match on two backgrounds. */
static void box_flip(struct assets *a)
{
	static struct scene s;
	static uint16_t fb[2][2][FW * FH];
	struct fight_state st;
	unsigned checked = 0, idle = a->chr[0].action[IDLE].first;
	uint16_t saved = a->step[idle].image;
	for (unsigned k = 0; k < a->steps; k++) {
		const struct step *step = &a->step[k];
		a->step[idle].image = step->image;
		for (int f = 0; f < 2; f++) {
			fight_reset(&st, 0);
			st.p[0].x = 240;
			st.p[0].face = f ? -1 : 1;
			assert(fight_pose(&st, a, 0) == idle);
			fight_scene(&st, a, &s);
			unsigned c = 0;
			while (c < s.count && s.cmd[c].op != SPRITE)
				c++;
			assert(c < s.count && s.cmd[c].image == step->image && s.cmd[c].flip == f);
			s.cmd[0] = s.cmd[c];
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
		/* Hurtboxes sit on the body: some sprite pixels in every pose. */
		assert(!step->hurt || covered);
	}
	a->step[idle].image = saved;
	assert(checked > 200);
}
/* The L overlay draws every hurt and hit box of the shown steps. */
static void overlay(const struct assets *a)
{
	static struct scene s, plain;
	struct fight_state st;
	fight_reset(&st, 0);
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
