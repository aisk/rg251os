/* FIGHTP06 pack decoder, per-match cast and clipped RGB565 software
 * draw-command backend. The decoder checks every count, offset and index
 * against the file length and the resident limits in fighter.h (palettes
 * and frames per sheet, system profiles with walls and effect sets, stats,
 * names, box bounds and counts, move, special, projectile and step motion
 * data, spark animations, stages with their layers, edge rules and anims),
 * keeps the whole roster's rule data and palette-index frames and every
 * stage's palette-index layers resident, resolves each character's stats
 * (its system's defaults plus its overrides, with the jump's peak and fall
 * speed derived and bounded), fills hitstun/blockstun/hitstop a move takes
 * from its class and scales damage and chip by the system's damage scale,
 * derives hit groups, startup/active/recovery, reach and the weapon flag per
 * move, and expands the effects and sparks once. fight_cast expands the two
 * fighters of the current match (character, palette) and its stage into
 * RGB565 runs when they change, so only two characters' and one stage's
 * pixels are ever resident; the loader proves any pair plus any stage
 * fits. A stage layer is expanded into one image that covers every screen
 * pixel it can reach for any camera and shake, with its edge rules already
 * applied, so drawing it is the sprite path: an opaque row is one span, a
 * row copy. Drawing a sprite is span
 * copies (mirroring reverses the span), rectangles are row fills, and
 * constant alpha/additive blending have their own loops without per-pixel
 * mode branches or divisions. All destination reads stay in the cached
 * canvas; the host presents it. tools/fighter/pack.py documents the file
 * layout. */
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
static int s8(struct reader *r) { return (int)(int8_t)u8(r); }
static uint32_t u32(struct reader *r)
{
	uint32_t lo = u16(r);
	return lo | (uint32_t)u16(r) << 16;
}
static int frame(struct reader *r, struct assets *a, struct frame *f)
{
	f->sheet = (uint16_t)u16(r);
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
	f->index = a->indices;
	if (pixels > MAX_INDICES - a->indices || r->size - r->at < pixels)
		return 0;
	memcpy(a->index + a->indices, r->p + r->at, pixels);
	r->at += pixels;
	a->indices += pixels;
	f->max_index = 0;
	for (uint32_t i = 0; i < pixels; i++)
		if (a->index[f->index + i] > f->max_index)
			f->max_index = a->index[f->index + i];
	return 1;
}
/* Can frame f be drawn with palette c? */
static int paints(const struct assets *a, unsigned f, unsigned c)
{
	return a->frame[f].sheet == a->palette_sheet[c] &&
	       a->frame[f].max_index < a->palette_size[c];
}
/* Expand frame f with palette c into a new image; the loader has already
 * proven the limits, so this cannot overflow for a cast character. */
static int image(struct assets *a, unsigned f, unsigned c)
{
	const struct frame *s = &a->frame[f];
	if (a->images >= MAX_IMAGES || s->count > MAX_PIXELS - a->pixels)
		return -1;
	struct image *im = &a->image[a->images];
	*im = (struct image){s->w,	   s->h,      s->ax,	       s->ay,
			     s->first_row, a->pixels, s->count, (uint16_t)f,
			     (uint16_t)c};
	for (uint32_t i = 0; i < s->count; i++)
		a->pixel[a->pixels + i] = a->palette[c][a->index[s->index + i]];
	a->pixels += s->count;
	return (int)a->images++;
}
/* ---- systems and stats ---- */
/* Stats in pack order; a character's override mask has one bit per entry. */
enum stat {
	S_LIFE, S_DEFENSE, S_WALK, S_WALK_BACK, S_JUMP_VY, S_GRAVITY, S_VX, S_VX_BACK,
	S_PREJUMP, S_WEIGHT, S_HEIGHT, STATS
};
static int16_t *stat_field(struct stats *st, int k)
{
	int16_t *f[STATS] = {&st->life,	     &st->defense,    &st->walk,	 &st->walk_back,
			     &st->jump.vy,   &st->jump.gravity, &st->jump.vx, &st->jump.vx_back,
			     &st->jump.prejump, &st->weight,     NULL};
	return f[k];
}
/* Store packed value v of stat k; 0 if it cannot be one (stats_ok checks
 * the ranges once all are set). */
static int set_stat(struct stats *st, int k, unsigned v)
{
	if (k == S_HEIGHT) {
		st->height = (uint8_t)v;
		return v < HEIGHTS;
	}
	*stat_field(st, k) = (int16_t)v;
	return k == S_JUMP_VY ? v >= 0x8000 : v < 0x8000;
}
/* Simulate the jump arc and a fall from MAX_HEIGHT (as after a hit during a
 * tall move) the way fighter.c moves airborne fighters; fall is the largest
 * vy either leaves in the state. */
static void arc(struct jump *j)
{
	int y = 0, vy = j->vy, peak = 0, fall = 0;
	for (;;) {
		y += vy / 4;
		vy += j->gravity;
		if (y >= 0)
			break;
		peak = y < peak ? y : peak;
		fall = vy > fall ? vy : fall;
	}
	for (y = -MAX_HEIGHT, vy = 0;;) {
		y += vy / 4;
		vy += j->gravity;
		if (y >= 0)
			break;
		fall = vy > fall ? vy : fall;
	}
	j->peak = (int16_t)-peak;
	j->fall = (int16_t)fall;
}
static int stats_ok(struct stats *st)
{
	if (st->life < 1 || st->life > MAX_LIFE || st->defense < MIN_DEFENSE ||
	    st->defense > MAX_PERMILLE || st->walk < 0 || st->walk > MAX_WALK ||
	    st->walk_back < 0 || st->walk_back > MAX_WALK || st->jump.vy < -MAX_JUMP_VY ||
	    st->jump.vy > -1 || st->jump.gravity < 1 || st->jump.gravity > MAX_GRAVITY ||
	    st->jump.vx < 0 || st->jump.vx > MAX_WALK || st->jump.vx_back < 0 ||
	    st->jump.vx_back > MAX_WALK || st->jump.prejump < 1 ||
	    st->jump.prejump > MAX_PREJUMP || st->weight < MIN_WEIGHT ||
	    st->weight > MAX_PERMILLE || st->height >= HEIGHTS)
		return 0;
	arc(&st->jump);
	return st->jump.peak <= MAX_HEIGHT;
}
static int system_name_char(unsigned c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
static int read_system(struct reader *r, struct system *sy, unsigned sparks)
{
	unsigned len = u8(r);
	if (!r->ok || !len || len > MAX_NAME)
		return 0;
	memset(sy, 0, sizeof(*sy));
	for (unsigned i = 0; i < len; i++) {
		sy->name[i] = (char)u8(r);
		if (!system_name_char((unsigned char)sy->name[i]))
			return 0;
	}
	unsigned scale = u16(r);
	if (scale < MIN_SCALE || scale > MAX_PERMILLE)
		return 0;
	sy->damage_scale = (uint16_t)scale;
	for (int k = 0; k < STATS; k++) {
		if (!set_stat(&sy->stats, k, u16(r)))
			return 0;
	}
	struct stats *st = &sy->stats;
	unsigned v[8];
	for (int k = 0; k < 8; k++)
		v[k] = u16(r);
	if (!r->ok || !stats_ok(st) || v[0] < 2 || v[0] > MAX_PUSH_DISTANCE || v[1] > FW ||
	    !v[2] || v[2] > MAX_BUFFER || v[3] < 3 || v[3] > HISTORY || v[4] < 3 ||
	    v[4] > HISTORY || !v[5] || v[5] > MAX_CHARGE || v[6] > MAX_GRACE || !v[7] ||
	    v[7] > HISTORY)
		return 0;
	st->push = (int16_t)v[0];
	st->guard_range = (int16_t)v[1];
	st->buffer = (uint8_t)v[2];
	st->quarter = (uint8_t)v[3];
	st->dragon = (uint8_t)v[4];
	st->charge = (uint8_t)v[5];
	st->grace = (uint8_t)v[6];
	st->release = (uint8_t)v[7];
	/* Class rows from light: hitstun, blockstun, hitstop, which are set. */
	for (int c = CLASS_LIGHT; c < CLASSES; c++) {
		unsigned hitstun = u16(r), blockstun = u16(r), hitstop = u16(r), has = u16(r);
		if (!r->ok || has > 3 ||
		    ((has & 1) ? !hitstun || hitstun > MAX_STUN || !blockstun ||
				     blockstun > MAX_STUN
			       : hitstun || blockstun) ||
		    ((has & 2) ? hitstop > MAX_HITSTOP : hitstop != 0))
			return 0;
		sy->cls[c] = (struct class_row){(uint8_t)hitstun, (uint8_t)blockstun,
						(uint8_t)hitstop, (uint8_t)has};
	}
	unsigned margin = u16(r), play = u16(r);
	if (!r->ok || margin < MIN_MARGIN || margin > MAX_MARGIN || play < MIN_PLAY_WIDTH ||
	    play > FW)
		return 0;
	sy->margin = (uint16_t)margin;
	sy->play_width = (uint16_t)play;
	for (int k = 0; k < FX_KEYS; k++) {
		unsigned e = u16(r);
		if (!r->ok || e > sparks)
			return 0;
		sy->effect[k] = (uint16_t)e;
	}
	return 1;
}
/* A character's stats: its system's, then each override in the mask. */
static int read_stats(struct reader *r, const struct assets *a, struct character *c)
{
	unsigned system = u16(r), mask = u16(r);
	if (!r->ok || system >= a->systems || mask >> STATS)
		return 0;
	c->system = system;
	c->stats = a->sys[system].stats;
	for (int k = 0; k < STATS; k++) {
		unsigned v = u16(r);
		if (!((mask >> k) & 1)) {
			if (v)
				return 0;
			continue;
		}
		if (!set_stat(&c->stats, k, v))
			return 0;
	}
	return r->ok && stats_ok(&c->stats);
}
/* damage .. block_pushback, then (after the caller's fields) class and
 * defaults: a default bit takes that field from the class row, whose value
 * in the pack must be 0. Damage and chip are scaled by the system. */
static int hit_fields(struct reader *r, unsigned v[7])
{
	for (int j = 0; j < 7; j++)
		v[j] = u16(r);
	return r->ok;
}
static int resolve_hit(struct hit *h, unsigned v[7], unsigned cls, unsigned defaults,
		       const struct system *sy)
{
	if (cls >= CLASSES || defaults > 7 || (defaults && !cls))
		return 0;
	const struct class_row *row = &sy->cls[cls];
	if ((defaults & (DEFAULT_HITSTUN | DEFAULT_BLOCKSTUN)) && !(row->has & 1))
		return 0;
	if ((defaults & DEFAULT_HITSTOP) && !(row->has & 2))
		return 0;
	static const unsigned bit[3] = {DEFAULT_HITSTUN, DEFAULT_BLOCKSTUN, DEFAULT_HITSTOP};
	const unsigned value[3] = {row->hitstun, row->blockstun, row->hitstop};
	for (int k = 0; k < 3; k++)
		if (defaults & bit[k]) {
			if (v[2 + k])
				return 0;
			v[2 + k] = value[k];
		}
	if (v[0] > MAX_DAMAGE || v[1] > v[0] || !v[2] || v[2] > MAX_STUN || !v[3] ||
	    v[3] > MAX_STUN || v[4] > MAX_HITSTOP || v[5] > MAX_PUSH || v[6] > MAX_PUSH)
		return 0;
	/* Rounded half up; a nonzero value stays at least 1. */
	for (int k = 0; k < 2; k++) {
		unsigned scaled = (v[k] * sy->damage_scale + 500) / 1000;
		v[k] = v[k] && !scaled ? 1 : scaled;
	}
	*h = (struct hit){(uint16_t)v[0], (uint16_t)v[1], (uint16_t)v[2], (uint16_t)v[3],
			  (uint16_t)v[4], (uint16_t)v[5], (uint16_t)v[6], 0, (uint8_t)cls};
	return 1;
}
static int anim(struct anim *an, unsigned first, unsigned count, unsigned steps,
		unsigned base)
{
	if (!count || first + count > steps)
		return 0;
	*an = (struct anim){(uint16_t)(base + first), (uint16_t)count, 0};
	return 1;
}
static unsigned total(const struct assets *a, struct anim *an)
{
	unsigned t = 0;
	for (unsigned i = 0; i < an->count; i++)
		t += a->step[an->first + i].ticks;
	return t;
}
/* Hit groups, startup/active/recovery and reach of a move; 0 if its steps
 * already carry different groups from another move, its motion leaves the
 * ground band or does not end on the ground, or it is too long. */
static int derive(struct assets *a, struct move *m)
{
	unsigned t = 0, start = 0, end = 0, any = 0, groups = 0;
	int height = 0;
	m->reach = -MAX_SIDE;
	m->spawn = 0;
	for (unsigned i = 0; i < m->anim.count; i++) {
		struct step *s = &a->step[m->anim.first + i];
		unsigned group = 0;
		if (s->hit) {
			if (!i || !a->step[m->anim.first + i - 1].hit)
				groups++;
			group = groups;
			if (!any)
				start = t;
			any = 1;
			end = t + s->ticks;
			for (unsigned k = 0; k < s->hit; k++) {
				const struct box *b = &a->box[s->box + s->hurt + k];
				if (b->x + b->w > m->reach)
					m->reach = (int16_t)(b->x + b->w);
			}
		}
		if (s->group && s->group != group)
			return 0;
		s->group = (uint8_t)group;
		if (s->spawn && !m->spawn)
			m->spawn = s->spawn;
		height += s->vy * (int)s->ticks;
		if (height > 0 || height < -MAX_HEIGHT)
			return 0;
		t += s->ticks;
	}
	if (height || t > MAX_MOVE_TICKS)
		return 0;
	m->anim.total = (uint16_t)t;
	m->groups = (uint8_t)groups;
	m->startup = (uint8_t)(any ? start : 0);
	m->active = (uint8_t)(any ? end - start : 0);
	m->recovery = (uint8_t)(any ? t - end : t);
	return 1;
}
static int name_char(unsigned c)
{
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-';
}
static int character(struct reader *r, struct assets *a, struct character *c, unsigned stages)
{
	static uint32_t mark[MAX_FRAMES], stamp;
	unsigned len = u8(r);
	if (!r->ok || !len || len > MAX_NAME)
		return 0;
	memset(c, 0, sizeof(*c));
	for (unsigned i = 0; i < len; i++) {
		c->name[i] = (char)u8(r);
		if (!name_char((unsigned char)c->name[i]))
			return 0;
	}
	c->palette[0] = (uint16_t)u16(r);
	c->palette[1] = (uint16_t)u16(r);
	if (!read_stats(r, a, c))
		return 0;
	const struct system *sy = &a->sys[c->system];
	unsigned steps = u16(r), boxes = u16(r), moves = u16(r), projectiles = u16(r);
	if (!r->ok || c->palette[0] >= a->palettes || c->palette[1] >= a->palettes ||
	    !steps || steps > MAX_CHARACTER_STEPS || steps > MAX_STEPS - a->steps ||
	    boxes > MAX_BOXES - a->boxes || moves < MOVES || moves > MAX_MOVES ||
	    projectiles > MAX_PROJECTILE_TYPES)
		return 0;
	unsigned base = a->steps, v[7];
	c->first_step = base;
	c->steps = steps;
	c->moves = moves;
	c->projectiles = projectiles;
	for (int k = 0; k < ACTIONS; k++) {
		unsigned first = u16(r), count = u16(r);
		if (!anim(&c->action[k], first, count, steps, base))
			return 0;
	}
	/* The eight normals in trigger order, then the specials. */
	for (unsigned k = 0; k < moves; k++) {
		struct move *m = &c->move[k];
		unsigned kind = u16(r), trigger = u16(r), buttons = u16(r), first = u16(r),
			 count = u16(r);
		if (!hit_fields(r, v))
			return 0;
		unsigned guard = u16(r), cancel = u16(r), cls = u16(r), defaults = u16(r);
		if (!r->ok || !resolve_hit(&m->hit, v, cls, defaults, sy) || kind != (k >= MOVES) ||
		    (kind ? trigger >= MOTIONS || !buttons || buttons > 15
			  : trigger != k || buttons) ||
		    guard >= GUARD_TYPES || (cancel & ~(unsigned)CANCEL_MASK) ||
		    !anim(&m->anim, first, count, steps, base))
			return 0;
		m->hit.guard = (uint8_t)guard;
		m->cancel = (uint8_t)cancel;
		m->special = (uint8_t)kind;
		m->motion = (uint8_t)(kind ? trigger : 0);
		m->buttons = (uint8_t)buttons;
	}
	for (unsigned k = 0; k < projectiles; k++) {
		struct projectile *p = &c->projectile[k];
		unsigned first = u16(r), count = u16(r);
		int speed = s16(r), lifetime = s16(r), x = s16(r), y = s16(r);
		if (!hit_fields(r, v))
			return 0;
		unsigned guard = u16(r), blend = u16(r), alpha = u16(r), cls = u16(r),
			 defaults = u16(r);
		if (!r->ok || !resolve_hit(&p->hit, v, cls, defaults, sy) ||
		    !anim(&p->anim, first, count, steps, base) || speed < 1 ||
		    speed > MAX_VELOCITY || lifetime < 1 || lifetime > MAX_LIFETIME ||
		    x < -MAX_SIDE || x > MAX_SIDE || y < -MAX_SIDE || y > MAX_SIDE ||
		    guard >= GUARD_TYPES || blend >= BLENDS || !alpha || alpha > 255)
			return 0;
		p->speed = (int16_t)speed;
		p->lifetime = (uint16_t)lifetime;
		p->x = (int16_t)x;
		p->y = (int16_t)y;
		p->hit.guard = (uint8_t)guard;
		p->blend = (uint8_t)blend;
		p->alpha = (uint8_t)alpha;
	}
	for (unsigned i = 0; i < steps; i++) {
		struct step *s = &a->step[base + i];
		unsigned f = u16(r), ticks = u16(r), box = u16(r), hurt = u8(r), hit = u8(r);
		int vx = s8(r), vy = s8(r);
		unsigned spawn = u8(r);
		if (!r->ok || f >= a->frames || !ticks || hurt > MAX_STEP_BOXES ||
		    hit > MAX_STEP_BOXES || box + hurt + hit > boxes || vx < -MAX_VELOCITY ||
		    vx > MAX_VELOCITY || vy < -MAX_VELOCITY || vy > MAX_VELOCITY ||
		    spawn > projectiles)
			return 0;
		*s = (struct step){(uint16_t)f,		   (uint16_t)ticks,
				   (uint16_t)(a->boxes + box), (uint8_t)hurt,
				   (uint8_t)hit,	   0,
				   (uint8_t)spawn,	   (int8_t)vx,
				   (int8_t)vy};
	}
	a->steps += steps;
	for (unsigned i = 0; i < boxes; i++) {
		int x = s16(r), y = s16(r), w = (int)u16(r), h = (int)u16(r);
		if (!r->ok || x < -MAX_SIDE || x > MAX_SIDE || y < -MAX_SIDE || y > MAX_SIDE ||
		    w < 1 || w > MAX_SIDE || h < 1 || h > MAX_SIDE)
			return 0;
		a->box[a->boxes++] =
		    (struct box){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
	}
	unsigned slash = u16(r), home = u16(r);
	if (!r->ok || slash > 31 || home > stages)
		return 0;
	c->slash = (uint8_t)slash;
	c->home = (uint16_t)home;
	for (unsigned k = 0; k < moves; k++)
		c->move[k].slash = (uint8_t)((slash >> (k < MOVES ? k % 4 : 4)) & 1);
	/* Actions never hit, move or spawn; projectile steps have no body, no
	 * motion and always a hitbox. */
	for (int k = 0; k < ACTIONS; k++) {
		struct anim *an = &c->action[k];
		for (unsigned i = 0; i < an->count; i++) {
			const struct step *s = &a->step[an->first + i];
			if (s->hit || s->vx || s->vy || s->spawn)
				return 0;
		}
		unsigned t = total(a, an);
		if (t > 65535)
			return 0;
		an->total = (uint16_t)t;
	}
	for (unsigned k = 0; k < projectiles; k++) {
		struct anim *an = &c->projectile[k].anim;
		for (unsigned i = 0; i < an->count; i++) {
			const struct step *s = &a->step[an->first + i];
			if (s->hurt || !s->hit || s->vx || s->vy || s->spawn)
				return 0;
		}
		unsigned t = total(a, an);
		if (t > 65535)
			return 0;
		an->total = (uint16_t)t;
	}
	for (unsigned k = 0; k < moves; k++)
		if (!derive(a, &c->move[k]))
			return 0;
	/* Every frame the character shows paints with both its palettes; count
	 * what a cast of it costs. */
	stamp++;
	for (unsigned i = 0; i < steps; i++) {
		unsigned f = a->step[base + i].frame;
		if (!paints(a, f, c->palette[0]) || !paints(a, f, c->palette[1]))
			return 0;
		if (mark[f] != stamp) {
			mark[f] = stamp;
			c->pixels += a->frame[f].count;
			c->images++;
		}
	}
	return 1;
}
/* ---- sparks and stages ---- */
static int read_sparks(struct reader *r, struct assets *a, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		unsigned blend = u8(r), alpha = u8(r), steps = u16(r);
		if (!r->ok || blend >= BLENDS || !alpha || !steps || steps > MAX_SPARK_STEPS)
			return 0;
		struct spark *sp = &a->spark[i];
		*sp = (struct spark){(uint16_t)a->spark_steps, (uint16_t)steps, 0, (uint8_t)blend,
				     (uint8_t)alpha};
		unsigned total = 0;
		for (unsigned k = 0; k < steps; k++) {
			unsigned f = u16(r), c = u16(r), ticks = u16(r);
			if (!r->ok || f >= a->frames || c >= a->palettes || !paints(a, f, c) || !ticks ||
			    ticks > 255)
				return 0;
			int im = image(a, f, c);
			if (im < 0)
				return 0;
			a->spark_step[a->spark_steps++] = (struct spark_step){(uint16_t)im, (uint16_t)ticks};
			total += ticks;
		}
		sp->total = (uint16_t)total;
	}
	a->sparks = n;
	return 1;
}
static int floor_div(int n, int d) { return n >= 0 ? n / d : -((-n + d - 1) / d); }
/* Screen x of layer l's left edge with the camera at cam. */
int fight_layer_x(const struct layer *l, int cam)
{
	return l->x - floor_div(cam * l->num, l->den);
}
/* The layer pixel range [lo, hi) an axis of size n needs to cover screen
 * pixels [0, screen) when the layer origin moves over [omin, omax]; an
 * axis without an edge rule stays inside the layer. */
static void cover(int omin, int omax, int screen, int n, int edge, int *lo, int *hi)
{
	*lo = -omax;
	*hi = screen - omin;
	if (edge == EDGE_NONE) {
		*lo = *lo < 0 ? 0 : *lo;
		*hi = *hi > n ? n : *hi;
	}
}
/* Layer coordinate v mapped into 0..n-1 by an edge rule; -1 = transparent,
 * -2 = the fill colour. */
static int edge_map(int v, int n, int edge)
{
	if (v >= 0 && v < n)
		return v;
	if (edge == EDGE_EXTEND)
		return v < 0 ? 0 : n - 1;
	if (edge == EDGE_REPEAT)
		return ((v % n) + n) % n;
	if (edge == EDGE_MIRROR) {
		int m = ((v % (2 * n)) + 2 * n) % (2 * n);
		return m < n ? m : 2 * n - 1 - m;
	}
	return edge == EDGE_COLOR ? -2 : -1;
}
struct need {
	uint32_t pixels, rows, spans;
	unsigned images;
};
/* Expand layer l of a stage W wide into one image covering every pixel it
 * can show (any camera, any shake), or only count what that takes when
 * commit is 0. Returns the image (-1: nothing visible, -2: too large) and
 * the covered origin in layer pixels. */
static int expand_layer(struct assets *a, const struct stage *st, const struct layer *l,
			int commit, struct need *n, int *x0, int *y0)
{
	static int16_t line[MAX_SIDE];
	const struct frame *f = &a->frame[l->frame];
	int left = -f->ax, top = -f->ay, cmax = st->width - FW;
	int sx0 = fight_layer_x(l, 0), sx1 = fight_layer_x(l, cmax),
	    sy = GROUND - st->ground + l->y;
	int xl, xh, yl, yh;
	cover(sx0 < sx1 ? sx0 : sx1, sx0 < sx1 ? sx1 : sx0, FW, l->w, l->edge[0], &xl, &xh);
	cover(sy - SHAKE_MAX, sy + SHAKE_MAX, FH, l->h, l->edge[1], &yl, &yh);
	*x0 = xl;
	*y0 = yl;
	if (xl >= xh || yl >= yh)
		return -1;
	int w = xh - xl, h = yh - yl;
	if (w > MAX_COVER)
		return -2;
	uint32_t row0 = a->rows, span0 = a->spans, pix0 = a->pixels, pixels = 0, spans = 0;
	const uint16_t *pal = a->palette[l->palette];
	int cached = -3;
	for (int y = 0; y < h; y++) {
		int sr = edge_map(yl + y, l->h, l->edge[1]);
		if (sr >= 0 && sr != cached) {
			/* Palette indices of source row sr, -1 where transparent. */
			for (int i = 0; i < l->w; i++)
				line[i] = -1;
			int fr = sr - top;
			if (fr >= 0 && fr < f->h)
				for (uint32_t k = a->row[f->first_row + fr];
				     k < a->row[f->first_row + fr + 1]; k++) {
					const struct span *sp = &a->span[k];
					for (unsigned i = 0; i < sp->len; i++)
						line[left + sp->x + i] =
						    a->index[f->index + sp->pixel + i];
				}
			cached = sr;
		}
		if (commit)
			a->row[row0 + (unsigned)y] = span0 + spans;
		int x = 0;
		while (x < w) {
			int c = -1;
			for (; x < w; x++) {
				int sc = edge_map(xl + x, l->w, l->edge[0]);
				c = sr == -2 || sc == -2 ? -2 : sr < 0 || sc < 0 ? -1 : line[sc];
				if (c != -1)
					break;
			}
			if (x == w)
				break;
			int start = x;
			for (; x < w; x++) {
				int sc = edge_map(xl + x, l->w, l->edge[0]);
				c = sr == -2 || sc == -2 ? -2 : sr < 0 || sc < 0 ? -1 : line[sc];
				if (c == -1)
					break;
				if (commit)
					a->pixel[pix0 + pixels + (unsigned)(x - start)] =
					    c == -2 ? l->color : pal[c];
			}
			if (commit)
				a->span[span0 + spans] = (struct span){
				    (uint16_t)start, (uint16_t)(x - start), pixels};
			pixels += (uint32_t)(x - start);
			spans++;
		}
	}
	n->pixels += pixels;
	n->rows += (uint32_t)h + 1;
	n->spans += spans;
	n->images++;
	if (!commit)
		return 0;
	a->row[row0 + (unsigned)h] = span0 + spans;
	a->rows += (unsigned)h + 1;
	a->spans += spans;
	a->pixels += pixels;
	struct image *im = &a->image[a->images];
	*im = (struct image){w, h, 0, 0, row0, pix0, pixels, l->frame, l->palette};
	return (int)a->images++;
}
/* Expand (commit) or count stage k: its layers and every anim step frame. */
static int expand_stage(struct assets *a, unsigned k, int commit, struct need *n)
{
	const struct stage *st = &a->stage[k];
	for (unsigned i = 0; i < st->layers; i++) {
		const struct layer *l = &a->layer[st->first_layer + i];
		int x0, y0, im = expand_layer(a, st, l, commit, n, &x0, &y0);
		if (im == -2)
			return 0;
		if (commit) {
			a->layer_image[i] = im;
			a->layer_x0[i] = (int16_t)x0;
			a->layer_y0[i] = (int16_t)y0;
		}
		for (unsigned j = 0; j < l->anims; j++) {
			const struct stage_anim *an = &a->anim[l->first_anim + j];
			for (unsigned t = 0; t < an->count; t++) {
				const struct stage_step *sp = &a->stage_step[an->first + t];
				int got = -1;
				if (sp->frame != 0xffff) {
					n->pixels += a->frame[sp->frame].count;
					n->images++;
					if (commit)
						got = image(a, sp->frame, l->palette);
				}
				if (commit)
					a->stage_look[an->first + t] = (int16_t)got;
			}
		}
	}
	return 1;
}
static int read_stages(struct reader *r, struct assets *a, unsigned n)
{
	for (unsigned k = 0; k < n; k++) {
		struct stage *st = &a->stage[k];
		char name[MAX_NAME + 1] = {0};
		unsigned len = u8(r);
		if (!r->ok || !len || len > MAX_NAME)
			return 0;
		for (unsigned i = 0; i < len; i++) {
			name[i] = (char)u8(r);
			if (!name_char((unsigned char)name[i]))
				return 0;
		}
		unsigned width = u16(r), ground = u16(r), system = u16(r), layers = u16(r);
		if (!r->ok || width < MIN_STAGE_WIDTH || width > MAX_STAGE_WIDTH || ground > MAX_SIDE ||
		    system >= a->systems || !layers || layers > MAX_LAYERS ||
		    layers > MAX_STAGE_LAYERS - a->layers)
			return 0;
		*st = (struct stage){{0}, (uint16_t)width, (uint16_t)ground, (uint16_t)system,
				     (uint16_t)a->layers, (uint16_t)layers, 0, 0, 0, 0};
		memcpy(st->name, name, sizeof(name));
		for (unsigned i = 0; i < layers; i++) {
			struct layer *l = &a->layer[a->layers++];
			unsigned f = u16(r), c = u16(r), w = u16(r), h = u16(r);
			int x = s16(r), y = s16(r), num = s16(r);
			unsigned den = u16(r), ex = u8(r), ey = u8(r), color = u16(r), anims = u16(r);
			if (!r->ok || f >= a->frames || c >= a->palettes || !paints(a, f, c) || !w ||
			    w > MAX_SIDE || !h || h > MAX_SIDE || x < -MAX_SIDE || x > MAX_SIDE ||
			    y < -MAX_SIDE || y > MAX_SIDE || num < -MAX_SIDE || num > MAX_SIDE || !den ||
			    den > MAX_STAGE_WIDTH || ex >= EDGES || ey >= EDGES ||
			    anims > MAX_LAYER_ANIMS || anims > MAX_STAGE_ANIMS - a->anims)
				return 0;
			const struct frame *fr = &a->frame[f];
			/* The layer frame lies inside its w x h rectangle. */
			if (fr->ax > 0 || fr->ay > 0 || -fr->ax + fr->w > (int)w ||
			    -fr->ay + fr->h > (int)h)
				return 0;
			*l = (struct layer){(uint16_t)f, (uint16_t)c, (uint16_t)w, (uint16_t)h,
					    (int16_t)x, (int16_t)y, (int16_t)num, (uint16_t)den,
					    {(uint8_t)ex, (uint8_t)ey}, (uint16_t)color,
					    (uint16_t)a->anims, (uint16_t)anims};
			for (unsigned j = 0; j < anims; j++) {
				struct stage_anim *an = &a->anim[a->anims++];
				unsigned steps = u16(r);
				if (!r->ok || !steps || steps > MAX_ANIM_STEPS ||
				    steps > MAX_STAGE_STEPS - a->stage_steps)
					return 0;
				*an = (struct stage_anim){(uint16_t)a->stage_steps, (uint16_t)steps, 0};
				for (unsigned t = 0; t < steps; t++) {
					unsigned sf = u16(r);
					int sx = s16(r), sy = s16(r);
					unsigned ticks = u16(r);
					if (!r->ok || (sf != 0xffff && (sf >= a->frames || !paints(a, sf, c))) ||
					    sx < -MAX_SIDE || sx > MAX_SIDE || sy < -MAX_SIDE ||
					    sy > MAX_SIDE || !ticks)
						return 0;
					a->stage_step[a->stage_steps++] =
					    (struct stage_step){(uint16_t)sf, (uint16_t)ticks, (int16_t)sx,
								(int16_t)sy};
					an->total += ticks;
				}
			}
		}
		struct need need = {0, 0, 0, 0};
		if (!expand_stage(a, k, 0, &need))
			return 0;
		st->pixels = need.pixels;
		st->rows = need.rows;
		st->spans = need.spans;
		st->images = need.images;
	}
	a->stages = n;
	return 1;
}
int fight_assets(struct assets *a, const void *data, size_t size, int *automatic)
{
	struct reader r = {data, 0, size, 1};
	if (!data || size < 48 || memcmp(data, "FIGHTP06", 8))
		return 0;
	r.at = 8;
	uint32_t length = u32(&r), hash = u32(&r), flags = u32(&r);
	uint32_t palettes = u32(&r), nframes = u32(&r), ncharacters = u32(&r),
		 effects = u32(&r), systems = u32(&r), sparks = u32(&r), stages = u32(&r);
	if (length != size || flags > 1 || !palettes || palettes > MAX_PALETTES ||
	    !nframes || nframes > MAX_FRAMES || !ncharacters ||
	    ncharacters > MAX_CHARACTERS || effects > MAX_EFFECTS || !systems ||
	    systems > MAX_SYSTEMS || sparks > MAX_SPARKS || stages > MAX_STAGES)
		return 0;
	uint32_t check = 2166136261u;
	for (size_t i = 16; i < size; i++)
		check = (check ^ r.p[i]) * 16777619u;
	if (check != hash)
		return 0;
	memset(a, 0, offsetof(struct assets, palette_sheet));
	a->hash = hash;
	a->palettes = palettes;
	for (unsigned i = 0; i < palettes; i++) {
		a->palette_sheet[i] = (uint16_t)u16(&r);
		a->palette_size[i] = (uint16_t)u16(&r);
		if (!r.ok || !a->palette_size[i] || a->palette_size[i] > 256)
			return 0;
		for (unsigned k = 0; k < a->palette_size[i]; k++)
			a->palette[i][k] = (uint16_t)u16(&r);
	}
	a->frames = nframes;
	for (unsigned i = 0; i < nframes; i++)
		if (!frame(&r, a, &a->frame[i]))
			return 0;
	if (!read_sparks(&r, a, sparks))
		return 0;
	a->systems = systems;
	for (unsigned i = 0; i < systems; i++)
		if (!read_system(&r, &a->sys[i], sparks))
			return 0;
	a->characters = ncharacters;
	for (unsigned i = 0; i < ncharacters; i++)
		if (!character(&r, a, &a->chr[i], stages))
			return 0;
	for (unsigned i = 0; i < effects; i++) {
		unsigned f = u16(&r), c = u16(&r);
		if (!r.ok || f >= nframes || c >= palettes || !paints(a, f, c))
			return 0;
		int im = image(a, f, c);
		if (im < 0)
			return 0;
		a->effect[a->effects++] = (uint16_t)im;
	}
	if (!read_stages(&r, a, stages) || !r.ok || r.at != size)
		return 0;
	/* Any two cast characters (a mirror match casts one twice) and any
	 * stage must fit. */
	uint32_t pixels = 0, rows = 0, spans = 0;
	unsigned images = 0, stage_images = 0;
	for (unsigned i = 0; i < ncharacters; i++)
		for (unsigned j = i; j < ncharacters; j++) {
			uint32_t p = a->chr[i].pixels + a->chr[j].pixels;
			unsigned n = a->chr[i].images + a->chr[j].images;
			pixels = p > pixels ? p : pixels;
			images = n > images ? n : images;
		}
	uint32_t stage_pixels = 0;
	for (unsigned k = 0; k < stages; k++) {
		const struct stage *st = &a->stage[k];
		stage_pixels = st->pixels > stage_pixels ? st->pixels : stage_pixels;
		rows = st->rows > rows ? st->rows : rows;
		spans = st->spans > spans ? st->spans : spans;
		stage_images = st->images > stage_images ? st->images : stage_images;
	}
	if (pixels > MAX_PIXELS - a->pixels || stage_pixels > MAX_PIXELS - a->pixels - pixels ||
	    images > MAX_IMAGES - a->images || stage_images > MAX_IMAGES - a->images - images ||
	    rows > MAX_ROWS + 1 - a->rows || spans > MAX_SPANS - a->spans)
		return 0;
	a->effect_images = a->images;
	a->effect_pixels = a->pixels;
	a->pack_rows = a->rows;
	a->pack_spans = a->spans;
	a->cast[0][0] = a->cast[1][0] = -1;
	a->cast_stage = -1;
	*automatic = (int)flags;
	return 1;
}
int fight_cast(struct assets *a, const struct fight_state *s)
{
	int stage = a->stages ? (int)(s->arena < a->stages ? s->arena : 0) : -1;
	if (a->cast[0][0] == (int)s->chr[0] && a->cast[0][1] == (int)s->pal[0] &&
	    a->cast[1][0] == (int)s->chr[1] && a->cast[1][1] == (int)s->pal[1] &&
	    a->cast_stage == stage)
		return 0;
	a->images = a->effect_images;
	a->pixels = a->effect_pixels;
	a->rows = a->pack_rows;
	a->spans = a->pack_spans;
	a->stage_pixels = 0;
	if (stage >= 0) {
		struct need n = {0, 0, 0, 0};
		uint32_t start = a->pixels;
		expand_stage(a, (unsigned)stage, 1, &n);
		a->stage_pixels = a->pixels - start;
	}
	a->cast_stage = stage;
	for (int i = 0; i < 2; i++) {
		const struct character *c = &a->chr[s->chr[i] < a->characters ? s->chr[i] : 0];
		unsigned palette = c->palette[s->pal[i] ? 1 : 0];
		uint32_t start = a->pixels;
		memset(a->look[i], 0xff, sizeof(a->look[i]));
		for (unsigned k = 0; k < c->steps; k++) {
			unsigned f = a->step[c->first_step + k].frame;
			if (a->look[i][f] < 0)
				a->look[i][f] = (int16_t)image(a, f, palette);
		}
		a->cast[i][0] = (int)(c - a->chr);
		a->cast[i][1] = s->pal[i] ? 1 : 0;
		a->cast_pixels[i] = a->pixels - start;
	}
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
