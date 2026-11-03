/* Renderer and pack-decoder regression for the fighter core, linked into the
 * callback harness (test.c). It writes its own FIGHTP02 packs from known
 * bitmaps, checks that malformed or truncated packs are rejected, and
 * compares fight_render pixel for pixel with an independent per-pixel
 * reference (straight formulas, divisions, no spans) on fixed edge scenes,
 * seeded random scenes and real game scenes. FIGHTER_BENCH=1 also prints
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
	    span2_x, index, char_palette, action_count, action_first, step_frame,
	    step_ticks, effect_frame;
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
static void build(struct pack *p, int automatic)
{
	memset(p, 0, sizeof(*p));
	memcpy(p->data, "FIGHTP02", 8);
	p->len = 16;
	p->flags = p->len;
	put(p, (unsigned)automatic, 4);
	p->palettes = p->len;
	put(p, 2, 4);
	put(p, SYN_FRAMES, 4);
	put(p, 9, 4); /* steps */
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
	/* Characters: palette, then 8 actions of (first step, count). */
	for (int c = 0; c < 2; c++) {
		if (c == 1)
			p->char_palette = p->len;
		put(p, (unsigned)c, 2);
		for (int k = 0; k < ACTIONS; k++) {
			if (c == 1 && k == ATTACK) {
				p->action_first = p->len;
				p->action_count = p->len + 2;
			}
			put(p, (unsigned)(k % 7), 2);
			put(p, 1 + (unsigned)(k % 3), 2);
		}
	}
	for (int i = 0; i < 9; i++) {
		if (i == 4)
			p->step_frame = p->len, p->step_ticks = p->len + 2;
		put(p, (unsigned)(i % SYN_FRAMES), 2);
		put(p, 1 + (unsigned)i, 2);
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
	    {AT(char_palette), 2, 2},	    {AT(action_count), 0, 2},
	    {AT(action_first), 8, 2},	    {AT(step_frame), SYN_FRAMES, 2},
	    {AT(step_ticks), 0, 2},	    {AT(effect_frame), SYN_FRAMES, 2},
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
		fight_step(&st, 0, 0, 0);
		if (t % 150 == 7) {
			st.boxes = (t / 150) & 1;
			fight_scene(&st, a, &s);
			assert(compare(a, dec, &s, "game scene"));
		}
	}
	fight_reset(&st, 0);
	st.p[1].hp = 10;
	st.p[0].x = 250;
	st.p[1].x = 300;
	fight_step(&st, 1u << 8, 0, 1); /* A: strike, KO on the hit */
	for (int t = 0; t < 40; t++)
		fight_step(&st, 0, 0, 1);
	assert(st.ended && !st.p[1].hp);
	st.stress = 3;
	fight_scene(&st, a, &s);
	assert(compare(a, dec, &s, "KO scene"));
}
static void bench(const struct assets *a)
{
	static struct scene s;
	static uint16_t fb[FW * FH];
	struct fight_state st;
	fight_reset(&st, 0);
	for (int i = 0; i < 300; i++)
		fight_step(&st, 0, 0, 0);
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
	if (getenv("FIGHTER_BENCH"))
		bench(&assets_a);
}
