/* Native libretro callback harness: malformed content, deterministic state,
 * output formats, audio and a complete automated scene cycle, plus the rules
 * on exact ticks: frame data, hit/block/hitstop/hitstun timing, pushback and
 * corner transfer, guard heights, trades, motion and charge commands, the
 * input buffer and prejump, special and chain cancels, Power Wave flight,
 * hits, guard and clash, Burn Knuckle motion, Rising Tackle height, the
 * arcade ladder (select, opponents, loss, clear, Auto rotation, replay),
 * state round-trip and rejection, and the AI's use of every move, with
 * expected numbers taken from assets/fighter/terry.json. It also writes
 * preview PPMs of the select, Power Wave (boxes on), result and clear
 * screens. test-render.c adds the pack decoder, box and pixel-exact
 * renderer checks. ASan/UBSan run this without a window or device. */
#include "fighter.h"
#include "libretro.h"
#include "test-render.h"
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t keys[2], image_hash, sound_hash;
static unsigned video_count, audio_count, formats;
static bool use32, reject_formats;
static unsigned benchmark_complete;
static void log_line(enum retro_log_level level, const char *fmt, ...)
{
	(void)level;
	if (strstr(fmt, "benchmark complete:"))
		benchmark_complete++;
}
static uint16_t last_frame[FW * FH];
static const char *players = "cpu";
static bool environment(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_line;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		((struct retro_variable *)data)->value = players;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		formats++;
		return !reject_formats && *(enum retro_pixel_format *)data ==
					      (use32 ? RETRO_PIXEL_FORMAT_XRGB8888
						     : RETRO_PIXEL_FORMAT_RGB565);
	default:
		return false;
	}
}
static uint32_t hash(const void *data, size_t n)
{
	const uint8_t *p = data;
	uint32_t h = 2166136261;
	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}
static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
	assert(data && w == FW && h == FH && pitch == FW * (use32 ? 4u : 2u));
	image_hash = hash(data, pitch * h);
	video_count++;
	if (!use32)
		memcpy(last_frame, data, sizeof(last_frame));
}
static size_t audio(const int16_t *data, size_t n)
{
	assert(n == 800);
	sound_hash = hash(data, n * 4);
	audio_count++;
	return n;
}
static void poll_input(void) {}
static int16_t input(unsigned port, unsigned device, unsigned index, unsigned id)
{
	assert(port < 2 && device == RETRO_DEVICE_JOYPAD && !index && id < 16);
	return (keys[port] >> id) & 1;
}
static uint8_t *read_all(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	assert(f);
	assert(!fseek(f, 0, SEEK_END));
	long len = ftell(f);
	assert(len > 0);
	rewind(f);
	uint8_t *p = malloc((size_t)len);
	assert(p);
	assert(fread(p, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	*n = (size_t)len;
	return p;
}
static void save(void *buf, size_t n) { assert(retro_serialize(buf, n)); }
/* ---- rules on exact ticks; expected numbers come from terry.json ---- */
#define K(k) (1u << RETRO_DEVICE_ID_JOYPAD_##k)
static struct assets pack;
static unsigned terry;
enum { POWER_WAVE = MOVES, BURN_KNUCKLE, RISING_TACKLE };
static void duel(struct fight_state *s, int x0, int x1)
{
	fight_reset(s, 0);
	fight_start(s, terry, terry);
	s->stage = 1;
	s->p[0].x = x0;
	s->p[1].x = x1;
}
static void step2(struct fight_state *s, uint32_t k0, uint32_t k1)
{
	fight_step(s, &pack, k0, k1, 1);
}
/* n counts fight_step calls from 1; P0 presses `press` on call 1. */
static void run(struct fight_state *s, int n, uint32_t press, uint32_t hold0, uint32_t hold1)
{
	step2(s, (n == 1 ? press : 0) | hold0, hold1);
}
static int in_action(const struct fight_state *s, int i, int action)
{
	const struct anim *an = &pack.chr[s->chr[i]].action[action];
	unsigned k = fight_pose(s, &pack, i);
	return k >= an->first && k < an->first + an->count;
}
static int live_shots(const struct fight_state *s)
{
	int n = 0;
	for (int k = 0; k < MAX_PROJECTILES; k++)
		n += s->shot[k].kind != 0;
	return n;
}
static const struct shot *shot_of(const struct fight_state *s, int owner)
{
	for (int k = 0; k < MAX_PROJECTILES; k++)
		if (s->shot[k].kind && s->shot[k].owner == owner)
			return &s->shot[k];
	return NULL;
}
/* Keys of a motion whose button lands on call `last` (face 1 = right):
 * 236 is D, D, DF, DF, F+button on last-4 .. last; 214 likewise with back. */
static uint32_t motion(int m, int n, int last, int face, uint32_t button)
{
	uint32_t f = face > 0 ? K(RIGHT) : K(LEFT), b = face > 0 ? K(LEFT) : K(RIGHT);
	int t = n - (last - 4);
	if (t < 0 || t > 4)
		return 0;
	uint32_t dir = m == MOTION_236 ? (t < 2 ? K(DOWN) : t < 4 ? K(DOWN) | f : f)
				       : (t < 2 ? K(DOWN) : t < 4 ? K(DOWN) | b : b);
	return dir | (t == 4 ? button : 0);
}
static void frame_data(void)
{
	/* (startup, active, recovery, damage, hitstun, blockstun, hitstop,
	 * pushback, block pushback, guard) of each normal in terry.json. */
	static const int want[MOVES][10] = {
	    {3, 3, 6, 30, 12, 10, 6, 14, 18, GUARD_HIGH},
	    {6, 5, 13, 70, 18, 15, 10, 22, 26, GUARD_HIGH},
	    {5, 4, 8, 35, 14, 11, 6, 16, 20, GUARD_HIGH},
	    {10, 6, 17, 80, 20, 16, 10, 24, 28, GUARD_HIGH},
	    {2, 3, 5, 25, 11, 9, 6, 12, 16, GUARD_HIGH},
	    {6, 4, 12, 65, 18, 14, 10, 20, 24, GUARD_HIGH},
	    {4, 4, 8, 30, 13, 11, 6, 14, 18, GUARD_LOW},
	    {9, 4, 18, 75, 22, 16, 10, 24, 28, GUARD_LOW}};
	static const unsigned cancel[MOVES] = {
	    CANCEL_CHAIN | CANCEL_SPECIAL, CANCEL_SPECIAL, CANCEL_SPECIAL, 0,
	    CANCEL_CHAIN | CANCEL_SPECIAL, CANCEL_SPECIAL, CANCEL_CHAIN | CANCEL_SPECIAL, 0};
	const struct character *c = &pack.chr[terry];
	assert(!strcmp(c->name, "TERRY") && c->moves == MOVES + 3 && c->projectiles == 1);
	for (int k = 0; k < MOVES; k++) {
		const struct move *m = &c->move[k];
		const int *w = want[k];
		assert(m->startup == w[0] && m->active == w[1] && m->recovery == w[2] &&
		       m->anim.total == w[0] + w[1] + w[2] && m->hit.damage == w[3] &&
		       m->hit.chip == 0 && m->hit.hitstun == w[4] && m->hit.blockstun == w[5] &&
		       m->hit.hitstop == w[6] && m->hit.pushback == w[7] &&
		       m->hit.block_pushback == w[8] && m->hit.guard == w[9] && m->groups == 1 &&
		       !m->special && m->cancel == cancel[k] && !m->spawn);
	}
	/* Power Wave: 41 ticks, no hitbox of its own, launches on elapsed 17. */
	const struct move *pw = &c->move[POWER_WAVE], *bk = &c->move[BURN_KNUCKLE],
			  *rt = &c->move[RISING_TACKLE];
	assert(pw->special && pw->motion == MOTION_236 && pw->buttons == (BUTTON_LP | BUTTON_HP) &&
	       pw->anim.total == 41 && !pw->groups && pw->spawn == 1 && !pw->cancel);
	assert(bk->special && bk->motion == MOTION_214 && bk->startup == 13 && bk->active == 12 &&
	       bk->recovery == 15 && bk->hit.damage == 90 && bk->hit.chip == 18 &&
	       bk->hit.hitstun == 22 && bk->hit.blockstun == 18 && bk->groups == 1);
	assert(rt->special && rt->motion == CHARGE_28 && rt->startup == 6 && rt->active == 18 &&
	       rt->recovery == 33 && rt->hit.damage == 100 && rt->hit.chip == 20);
	const struct projectile *wave = &c->projectile[0];
	assert(wave->speed == 4 && wave->lifetime == 120 && wave->x == 60 && wave->y == 0 &&
	       wave->hit.damage == 70 && wave->hit.chip == 14 && wave->hit.hitstun == 20 &&
	       wave->hit.blockstun == 16 && wave->hit.hitstop == 8 && wave->hit.pushback == 20 &&
	       wave->hit.block_pushback == 24 && wave->blend == OPAQUE &&
	       wave->anim.count == 6 && wave->anim.total == 18);
}
/* Stand LP connects on its first active tick (call 4), both freeze for 6
 * ticks (5..10), the attacker recovers on call 4 + 6 + (3 + 6) = 19 and the
 * defender on 4 + 6 + 12 = 22: +3, as `pack.py data` prints. */
static void hit_timing(void)
{
	static const int slide[7] = {264, 267, 269, 271, 272, 273, 274};
	struct fight_state s;
	duel(&s, 200, 260);
	for (int n = 1; n <= 24; n++) {
		run(&s, n, K(Y), 0, 0);
		const struct fighter *a = &s.p[0], *d = &s.p[1];
		assert(d->hp == (n < 4 ? 1000 : 970));
		assert((a->move == STAND_LP + 1) == (n < 19));
		assert(a->freeze == (n >= 4 && n <= 10 ? 10 - n : 0) && d->freeze == a->freeze);
		assert(d->stun == (n < 4 ? 0 : n <= 10 ? 12 : n < 22 ? 22 - n : 0));
		assert(a->hit == (n >= 4 && n < 19));
		if (n >= 4 && n <= 10)
			assert(a->elapsed == 3);
		if (n >= 4 && n < 22)
			assert(in_action(&s, 1, HURT) && !d->block);
		assert(d->x == (n <= 10 ? 260 : n <= 17 ? slide[n - 11] : 274));
		assert(a->x == 200 && s.guards == 0 && s.hits == (n >= 4));
	}
}
/* Blocked stand LP: stance from call 2, blockstun 10 ends on call 20 (+1),
 * block pushback 18 over calls 11..18. */
static void block_timing(void)
{
	static const int slide[8] = {267, 271, 274, 276, 277, 278, 279, 280};
	struct fight_state s;
	duel(&s, 200, 260);
	for (int n = 1; n <= 19; n++) {
		run(&s, n, K(Y), 0, K(RIGHT));
		const struct fighter *d = &s.p[1];
		assert(d->hp == 1000 && s.guards == (n >= 4) && s.hits == (n >= 4));
		assert(d->block == (n == 1 ? 0 : n < 4 ? 2 : 1));
		assert(d->stun == (n < 4 ? 0 : n <= 10 ? 10 : 20 - n));
		assert(d->x == (n == 1 || n <= 10 ? 262 : n <= 18 ? slide[n - 11] : 280));
		if (n >= 2)
			assert(in_action(&s, 1, GUARD));
		assert((s.p[0].move != 0) == (n < 19));
	}
	run(&s, 20, 0, 0, K(RIGHT));
	assert(!s.p[1].stun && !s.p[1].block && s.p[1].x == 282);
}
/* In the corner the defender cannot slide: the attacker moves back the
 * same 14 pixels on the same ticks. */
static void corner(void)
{
	static const int slide[7] = {366, 363, 361, 359, 358, 357, 356};
	struct fight_state s;
	duel(&s, 370, WALL_RIGHT);
	for (int n = 1; n <= 20; n++) {
		run(&s, n, K(Y), 0, 0);
		assert(s.p[1].x == WALL_RIGHT && s.p[1].hp == (n < 4 ? 1000 : 970));
		assert(s.p[0].x == (n <= 10 ? 370 : n <= 17 ? slide[n - 11] : 356));
	}
	/* Mirrored: P1 attacks P0 pinned at the left wall. */
	duel(&s, WALL_LEFT, WALL_LEFT + 60);
	for (int n = 1; n <= 20; n++)
		step2(&s, 0, n == 1 ? K(Y) : 0);
	assert(s.p[0].x == WALL_LEFT && s.p[1].x == WALL_LEFT + 60 + 14 && s.p[0].hp == 970);
}
/* High, low and overhead against standing and crouching guard. */
static void guard_types(void)
{
	struct fight_state s;
	/* Crouch LK (low, startup 4) hits a standing guard on call 5... */
	duel(&s, 200, 260);
	for (int n = 1; n <= 5; n++)
		run(&s, n, K(B), K(DOWN), K(RIGHT));
	assert(s.p[0].move == CROUCH_LK + 1 && s.p[1].hp == 970 && s.p[1].stun == 13 &&
	       !s.guards && in_action(&s, 1, HURT));
	/* ...and is blocked low on call 5, blockstun 11, crouching guard. */
	duel(&s, 200, 260);
	for (int n = 1; n <= 4; n++) {
		run(&s, n, K(B), K(DOWN), K(DOWN) | K(RIGHT));
		assert(s.p[1].crouch && s.p[1].hp == 1000 && !s.p[1].stun);
	}
	run(&s, 5, K(B), K(DOWN), K(DOWN) | K(RIGHT));
	assert(s.guards == 1 && s.p[1].stun == 11 && s.p[1].block == 1 &&
	       in_action(&s, 1, CROUCH_GUARD) && s.p[1].x == 260);
	/* Crouch LP (high, startup 2) is blocked crouching on call 3. */
	duel(&s, 200, 250);
	for (int n = 1; n <= 3; n++)
		run(&s, n, K(Y), K(DOWN), K(DOWN) | K(RIGHT));
	assert(s.guards == 1 && s.p[1].stun == 9 && s.p[1].hp == 1000);
	/* As an overhead it beats the crouching guard and is blocked standing. */
	pack.chr[terry].move[CROUCH_LP].hit.guard = GUARD_OVERHEAD;
	duel(&s, 200, 250);
	for (int n = 1; n <= 3; n++)
		run(&s, n, K(Y), K(DOWN), K(DOWN) | K(RIGHT));
	assert(!s.guards && s.p[1].hp == 975 && s.p[1].stun == 11 &&
	       in_action(&s, 1, CROUCH_HURT));
	duel(&s, 200, 250);
	for (int n = 1; n <= 3; n++)
		run(&s, n, K(Y), K(DOWN), K(RIGHT));
	assert(s.guards == 1 && s.p[1].hp == 1000 && !s.p[1].crouch);
	pack.chr[terry].move[CROUCH_LP].hit.guard = GUARD_HIGH;
	/* Stand LP's hitbox passes over a crouching body: whiff, recover on 13. */
	duel(&s, 200, 260);
	for (int n = 1; n <= 13; n++) {
		run(&s, n, K(Y), 0, K(DOWN));
		assert(!s.hits && s.p[1].hp == 1000 && (s.p[0].move != 0) == (n < 13));
	}
	/* Without a back input there is no guard: crouching hit by crouch LK. */
	duel(&s, 200, 260);
	for (int n = 1; n <= 5; n++)
		run(&s, n, K(B), K(DOWN), K(DOWN));
	assert(s.p[1].hp == 970 && in_action(&s, 1, CROUCH_HURT));
}
/* Simultaneous stand LPs trade: both hit on call 4, same hitstop. */
static void trade(void)
{
	struct fight_state s;
	duel(&s, 200, 260);
	for (int n = 1; n <= 4; n++)
		step2(&s, n == 1 ? K(Y) : 0, n == 1 ? K(Y) : 0);
	assert(s.hits == 2 && s.p[0].hp == 970 && s.p[1].hp == 970 && s.p[0].stun == 12 &&
	       s.p[1].stun == 12 && s.p[0].freeze == 6 && s.p[1].freeze == 6 &&
	       !s.p[0].move && !s.p[1].move);
}
/* Motions against hand-written histories: oldest direction first. */
static void set_history(struct fighter *p, const char *dirs)
{
	memset(p->history, 0, sizeof(p->history));
	for (; *dirs; dirs++) {
		/* Numpad digit for a right-facing fighter to raw bits. */
		int n = *dirs - '0';
		unsigned d = (n >= 7 ? 1u : 0) | (n <= 3 ? 2u : 0) | (n % 3 == 1 ? 4u : 0) |
			     (n % 3 == 0 ? 8u : 0);
		for (int w = HISTORY / 8 - 1; w > 0; w--)
			p->history[w] = p->history[w] << 4 | p->history[w - 1] >> 28;
		p->history[0] = p->history[0] << 4 | d;
	}
}
static void commands(void)
{
	struct fighter p;
	memset(&p, 0, sizeof(p));
	p.face = 1;
	/* 236 with the 2 at most 14 ticks before the newest direction. */
	set_history(&p, "236");
	assert(fight_motion(&p, MOTION_236) && !fight_motion(&p, MOTION_214) &&
	       !fight_motion(&p, MOTION_623));
	set_history(&p, "2555555555555" "36");
	assert(fight_motion(&p, MOTION_236));
	set_history(&p, "25555555555555" "36");
	assert(!fight_motion(&p, MOTION_236));
	/* In-between directions are allowed; order is not optional. */
	set_history(&p, "21236");
	assert(fight_motion(&p, MOTION_236));
	set_history(&p, "326");
	assert(!fight_motion(&p, MOTION_236));
	/* Directions follow facing: 214 for a left-facing fighter is D, DR, R. */
	set_history(&p, "214");
	assert(fight_motion(&p, MOTION_214));
	p.face = -1;
	assert(fight_motion(&p, MOTION_236) && !fight_motion(&p, MOTION_214));
	p.face = 1;
	/* 623 within 20 ticks; it is not a 236. */
	set_history(&p, "6222223");
	assert(fight_motion(&p, MOTION_623) && !fight_motion(&p, MOTION_236));
	set_history(&p, "6555555555555555555" "23");
	assert(!fight_motion(&p, MOTION_623));
	set_history(&p, "655555555555555555" "23");
	assert(fight_motion(&p, MOTION_623));
	/* Charges: back held 30 (byte 0) or released within the grace (byte 1). */
	set_history(&p, "6");
	p.charge = CHARGE_TICKS - 1;
	assert(!fight_motion(&p, CHARGE_46));
	p.charge = CHARGE_TICKS;
	assert(fight_motion(&p, CHARGE_46) && !fight_motion(&p, CHARGE_28));
	p.charge = 1u << 8;
	assert(fight_motion(&p, CHARGE_46));
	/* The release direction must be among the last 4 directions. */
	set_history(&p, "65555");
	assert(!fight_motion(&p, CHARGE_46));
	set_history(&p, "6555");
	assert(fight_motion(&p, CHARGE_46));
	set_history(&p, "9");
	p.charge = (uint32_t)CHARGE_TICKS << 16;
	assert(fight_motion(&p, CHARGE_28) && !fight_motion(&p, CHARGE_46));
	set_history(&p, "7");
	assert(fight_motion(&p, CHARGE_28));
}
/* Presses in the last BUFFER_TICKS (4) recovery ticks come out on the
 * first free tick; earlier ones are dropped. A whiffed stand LP pressed on
 * call 1 ends on call 13. */
static void buffer(void)
{
	struct fight_state s;
	for (int press = 7; press <= 10; press++) {
		duel(&s, 100, 400);
		for (int n = 1; n <= 16; n++) {
			step2(&s, n == 1 ? K(Y) : n == press ? K(X) : 0, 0);
			if (n < 13)
				assert(s.p[0].move == STAND_LP + 1);
			else if (press >= 9)
				assert(s.p[0].move == STAND_HP + 1 && s.p[0].elapsed == n - 13);
			else
				assert(!s.p[0].move && !s.p[0].buffer);
		}
	}
	/* Up starts a 3-tick prejump; airborne from call 4. A press during it
	 * replaces the jump. */
	duel(&s, 100, 400);
	for (int n = 1; n <= 5; n++) {
		step2(&s, K(UP), 0);
		assert(s.p[0].squat == (n < 4 ? 4 - n : 0) && s.p[0].y == (n < 5 ? 0 : -12));
		assert(s.p[0].vy == (n < 4 ? 0 : n == 4 ? -48 : -45));
	}
	duel(&s, 100, 400);
	step2(&s, K(UP), 0);
	step2(&s, K(UP) | K(Y), 0);
	assert(s.p[0].move == STAND_LP + 1 && !s.p[0].squat);
	for (int n = 3; n <= 20; n++)
		step2(&s, 0, 0);
	assert(!s.p[0].y && !s.p[0].vy && !s.p[0].move);
}
/* Cancels. Stand LP hits on call 4 (hitstop 5..10); 236+Y entered during
 * the hitstop (button on call 9) cancels into Power Wave on call 11, which
 * launches on its elapsed 17 = call 28 at 200 + 60 = 260, already inside
 * the pushed-back P1 (274, lowest hurtbox from 274 - 31 + 6... = 249). */
static void cancels(void)
{
	struct fight_state s;
	duel(&s, 200, 260);
	for (int n = 1; n <= 30; n++) {
		uint32_t k = n == 1 ? K(Y) : motion(MOTION_236, n, 9, 1, K(Y));
		step2(&s, k, 0);
		if (n >= 4 && n <= 10)
			assert(s.p[0].move == STAND_LP + 1 && s.p[0].freeze == 10 - n);
		if (n >= 9 && n <= 10)
			assert(s.p[0].buffer == POWER_WAVE + 1 && s.p[0].age == 0);
		if (n >= 11)
			assert(s.p[0].move == POWER_WAVE + 1 && s.p[0].elapsed == n - 11);
		if (n < 28)
			assert(!live_shots(&s) && s.p[1].hp == (n < 4 ? 1000 : 970));
	}
	assert(s.specials == 1 && s.hits == 2 && s.p[1].hp == 900 && !live_shots(&s));
	/* No cancel without a hit: the same input after a whiff is dropped. */
	duel(&s, 100, 400);
	for (int n = 1; n <= 14; n++) {
		step2(&s, n == 1 ? K(Y) : motion(MOTION_236, n, 6, 1, K(Y)), 0);
		assert(s.p[0].move == (n < 13 ? STAND_LP + 1 : 0));
	}
	/* Stand HK has no cancel: hit on call 11, hitstop 12..21, the special
	 * pressed on call 16 is dropped after 4 unfrozen ticks (call 26) and
	 * HK ends on call 11 + 10 + 23 = 44. */
	duel(&s, 200, 260);
	for (int n = 1; n <= 44; n++) {
		step2(&s, n == 1 ? K(A) : motion(MOTION_236, n, 16, 1, K(Y)), 0);
		assert(s.p[0].move == (n < 44 ? STAND_HK + 1 : 0));
		assert((s.p[0].buffer != 0) == (n >= 16 && n < 26));
	}
	assert(s.p[1].hp == 920 && !s.specials);
	/* Chain: crouch LP hits on call 3 (hitstop 4..9); a second crouch LP
	 * pressed on call 5 starts on call 10. */
	duel(&s, 200, 250);
	for (int n = 1; n <= 10; n++)
		step2(&s, K(DOWN) | (n == 1 || n == 5 ? K(Y) : 0), 0);
	assert(s.p[0].move == CROUCH_LP + 1 && !s.p[0].elapsed && !s.p[0].hit &&
	       s.p[1].hp == 975);
	for (int n = 11; n <= 12; n++)
		step2(&s, K(DOWN), 0);
	assert(s.p[1].hp == 950 && s.hits == 2);
}
/* Power Wave thrown on call 5 at 100 launches on call 22 at 160 and moves
 * 4 px a tick. P1 at 300 faces left: its lowest idle hurtbox [-31, -51,
 * 56, 51] spans 275..330 and the wave's box [-40, -50, 46, 50] reaches
 * x + 6, so it first overlaps at x = 272, call 22 + 28 = 50. Guarding
 * (back held from call 40, when the wave is in range), the guard1 box
 * [-33, -49, 60, 49] reaches 273 and the wave connects at x = 268, call 49. */
static void projectile(void)
{
	struct fight_state s;
	for (int guard = 0; guard < 2; guard++) {
		int hit = guard ? 49 : 50;
		duel(&s, 100, 300);
		for (int n = 1; n <= 62; n++) {
			/* P1 holds back once the wave is within guard range. */
			step2(&s, motion(MOTION_236, n, 5, 1, K(Y)), guard && n >= 40 ? K(RIGHT) : 0);
			const struct shot *sh = shot_of(&s, 0);
			assert(s.p[0].x == 100);
			if (n >= 5 && n < 46)
				assert(s.p[0].move == POWER_WAVE + 1 && s.p[0].elapsed == n - 5);
			if (n < 22)
				assert(!sh);
			else if (n < hit)
				assert(sh && sh->x == 160 + 4 * (n - 22) && sh->age == n - 22 &&
				       sh->y == 0 && sh->face == 1 && s.p[1].hp == 1000);
			if (n == hit) {
				assert(!sh && s.hits == 1 && s.guards == (unsigned)guard);
				assert(s.p[1].hp == (guard ? 986 : 930));
				assert(s.p[1].stun == (guard ? 16 : 20) && s.p[1].freeze == 8);
				assert(s.p[0].freeze == 0 && s.p[1].block == guard);
			}
		}
		/* Pushback 20 / 24 slides P1 after the hitstop; P0 stays. */
		assert(s.p[1].x > 300 && s.p[0].x == 100);
	}
	/* One projectile at a time: a second 236+Y is a stand LP. */
	duel(&s, 100, 400);
	for (int n = 1; n <= 46; n++)
		step2(&s, motion(MOTION_236, n, 5, 1, K(Y)) | motion(MOTION_236, n, 46, 1, K(Y)),
		      0);
	assert(shot_of(&s, 0) && s.p[0].move == STAND_LP + 1 && s.specials == 1);
	/* Projectiles vanish 64 px off screen (x > 544) or at lifetime 120. */
	duel(&s, 100, 150);
	s.shot[0] = (struct shot){1, 0, 540, 0, 1, 10};
	s.shot[1] = (struct shot){1, 1, 300, 0, -1, 118};
	step2(&s, 0, 0);
	assert(s.shot[0].x == 544 && s.shot[1].age == 119);
	step2(&s, 0, 0);
	assert(!live_shots(&s));
	/* Clash: P1 at 380 throws on the same calls. Its box mirrors to
	 * x - 6 .. x + 40; 166 + 4k > 314 - 4k first at k = 19, call 41. */
	duel(&s, 100, 380);
	for (int n = 1; n <= 41; n++) {
		step2(&s, motion(MOTION_236, n, 5, 1, K(Y)), motion(MOTION_236, n, 5, -1, K(Y)));
		assert(live_shots(&s) == (n >= 22 && n < 41 ? 2 : 0));
	}
	assert(s.clashes == 1 && s.p[0].hp == 1000 && s.p[1].hp == 1000 && !s.hits);
}
/* Burn Knuckle (214+Y on call 5) moves 4 px for 2 ticks from elapsed 11,
 * 6 px for 9, 5 px for 3 and 2 px for 4: 100 + 8 + 54 + 15 + 8 = 185,
 * ending on call 45. Against a body it hits on elapsed 13. */
static void burn_knuckle(void)
{
	static const int x[40] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
				  100, 104, 108, 114, 120, 126, 132, 138, 144, 150,
				  156, 162, 167, 172, 177, 179, 181, 183, 185, 185,
				  185, 185, 185, 185, 185, 185, 185, 185, 185, 185};
	struct fight_state s;
	duel(&s, 100, 420);
	for (int n = 1; n <= 45; n++) {
		step2(&s, motion(MOTION_214, n, 5, 1, K(Y)), 0);
		if (n >= 5 && n < 45)
			assert(s.p[0].move == BURN_KNUCKLE + 1 && s.p[0].x == x[n - 5]);
	}
	assert(!s.p[0].move && s.p[0].x == 185);
	/* P1 at 200 facing left: hurtbox from 200 - 30 = 170; the fist box
	 * [36, -84, 28, 24] at x + 36 .. x + 64 reaches it once x + 64 > 170. */
	duel(&s, 100, 200);
	for (int n = 1; n <= 18; n++)
		step2(&s, motion(MOTION_214, n, 5, 1, K(Y)), 0);
	assert(s.p[1].hp == 910 && s.p[1].stun == 22 && s.p[0].freeze == 10 &&
	       s.p[0].elapsed == 13 && s.p[0].x == 114);
}
/* Rising Tackle: down held for calls 1..30 charges 30 ticks, up+Y on call
 * 31 starts it. Heights after elapsed 5, 8, 23, 26, 29, 35, 42 follow the
 * step motion; it ends on call 31 + 57 = 88. 29 ticks give a stand LP. */
static void rising_tackle(void)
{
	static const int at[7] = {5, 8, 23, 26, 29, 35, 42}, y[7] = {-12, -30, -75, -75, -72, -42, 0};
	struct fight_state s;
	duel(&s, 100, 400);
	for (int n = 1; n <= 88; n++) {
		step2(&s, n <= 30 ? K(DOWN) : n == 31 ? K(UP) | K(Y) : 0, 0);
		if (n >= 31 && n < 88) {
			assert(s.p[0].move == RISING_TACKLE + 1 && s.p[0].elapsed == n - 31);
			for (int k = 0; k < 7; k++)
				if (n - 31 == at[k])
					assert(s.p[0].y == y[k]);
		}
	}
	assert(!s.p[0].move && !s.p[0].y && !s.p[0].squat);
	duel(&s, 100, 400);
	for (int n = 2; n <= 30; n++)
		step2(&s, K(DOWN), 0);
	step2(&s, K(UP) | K(Y), 0);
	assert(s.p[0].move == STAND_LP + 1);
	/* Anti-air range: P1 at 260 is hit on elapsed 6 (call 37) at y -18. */
	duel(&s, 200, 260);
	for (int n = 1; n <= 37; n++)
		step2(&s, n <= 30 ? K(DOWN) : n == 31 ? K(UP) | K(Y) : 0, 0);
	assert(s.p[1].hp == 900 && s.p[0].y == -18 && s.p[0].freeze == 10);
}
/* The arcade ladder on a four-character roster made of Terry copies:
 * select, random unbeaten opponents, a loss, a clear and save/replay. */
static void win_round(struct fight_state *s, int winner)
{
	/* Stand LP into a 10 hp loser: KO on call 4, round over after 180. */
	s->p[0].x = 200;
	s->p[1].x = 260;
	s->p[1 - winner].hp = 10;
	for (int n = 1; n <= 184; n++)
		step2(s, winner == 0 && n == 1 ? K(Y) : 0, winner == 1 && n == 1 ? K(Y) : 0);
}
static void ladder(void)
{
	struct character *saved = malloc(sizeof(struct character) * 4);
	assert(saved);
	memcpy(saved, pack.chr, sizeof(struct character) * 4);
	unsigned characters = pack.characters;
	for (int k = 1; k < 4; k++) {
		pack.chr[k] = pack.chr[terry];
		pack.chr[k].name[0] = (char)('A' + k);
	}
	pack.characters = 4;
	struct fight_state s, replay;
	uint32_t w[STATE_WORDS], v[STATE_WORDS];
	fight_reset(&s, 0);
	step2(&s, K(RIGHT), 0);
	step2(&s, 0, 0);
	step2(&s, K(DOWN), 0); /* row 2 does not exist: stays */
	assert(s.mode == MODE_SELECT && s.chr[0] == 1);
	step2(&s, K(B), 0);
	assert(s.mode == MODE_FIGHT && s.stage == 1 && s.chr[0] == 1 && s.chr[1] != 1 &&
	       !s.pal[1] && !s.beaten);
	fight_encode(&s, w);
	assert(fight_decode(&replay, &pack, w));
	uint32_t order = 0;
	for (unsigned stage = 1; stage <= 3; stage++) {
		assert(s.mode == MODE_FIGHT && s.stage == stage && s.chr[1] != 1 &&
		       !((s.beaten >> s.chr[1]) & 1));
		order = order * 4 + s.chr[1];
		unsigned beaten = s.beaten | 1u << s.chr[1];
		win_round(&s, 0);
		assert(s.beaten == beaten && s.round == stage);
	}
	assert(s.mode == MODE_CLEAR && s.beaten == 0xd);
	/* The same inputs from the saved state meet the same opponents. */
	for (unsigned stage = 1; stage <= 3; stage++)
		win_round(&replay, 0);
	fight_encode(&s, w);
	fight_encode(&replay, v);
	assert(!memcmp(w, v, sizeof(w)));
	/* The clear screen returns to select after 181 ticks. */
	for (int n = 1; n <= 181; n++) {
		assert(s.mode == MODE_CLEAR);
		step2(&s, 0, 0);
	}
	assert(s.mode == MODE_SELECT && s.run == 1 && !s.beaten && !s.stage);
	/* A loss on stage 1 shows the result screen, a button after 20 ticks
	 * returns to select. */
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_FIGHT && s.stage == 1);
	win_round(&s, 1);
	assert(s.mode == MODE_RESULT && s.timer == 0);
	for (int n = 1; n <= 21; n++)
		step2(&s, n == 21 ? K(A) : 0, 0);
	assert(s.mode == MODE_SELECT && s.run == 2);
	/* Auto skips select and rotates P1 through the roster per run. */
	fight_reset(&s, 1);
	unsigned seen = 0;
	for (int t = 0; t < 60000 && seen != 15; t++) {
		fight_step(&s, &pack, 0, 0, 0);
		assert(s.mode != MODE_SELECT);
		if (s.mode == MODE_FIGHT) {
			assert(s.chr[0] == s.run % 4);
			seen |= 1u << s.chr[0];
		}
	}
	assert(seen == 15 && s.run >= 3);
	memcpy(pack.chr, saved, sizeof(struct character) * 4);
	pack.characters = characters;
	free(saved);
	/* One character: a mirror match with the alternate palette, one stage. */
	if (pack.characters == 1) {
		fight_reset(&s, 0);
		step2(&s, K(Y), 0);
		assert(s.mode == MODE_FIGHT && s.chr[1] == 0 && s.pal[1] == 1);
		win_round(&s, 0);
		assert(s.mode == MODE_CLEAR);
	}
}
/* Mid-hitstop state with a live projectile, a buffered special and a
 * charge round-trips, replays identically and rejects impossible values
 * without touching the destination. */
static void state_words(void)
{
	struct fight_state s, t, keep;
	uint32_t w[STATE_WORDS], v[STATE_WORDS], bad[STATE_WORDS];
	duel(&s, 200, 260);
	for (int n = 1; n <= 30; n++)
		step2(&s, n == 1 ? K(Y) : motion(MOTION_236, n, 9, 1, K(Y)), K(DOWN));
	duel(&s, 100, 300);
	for (int n = 1; n <= 30; n++)
		step2(&s, motion(MOTION_236, n, 5, 1, K(Y)), K(DOWN));
	assert(shot_of(&s, 0) && s.p[1].charge);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	fight_encode(&t, v);
	assert(!memcmp(w, v, sizeof(w)));
	for (int n = 31; n <= 140; n++) {
		uint32_t press = n % 5 ? 0 : K(X);
		step2(&s, press | motion(MOTION_214, n, 70, 1, K(Y)), K(LEFT));
		step2(&t, press | motion(MOTION_214, n, 70, 1, K(Y)), K(LEFT));
	}
	fight_encode(&s, w);
	fight_encode(&t, v);
	assert(!memcmp(w, v, sizeof(w)));
	/* A state with everything in flight: P0 mid Rising Tackle in the air
	 * with a buffered press, P1's Power Wave alive. */
	duel(&s, 100, 400);
	for (int n = 1; n <= 45; n++)
		step2(&s, n <= 30 ? K(DOWN) : n == 31 ? K(UP) | K(Y) : n == 45 ? K(X) : 0,
		      motion(MOTION_236, n, 20, -1, K(Y)));
	assert(s.p[0].move == RISING_TACKLE + 1 && s.p[0].y < 0 && s.p[0].buffer &&
	       shot_of(&s, 1));
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	enum { W_X, W_Y, W_VY, W_HP, W_FACE, W_WALK, W_BLOCK, W_CROUCH, W_MOVE, W_ELAPSED,
	       W_HIT, W_STUN, W_FREEZE, W_SLIDE, W_KEYS, W_CORNER, W_SQUAT, W_BUFFER, W_AGE,
	       W_PLAN, W_PLAN_TICK, W_CHARGE };
	enum { G_MODE = 8, G_TIMER, G_BEATEN, G_STAGE, G_RUN, G_WIN0, G_WIN1, G_CHR0, G_CHR1,
	       G_PAL0, G_PAL1 };
	const int P = 19, S = 19 + 2 * 26;
	int shot = S + 6 * (int)(shot_of(&s, 1) - s.shot);
	static const struct {
		int at, who, field;
		uint32_t value;
	} cases[] = {
	    {0, 1, W_MOVE, MOVES + 4},	   {0, 1, W_MOVE, 1},	       {0, 1, W_FREEZE, MAX_HITSTOP + 1},
	    {0, 1, W_STUN, MAX_STUN + 1},  {0, 1, W_SLIDE, MAX_PUSH + 1}, {0, 1, W_SLIDE, (uint32_t)-MAX_PUSH - 1},
	    {0, 1, W_KEYS, 0x10000},	   {0, 1, W_CROUCH, 2},	       {0, 1, W_BLOCK, 3},
	    {0, 0, W_ELAPSED, 100},	   {0, 0, W_HIT, 2},	       {0, 0, W_STUN, 5},
	    {0, 0, W_BLOCK, 1},	   {0, 1, W_HIT, 1},	       {0, 1, W_ELAPSED, 256},
	    {0, 0, W_X, WALL_LEFT - 1},	   {0, 0, W_FACE, 0},	       {0, 1, W_HP, 1001},
	    {0, 0, W_WALK, 2},		   {0, 1, W_VY, 61},	       {0, 0, W_Y, (uint32_t)-30},
	    {0, 0, W_VY, 3},		   {0, 1, W_CORNER, 1},	       {0, 1, W_SQUAT, JUMP_SQUAT + 1},
	    {0, 0, W_SQUAT, 1},	   {0, 0, W_BUFFER, MOVES + 4}, {0, 0, W_AGE, BUFFER_TICKS + 1},
	    {0, 1, W_AGE, 1},		   {0, 1, W_PLAN, STAND_HK + 1}, {0, 1, W_PLAN, MOVES + 4},
	    {0, 1, W_PLAN_TICK, 1},	   {0, 1, W_CHARGE, (CHARGE_GRACE + 1) << 8},
	    {0, 1, W_CHARGE, 1 | 1 << 8},
	    /* Globals. */
	    {1, 0, G_MODE, MODES},	   {1, 0, G_TIMER, 256},	       {1, 0, G_BEATEN, 1},
	    {1, 0, G_BEATEN, 2},	   {1, 0, G_STAGE, 2},	       {1, 0, G_WIN0, ROUNDS_TO_WIN},
	    {1, 0, G_WIN1, ROUNDS_TO_WIN}, {1, 0, G_CHR0, 1},	       {1, 0, G_CHR1, 1},
	    {1, 0, G_PAL0, 1},	   {1, 0, G_PAL1, 0},	       {1, 0, G_PAL1, 2},
	    {1, 0, 3, 2},		   {1, 0, 4, 4},		       {1, 0, 5, 2},
	    {1, 0, 6, 2},
	    /* Projectile words. */
	    {2, 0, 0, 2},		   {2, 0, 1, 2},
	    {2, 0, 4, 0},		   {2, 0, 5, 120},	       {2, 0, 5, (uint32_t)-1},
	    {2, 0, 2, FW + 65},	   {2, 0, 0, 0},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		memcpy(bad, w, sizeof(w));
		int at = cases[i].at == 0 ? P + cases[i].who * 26 + cases[i].field
		       : cases[i].at == 1 ? cases[i].field
					  : shot + cases[i].field;
		bad[at] = cases[i].value;
		keep = t;
		if (fight_decode(&t, &pack, bad)) {
			fprintf(stderr, "bad state case %zu accepted\n", i);
			abort();
		}
		assert(!memcmp(&keep, &t, sizeof(t)));
	}
	/* A copy of the live projectile in a free slot gives P1 two. */
	memcpy(bad, w, sizeof(w));
	int free_slot = S;
	while (bad[free_slot])
		free_slot += 6;
	memcpy(bad + free_slot, w + shot, 6 * sizeof(uint32_t));
	assert(!fight_decode(&t, &pack, bad));
	/* Projectiles outside a fight and nonzero reserved words. */
	memcpy(bad, w, sizeof(w));
	bad[G_MODE] = MODE_RESULT;
	assert(!fight_decode(&t, &pack, bad));
	memcpy(bad, w, sizeof(w));
	bad[STATE_WORDS - 1] = 1;
	assert(!fight_decode(&t, &pack, bad));
	/* Crouching in the air. */
	memcpy(bad, w, sizeof(w));
	bad[P + 26 + W_CROUCH] = 1;
	bad[P + 26 + W_Y] = (uint32_t)-10;
	assert(!fight_decode(&t, &pack, bad));
}
/* The Auto pack's AI uses every move of every character (all normals and
 * specials), throws projectiles, guards at both heights, and finishes
 * ladder runs. Contacts are counted on the tick a fighter enters hitstop
 * with fresh stun. */
static void ai_coverage(void)
{
	struct fight_state s;
	unsigned hits = 0, blocks = 0, crouch_blocks = 0, crouch_hits = 0, shots = 0;
	uint32_t started[MAX_CHARACTERS] = {0};
	fight_reset(&s, 1);
	for (int t = 0; t < 30000 + 15000 * (int)pack.characters; t++) {
		struct fight_state before = s;
		fight_step(&s, &pack, 0, 0, 0);
		if (s.mode != MODE_FIGHT || before.mode != MODE_FIGHT)
			continue;
		for (int k = 0; k < MAX_PROJECTILES; k++)
			shots += s.shot[k].kind && !s.shot[k].age;
		for (int i = 0; i < 2; i++) {
			const struct fighter *p = &s.p[i];
			if (p->move && (p->move != before.p[i].move || p->elapsed < before.p[i].elapsed))
				started[s.chr[i]] |= 1u << (p->move - 1);
			if (p->stun && !p->elapsed && p->freeze && !before.p[i].freeze) {
				blocks += p->block == 1;
				hits += p->block != 1;
				crouch_blocks += p->block == 1 && p->crouch;
				crouch_hits += p->block != 1 && p->crouch;
			}
		}
	}
	for (unsigned c = 0; c < pack.characters; c++)
		if (started[c] != (1u << pack.chr[c].moves) - 1) {
			fprintf(stderr, "ai: %s started moves %x\n", pack.chr[c].name, started[c]);
			abort();
		}
	/* Every special was started above; a launch proves spawn reaches the
	 * pool (rare with a large roster, so only one is required). */
	unsigned throwers = 0;
	for (unsigned c = 0; c < pack.characters; c++)
		throwers += pack.chr[c].projectiles > 0;
	assert(blocks > 20 && hits > 20 && crouch_blocks > 5 && crouch_hits > 5 && s.round > 3 &&
	       s.run > 0 && (shots > 0 || !throwers) && s.specials > 20);
}
/* Write the scene of a state as a PPM next to the harness preview. */
static const char *preview_base;
static void write_ppm(const char *path, const uint16_t *fb)
{
	FILE *f = fopen(path, "wb");
	assert(f);
	fprintf(f, "P6\n480 320\n255\n");
	for (int i = 0; i < FW * FH; i++) {
		unsigned c = fb[i];
		uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31),
				  (uint8_t)(((c >> 5) & 63) * 255 / 63),
				  (uint8_t)((c & 31) * 255 / 31)};
		assert(fwrite(rgb, 1, 3, f) == 3);
	}
	fclose(f);
}
static void snapshot(const struct fight_state *s, const char *suffix)
{
	static struct scene scene;
	static uint16_t fb[FW * FH];
	char path[512];
	size_t n = strlen(preview_base);
	if (n > 4 && !strcmp(preview_base + n - 4, ".ppm"))
		n -= 4;
	snprintf(path, sizeof(path), "%.*s-%s.ppm", (int)n, preview_base, suffix);
	fight_cast(&pack, s);
	fight_scene(s, &pack, &scene);
	fight_render(&pack, &scene, fb);
	write_ppm(path, fb);
}
/* Select, a Power Wave in flight with the box overlay, a lost run and a
 * cleared one, for human review. */
static void previews(void)
{
	struct fight_state s;
	fight_reset(&s, 0);
	for (int n = 0; n < 20; n++)
		step2(&s, 0, 0);
	snapshot(&s, "select");
	duel(&s, 100, 330);
	for (int n = 1; n <= 40; n++)
		step2(&s, motion(MOTION_236, n, 5, 1, K(Y)), 0);
	assert(shot_of(&s, 0));
	s.boxes = 1;
	snapshot(&s, "wave");
	fight_reset(&s, 0);
	step2(&s, K(Y), 0);
	win_round(&s, 1);
	assert(s.mode == MODE_RESULT);
	for (int n = 0; n < 10; n++)
		step2(&s, 0, 0);
	snapshot(&s, "result");
	fight_reset(&s, 0);
	step2(&s, K(Y), 0);
	while (s.mode == MODE_FIGHT)
		win_round(&s, 0);
	assert(s.mode == MODE_CLEAR);
	snapshot(&s, "clear");
}
static void logic_tests(const uint8_t *data, size_t len)
{
	int automatic;
	assert(fight_assets(&pack, data, len, &automatic));
	terry = 0;
	while (strcmp(pack.chr[terry].name, "TERRY"))
		assert(++terry < pack.characters);
	frame_data();
	hit_timing();
	block_timing();
	corner();
	guard_types();
	trade();
	commands();
	buffer();
	cancels();
	projectile();
	burn_knuckle();
	rising_tackle();
	ladder();
	state_words();
	ai_coverage();
	previews();
}

int main(int argc, char **argv)
{
	assert(argc == 4);
	retro_set_environment(environment);
	retro_set_video_refresh(video);
	retro_set_audio_sample_batch(audio);
	retro_set_input_poll(poll_input);
	retro_set_input_state(input);
	retro_init();
	assert(retro_api_version() == RETRO_API_VERSION);
	struct retro_system_info si;
	struct retro_system_av_info av;
	retro_get_system_info(&si);
	retro_get_system_av_info(&av);
	assert(!strcmp(si.valid_extensions, "fight") && !si.need_fullpath &&
	       av.timing.fps == 60 && av.timing.sample_rate == 48000);
	size_t len;
	uint8_t *data = read_all(argv[1], &len);
	render_tests(data, len);
	struct retro_game_info g = {argv[1], data, len, NULL};
	assert(!retro_load_game(NULL));
	for (size_t i = 0; i < 32; i++) {
		g.size = i;
		assert(!retro_load_game(&g));
	}
	g.size = len - 1;
	assert(!retro_load_game(&g));
	g.size = len;
	data[100] ^= 1;
	assert(!retro_load_game(&g));
	data[100] ^= 1;
	data[8] ^= 1;
	assert(!retro_load_game(&g));
	data[8] ^= 1;
	reject_formats = true;
	assert(!retro_load_game(&g));
	reject_formats = false;
	assert(retro_load_game(&g));
	assert(formats >= 3);
	size_t n = retro_serialize_size();
	uint8_t *start = malloc(n), *end = malloc(n), *copy = malloc(n);
	assert(start && end && copy);
	assert(!retro_serialize(start, n - 1));
	save(start, n);
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	for (int i = 0; i < 60; i++)
		retro_run();
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_A;
	for (int i = 0; i < 120; i++)
		retro_run();
	save(end, n);
	assert(memcmp(start, end, n));
	uint32_t picture = image_hash, sound = sound_hash;
	assert(retro_unserialize(start, n));
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	for (int i = 0; i < 60; i++)
		retro_run();
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_A;
	for (int i = 0; i < 120; i++)
		retro_run();
	save(copy, n);
	assert(!memcmp(end, copy, n) && picture == image_hash && sound == sound_hash);
	assert(n == 16 + STATE_WORDS * 4);
	memcpy(copy, end, n);
	copy[8] ^= 1;
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	copy[7] = '1'; /* FGSTATE1 from the fixed-sprite POC */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	copy[7] = '2'; /* FGSTATE2, before boxes, moves and hitstop */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	copy[7] = '3'; /* FGSTATE3, before buffer, specials, projectiles, ladder */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memset(copy + 16 + 8 * 4, 0xff, 4);
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	copy[16 + (STATE_WORDS - 1) * 4] = 1; /* reserved words stay zero */
	assert(!retro_unserialize(copy, n));
	assert(!retro_unserialize(end, n - 1));
	save(copy, n);
	assert(!memcmp(end, copy, n));
	/* Pause freezes simulation and audio state, while rendering continues. */
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_START;
	retro_run();
	keys[0] = 0;
	retro_run();
	save(start, n);
	for (int i = 0; i < 10; i++)
		retro_run();
	save(copy, n);
	assert(!memcmp(start, copy, n));
	retro_reset();
	keys[0] = 0;
	retro_run();
	/* Snapshot a frame for human review without running the handheld. */
	FILE *f = fopen(argv[3], "wb");
	assert(f);
	fprintf(f, "P6\n480 320\n255\n");
	for (int i = 0; i < FW * FH; i++) {
		unsigned c = last_frame[i];
		uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31),
				  (uint8_t)(((c >> 5) & 63) * 255 / 63),
				  (uint8_t)((c & 31) * 255 / 31)};
		assert(fwrite(rgb, 1, 3, f) == 3);
	}
	fclose(f);
	retro_unload_game();
	assert(!retro_serialize(start, n));
	use32 = true;
	assert(retro_load_game(&g));
	retro_run();
	retro_unload_game();
	use32 = false;
	players = "two";
	assert(retro_load_game(&g));
	/* Port 1 drives the ladder's opponent once P1 picked a fighter. */
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	retro_run();
	keys[0] = 0;
	keys[1] = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	retro_run();
	save(start, n);
	keys[1] = 0;
	retro_reset();
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	retro_run();
	keys[0] = 0;
	retro_run();
	save(end, n);
	assert(memcmp(start, end, n));
	/* A disconnected port must neither move nor attack. */
	retro_set_controller_port_device(1, RETRO_DEVICE_NONE);
	retro_reset();
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	retro_run();
	keys[0] = 0;
	keys[1] = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	retro_run();
	save(copy, n);
	assert(!memcmp(end, copy, n));
	retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
	keys[1] = 0;
	retro_unload_game();
	players = "cpu";
	free(data);
	data = read_all(argv[2], &len);
	g.data = data;
	g.size = len;
	assert(retro_load_game(&g));
	unsigned runs = getenv("FIGHTER_LONG_TEST") ? 36010 : 2500;
	for (unsigned i = 0; i < runs; i++)
		retro_run();
	assert(benchmark_complete == (runs >= 36000 ? 1u : 0u));
	save(start, n);
	assert(video_count == audio_count);
	assert(video_count > 2800);
	/* Rules on exact ticks against the manifest's frame data. */
	preview_base = argv[3];
	logic_tests(data, len);
	retro_unload_game();
	retro_deinit();
	free(start);
	free(end);
	free(copy);
	free(data);
	puts("fighter-test: pack decoder, box/move/special/projectile validation, "
	     "pixel-exact renderer, box flip and overlay, effect budget, content, "
	     "RGB565/XRGB8888, audio, ports, replay, pause, state validation, frame "
	     "data, hit/block/hitstop ticks, pushback and corner, guard heights, "
	     "trades, commands, buffer, prejump, cancels, projectiles and clash, "
	     "specials, ladder, AI coverage and stress cycle passed");
	return 0;
}
