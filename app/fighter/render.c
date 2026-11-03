/* FIGHTP03 pack decoder and clipped RGB565 software draw-command backend.
 * The decoder checks every count, offset and index against the file length
 * and the resident limits in fighter.h (including box bounds and counts and
 * move data ranges), expands each player's character steps with its palette,
 * derives hit groups, startup/active/recovery and reach per move, and
 * expands each (frame, palette) pair once into RGB565 runs: drawing a
 * character is span copies (mirroring reverses the span), rectangles are
 * row fills, and constant alpha/additive
 * blending have their own loops without per-pixel mode branches or
 * divisions. All destination reads stay in the cached canvas; the host
 * presents it. tools/fighter/pack.py documents the file layout. */
#include "fighter.h"
#include <string.h>

struct reader {
	const uint8_t *p;
	size_t at, size;
	int ok;
};
static unsigned u16(struct reader *r)
{
	if (r->size - r->at < 2) {
		r->ok = 0;
		return 0;
	}
	unsigned v = (unsigned)r->p[r->at] | (unsigned)r->p[r->at + 1] << 8;
	r->at += 2;
	return v;
}
static unsigned u8(struct reader *r)
{
	if (r->size - r->at < 1) {
		r->ok = 0;
		return 0;
	}
	return r->p[r->at++];
}
static int s16(struct reader *r) { return (int)(int16_t)u16(r); }
static uint32_t u32(struct reader *r)
{
	uint32_t lo = u16(r);
	return lo | (uint32_t)u16(r) << 16;
}
static int skip(struct reader *r, size_t n)
{
	if (r->size - r->at < n)
		return r->ok = 0;
	r->at += n;
	return 1;
}
struct source {
	int w, h, ax, ay;
	uint32_t first_row, count;
	size_t indices;
	unsigned max_index;
};
struct palette {
	unsigned n;
	uint16_t color[256];
};
static int frame(struct reader *r, struct assets *a, struct source *f)
{
	f->w = (int)u16(r);
	f->h = (int)u16(r);
	f->ax = s16(r);
	f->ay = s16(r);
	uint32_t spans = u32(r);
	if (!r->ok || f->w < 1 || f->h < 1 || f->w > MAX_SIDE || f->h > MAX_SIDE ||
	    f->ax < -MAX_SIDE || f->ax > MAX_SIDE || f->ay < -MAX_SIDE ||
	    f->ay > MAX_SIDE || a->rows + (unsigned)f->h + 1 > MAX_ROWS + 1 ||
	    spans > MAX_SPANS - a->spans)
		return 0;
	f->first_row = a->rows;
	uint32_t at = a->spans;
	for (int y = 0; y < f->h; y++) {
		a->row[a->rows++] = at;
		at += u16(r);
		if (at > a->spans + spans)
			return 0;
	}
	a->row[a->rows++] = at;
	if (!r->ok || at != a->spans + spans)
		return 0;
	uint32_t pixels = 0;
	for (int y = 0; y < f->h; y++) {
		unsigned end = 0;
		for (uint32_t k = a->row[f->first_row + y]; k < a->row[f->first_row + y + 1];
		     k++) {
			unsigned x = u16(r), len = u16(r);
			/* Spans are sorted, disjoint, non-empty and inside the row. */
			if (!r->ok || !len || x < end || x + len > (unsigned)f->w)
				return 0;
			end = x + len;
			a->span[k] = (struct span){(uint16_t)x, (uint16_t)len, pixels};
			pixels += len;
		}
	}
	a->spans += spans;
	f->count = pixels;
	f->indices = r->at;
	if (!skip(r, pixels))
		return 0;
	f->max_index = 0;
	for (uint32_t i = 0; i < pixels; i++)
		if (r->p[f->indices + i] > f->max_index)
			f->max_index = r->p[f->indices + i];
	return 1;
}
/* Expand a frame with a palette once; returns the image index or -1. */
static int image(struct assets *a, const uint8_t *p, const struct source *frames,
		 const struct palette *pal, unsigned f, unsigned c, uint32_t *charge)
{
	for (unsigned i = 0; i < a->images; i++)
		if (a->image[i].frame == f && a->image[i].palette == c)
			return (int)i;
	const struct source *s = &frames[f];
	if (a->images >= MAX_IMAGES || s->max_index >= pal[c].n ||
	    s->count > MAX_PIXELS - a->pixels)
		return -1;
	struct image *im = &a->image[a->images];
	*im = (struct image){s->w,	   s->h,      s->ax,	       s->ay,
			     s->first_row, a->pixels, s->count, (uint16_t)f,
			     (uint16_t)c};
	for (uint32_t i = 0; i < s->count; i++)
		a->pixel[a->pixels + i] = pal[c].color[p[s->indices + i]];
	a->pixels += s->count;
	*charge += s->count;
	return (int)a->images++;
}
/* A character as stored in the pack, before each player expands it with a
 * palette. Steps and boxes are character-local; box_base is where the
 * character's boxes start in a->box. */
struct raw_step {
	uint16_t frame, ticks, box;
	uint8_t hurt, hit;
};
struct raw_move {
	unsigned first, count;
	uint16_t damage, chip, hitstun, blockstun, hitstop, pushback, block_pushback;
	uint8_t guard, cancel;
};
struct raw_character {
	unsigned steps, box_base, first[ACTIONS], count[ACTIONS];
	struct raw_move move[MOVES];
	struct raw_step step[MAX_STEPS];
};
static int character(struct reader *r, struct assets *a, struct raw_character *c,
		     unsigned nframes)
{
	unsigned steps = u16(r), boxes = u16(r), moves = u16(r), seen = 0;
	if (!r->ok || !steps || steps > MAX_STEPS || boxes > MAX_BOXES - a->boxes ||
	    moves != MOVES)
		return 0;
	c->steps = steps;
	for (int k = 0; k < ACTIONS; k++) {
		c->first[k] = u16(r);
		c->count[k] = u16(r);
		if (!c->count[k] || c->first[k] + c->count[k] > steps)
			return 0;
	}
	/* Every trigger exactly once; stun at least one tick; all within limits. */
	for (unsigned k = 0; k < moves; k++) {
		unsigned v[12];
		for (int j = 0; j < 12; j++)
			v[j] = u16(r);
		if (!r->ok || v[0] >= MOVES || (seen & 1u << v[0]) || !v[2] ||
		    v[1] + v[2] > steps || v[3] > MAX_DAMAGE || v[4] > v[3] || !v[5] ||
		    v[5] > MAX_STUN || !v[6] || v[6] > MAX_STUN || v[7] > MAX_HITSTOP ||
		    v[8] > MAX_PUSH || v[9] > MAX_PUSH || v[10] >= GUARD_TYPES ||
		    (v[11] & ~(unsigned)CANCEL_MASK))
			return 0;
		seen |= 1u << v[0];
		c->move[v[0]] = (struct raw_move){
		    v[1],	     v[2],	       (uint16_t)v[3], (uint16_t)v[4],
		    (uint16_t)v[5], (uint16_t)v[6], (uint16_t)v[7], (uint16_t)v[8],
		    (uint16_t)v[9], (uint8_t)v[10], (uint8_t)v[11]};
	}
	for (unsigned i = 0; i < steps; i++) {
		struct raw_step *s = &c->step[i];
		s->frame = (uint16_t)u16(r);
		s->ticks = (uint16_t)u16(r);
		s->box = (uint16_t)u16(r);
		s->hurt = (uint8_t)u8(r);
		s->hit = (uint8_t)u8(r);
		if (!r->ok || s->frame >= nframes || !s->ticks || s->hurt > MAX_STEP_BOXES ||
		    s->hit > MAX_STEP_BOXES || (unsigned)s->box + s->hurt + s->hit > boxes)
			return 0;
	}
	c->box_base = a->boxes;
	for (unsigned i = 0; i < boxes; i++) {
		int x = s16(r), y = s16(r), w = (int)u16(r), h = (int)u16(r);
		if (!r->ok || x < -MAX_SIDE || x > MAX_SIDE || y < -MAX_SIDE || y > MAX_SIDE ||
		    w < 1 || w > MAX_SIDE || h < 1 || h > MAX_SIDE)
			return 0;
		a->box[a->boxes++] =
		    (struct box){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
	}
	/* Actions never carry hitboxes; a move lasts at most MAX_MOVE_TICKS. */
	for (int k = 0; k < ACTIONS; k++)
		for (unsigned i = 0; i < c->count[k]; i++)
			if (c->step[c->first[k] + i].hit)
				return 0;
	for (int k = 0; k < MOVES; k++) {
		unsigned total = 0;
		for (unsigned i = 0; i < c->move[k].count; i++)
			total += c->step[c->move[k].first + i].ticks;
		if (total > MAX_MOVE_TICKS)
			return 0;
	}
	return 1;
}
/* Expand character steps first..first+count with a palette into a->step;
 * runs of hitbox steps get consecutive group numbers. */
static int sequence(struct assets *a, const uint8_t *p, const struct source *frames,
		    const struct palette *pal, const struct raw_character *c,
		    unsigned palette, unsigned first, unsigned count, struct anim *an,
		    uint32_t *charge)
{
	unsigned groups = 0;
	if (count > MAX_STEPS - a->steps)
		return 0;
	*an = (struct anim){(uint16_t)a->steps, (uint16_t)count, 0};
	for (unsigned i = 0; i < count; i++) {
		const struct raw_step *s = &c->step[first + i];
		int im = image(a, p, frames, pal, s->frame, palette, charge);
		if (im < 0 || an->total + s->ticks > 65535)
			return 0;
		if (s->hit && (!i || !c->step[first + i - 1].hit))
			groups++;
		a->step[a->steps++] = (struct step){
		    (uint16_t)im,	  s->ticks, (uint16_t)(c->box_base + s->box),
		    s->hurt, s->hit, (uint8_t)(s->hit ? groups : 0)};
		an->total = (uint16_t)(an->total + s->ticks);
	}
	return 1;
}
/* Frame data the rules and logs use, derived from the expanded steps. */
static void derive(const struct assets *a, struct move *m)
{
	unsigned t = 0, start = 0, end = 0, any = 0;
	m->reach = -MAX_SIDE;
	m->groups = 0;
	for (unsigned i = 0; i < m->anim.count; i++) {
		const struct step *s = &a->step[m->anim.first + i];
		if (s->hit) {
			if (!any)
				start = t;
			any = 1;
			end = t + s->ticks;
			if (s->group > m->groups)
				m->groups = s->group;
			for (unsigned k = 0; k < s->hit; k++) {
				const struct box *b = &a->box[s->box + s->hurt + k];
				if (b->x + b->w > m->reach)
					m->reach = (int16_t)(b->x + b->w);
			}
		}
		t += s->ticks;
	}
	m->startup = (uint8_t)(any ? start : 0);
	m->active = (uint8_t)(any ? end - start : 0);
	m->recovery = (uint8_t)(any ? t - end : t);
}
int fight_assets(struct assets *a, const void *data, size_t size, int *automatic)
{
	static struct palette pal[MAX_PALETTES];
	static struct source frames[MAX_FRAMES];
	static struct raw_character chars[MAX_CHARACTERS];
	struct reader r = {data, 0, size, 1};
	if (!data || size < 36 || memcmp(data, "FIGHTP03", 8))
		return 0;
	r.at = 8;
	uint32_t length = u32(&r), hash = u32(&r), flags = u32(&r);
	uint32_t palettes = u32(&r), nframes = u32(&r), ncharacters = u32(&r),
		 effects = u32(&r);
	if (length != size || flags > 1 || !palettes || palettes > MAX_PALETTES ||
	    !nframes || nframes > MAX_FRAMES || !ncharacters ||
	    ncharacters > MAX_CHARACTERS || effects > MAX_EFFECTS)
		return 0;
	uint32_t check = 2166136261u;
	for (size_t i = 16; i < size; i++)
		check = (check ^ r.p[i]) * 16777619u;
	if (check != hash)
		return 0;
	memset(a, 0, offsetof(struct assets, row));
	a->hash = hash;
	for (unsigned i = 0; i < palettes; i++) {
		pal[i].n = u16(&r);
		if (!r.ok || !pal[i].n || pal[i].n > 256)
			return 0;
		for (unsigned k = 0; k < pal[i].n; k++)
			pal[i].color[k] = (uint16_t)u16(&r);
	}
	for (unsigned i = 0; i < nframes; i++)
		if (!frame(&r, a, &frames[i]))
			return 0;
	for (unsigned i = 0; i < ncharacters; i++)
		if (!character(&r, a, &chars[i], nframes))
			return 0;
	unsigned player[2], palette[2];
	for (int i = 0; i < 2; i++) {
		player[i] = u16(&r);
		palette[i] = u16(&r);
		if (player[i] >= ncharacters || palette[i] >= palettes)
			return 0;
	}
	for (unsigned i = 0; i < effects; i++) {
		unsigned f = u16(&r), c = u16(&r);
		if (f >= nframes || c >= palettes)
			return 0;
		uint32_t charge = 0;
		int im = image(a, r.p, frames, pal, f, c, &charge);
		if (im < 0)
			return 0;
		a->effect[a->effects++] = (uint16_t)im;
	}
	if (!r.ok || r.at != size)
		return 0;
	for (int i = 0; i < 2; i++) {
		const struct raw_character *c = &chars[player[i]];
		struct character *out = &a->chr[i];
		for (int k = 0; k < ACTIONS; k++)
			if (!sequence(a, r.p, frames, pal, c, palette[i], c->first[k],
				      c->count[k], &out->action[k], &out->pixels))
				return 0;
		for (int k = 0; k < MOVES; k++) {
			const struct raw_move *s = &c->move[k];
			struct move *m = &out->move[k];
			if (!sequence(a, r.p, frames, pal, c, palette[i], s->first, s->count,
				      &m->anim, &out->pixels))
				return 0;
			m->damage = s->damage;
			m->chip = s->chip;
			m->hitstun = s->hitstun;
			m->blockstun = s->blockstun;
			m->hitstop = s->hitstop;
			m->pushback = s->pushback;
			m->block_pushback = s->block_pushback;
			m->guard = s->guard;
			m->cancel = s->cancel;
			derive(a, m);
		}
	}
	*automatic = (int)flags;
	return 1;
}

static inline unsigned div255(unsigned x) { return (x + 1 + (x >> 8)) >> 8; }
static void fill(uint16_t *d, int n, uint16_t c)
{
	for (int i = 0; i < n; i++)
		d[i] = c;
}
static void alpha_fill(uint16_t *d, int n, uint16_t c, unsigned a)
{
	/* (src * a + dst * (255 - a) + 127) / 255 per channel. */
	unsigned ia = 255 - a, r = (c >> 11) * a + 127, g = ((c >> 5) & 63) * a + 127,
		 b = (c & 31) * a + 127;
	for (int i = 0; i < n; i++) {
		unsigned e = d[i];
		d[i] = (uint16_t)(div255(r + (e >> 11) * ia) << 11 |
				  div255(g + ((e >> 5) & 63) * ia) << 5 |
				  div255(b + (e & 31) * ia));
	}
}
static void add_fill(uint16_t *d, int n, uint16_t c, unsigned a)
{
	/* dst + src * a / 255 per channel, saturated. */
	unsigned r = (c >> 11) * a / 255, g = ((c >> 5) & 63) * a / 255,
		 b = (c & 31) * a / 255;
	for (int i = 0; i < n; i++) {
		unsigned e = d[i], rr = (e >> 11) + r, gg = ((e >> 5) & 63) + g,
			 bb = (e & 31) + b;
		rr = rr > 31 ? 31 : rr;
		gg = gg > 63 ? 63 : gg;
		bb = bb > 31 ? 31 : bb;
		d[i] = (uint16_t)(rr << 11 | gg << 5 | bb);
	}
}
static void copy_reverse(uint16_t *d, const uint16_t *s, int n)
{
	for (int i = 0; i < n; i++)
		d[i] = s[-i];
}
static void alpha_run(uint16_t *d, const uint16_t *s, int n, int step, unsigned a)
{
	unsigned ia = 255 - a;
	for (int i = 0; i < n; i++, s += step) {
		unsigned c = *s, e = d[i];
		d[i] = (uint16_t)(div255((c >> 11) * a + (e >> 11) * ia + 127) << 11 |
				  div255(((c >> 5) & 63) * a + ((e >> 5) & 63) * ia + 127)
				      << 5 |
				  div255((c & 31) * a + (e & 31) * ia + 127));
	}
}
struct add_table {
	uint8_t r[32], g[64], b[32];
};
static void add_run(uint16_t *d, const uint16_t *s, int n, int step,
		    const struct add_table *t)
{
	for (int i = 0; i < n; i++, s += step) {
		unsigned c = *s, e = d[i], r = (e >> 11) + t->r[c >> 11],
			 g = ((e >> 5) & 63) + t->g[(c >> 5) & 63],
			 b = (e & 31) + t->b[c & 31];
		r = r > 31 ? 31 : r;
		g = g > 63 ? 63 : g;
		b = b > 31 ? 31 : b;
		d[i] = (uint16_t)(r << 11 | g << 5 | b);
	}
}
static void rect(const struct command *c, uint16_t *fb)
{
	int x0 = c->x < 0 ? 0 : c->x, y0 = c->y < 0 ? 0 : c->y;
	int x1 = c->x + c->w > FW ? FW : c->x + c->w,
	    y1 = c->y + c->h > FH ? FH : c->y + c->h;
	if (x0 >= x1 || y0 >= y1 || !c->alpha)
		return;
	int n = x1 - x0;
	uint16_t *d = fb + y0 * FW + x0;
	if (c->blend == ADD)
		for (int y = y0; y < y1; y++, d += FW)
			add_fill(d, n, c->color, c->alpha);
	else if (c->alpha == 255)
		for (int y = y0; y < y1; y++, d += FW)
			fill(d, n, c->color);
	else
		for (int y = y0; y < y1; y++, d += FW)
			alpha_fill(d, n, c->color, c->alpha);
}
static void sprite(const struct assets *a, const struct command *c, uint16_t *fb)
{
	if (c->image < 0 || (unsigned)c->image >= a->images || !c->alpha)
		return;
	const struct image *im = &a->image[c->image];
	int top = c->y < 0 ? -c->y : 0,
	    bottom = c->y + im->h > FH ? FH - c->y : im->h;
	int mode = c->blend == ADD ? ADD : c->alpha == 255 ? OPAQUE : ALPHA;
	struct add_table t;
	if (mode == ADD) {
		for (unsigned v = 0; v < 64; v++) {
			if (v < 32)
				t.r[v] = t.b[v] = (uint8_t)(v * c->alpha / 255);
			t.g[v] = (uint8_t)(v * c->alpha / 255);
		}
	}
	const uint16_t *pixels = a->pixel + im->pixels;
	for (int y = top; y < bottom; y++) {
		uint16_t *d = fb + (c->y + y) * FW;
		const uint32_t *row = a->row + im->first_row + y;
		for (uint32_t k = row[0]; k < row[1]; k++) {
			const struct span *s = &a->span[k];
			int x0 = c->flip ? c->x + im->w - s->x - s->len : c->x + s->x;
			int x1 = x0 + s->len, cx0 = x0 < 0 ? 0 : x0,
			    cx1 = x1 > FW ? FW : x1;
			if (cx0 >= cx1)
				continue;
			int n = cx1 - cx0, step = c->flip ? -1 : 1;
			/* Source pixel for screen column cx0, walking by step. */
			const uint16_t *src = pixels + s->pixel +
					      (c->flip ? x1 - 1 - cx0 : cx0 - x0);
			if (mode == OPAQUE) {
				if (c->flip)
					copy_reverse(d + cx0, src, n);
				else
					memcpy(d + cx0, src, (size_t)n * 2);
			} else if (mode == ALPHA)
				alpha_run(d + cx0, src, n, step, c->alpha);
			else
				add_run(d + cx0, src, n, step, &t);
		}
	}
}
void fight_render(const struct assets *a, const struct scene *s, uint16_t *fb)
{
	for (unsigned k = 0; k < s->count && k < COMMANDS; k++) {
		const struct command *c = &s->cmd[k];
		if (c->op == RECT)
			rect(c, fb);
		else if (c->op == SPRITE)
			sprite(a, c, fb);
	}
}
