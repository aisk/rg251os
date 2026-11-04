/* Native libretro callback harness: malformed content, deterministic state,
 * output formats, audio (the sound bank next to the pack, replayed after a
 * state load to the same samples) and a complete automated scene cycle, plus
 * the rules on exact ticks: frame data, hit/block/hitstop/hitstun timing,
 * pushback and corner transfer, guard heights, trades, combo counts, motion
 * and charge commands, the input buffer and prejump, special and chain
 * cancels, Power Wave flight, hits, guard and clash, Burn Knuckle motion,
 * Rising Tackle height, follow-ups (Iori's three Aoi Hana stages: windows,
 * the buffer, expiry, contact, the move clock across a state load; May
 * Lee's Tatsuo Kyaku branches by held direction), the arcade ladder (the
 * versus card and READY freezing the rules, held buttons no press at FIGHT,
 * the banner, select, opponents, loss, clear, Auto rotation, replay, a full
 * MAX_CHARACTERS roster with its 64-bit beaten mask across a save, and
 * the select grid's moves, scrolling and bounds on rosters up to it), the
 * survival and practice modes (carried life, heal, streak and record; the
 * dummy picked and its four behaviours, refilled life, combo damage; B
 * quitting from a pause), the round clock, time over and best of three,
 * state
 * round-trip and rejection, run and backdash (taps, phases, speeds, the
 * arc, cancels, throw and strike invulnerability, samsho5sp's coast), the
 * AI's use of every move (and of runs and backdashes), the difficulty
 * (R on the select screen, Easy damage, guards, specials and Hard's cancels per level), and
 * the stats: each character's system and resolved stats, walk speeds, jump
 * arcs and prejump, life, defense, weight, push distance, guard range, input
 * windows and the samsho5sp damage scale changing the fight by exact
 * amounts. Those run on the flat fallback arena (480 wide, camera at 0).
 * On the pack's stages it checks the ladder's stage choice, the camera
 * (start, following the midpoint, clamped to the stage), the screen and
 * stage walls (also with a narrower play width), corners at both stage
 * ends, a pushback transfer at a screen wall, projectiles leaving the
 * camera's window, hit/guard/slash sparks and blood at the contact point,
 * the spark pool, shake and KO flash, every sound event, and that camera,
 * sparks and sound events survive save/load, replay identically and reject
 * impossible values; the AI run keeps both fighters on screen on every
 * stage. Expected numbers come from assets/fighter/terry.json,
 * haohmaru.json and systems.json. Fighter x is in quarter pixels (QX). It
 * also writes preview PPMs of the select, Power Wave (boxes on), result and
 * clear screens, a hit on each stage, ROUND 2 and the clock, the survival
 * select, the dummy pick, the practice HUD and a scrolled 64-character
 * select and dummy pick. test-render.c adds the pack
 * decoder, box, stage and pixel-exact renderer checks. ASan/UBSan run this
 * without a window or device. */
#include "audio.h"
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
static unsigned benchmark_complete, sound_banks, level_logs;
static char run_over[256];
static void log_line(enum retro_log_level level, const char *fmt, ...)
{
	(void)level;
	if (fmt && strstr(fmt, "run over")) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(run_over, sizeof(run_over), fmt, ap);
		va_end(ap);
	}
	if (strstr(fmt, "fight: sound bank"))
		sound_banks++;
	if (strstr(fmt, "fight: difficulty %s"))
		level_logs++;
	if (strstr(fmt, "benchmark complete:"))
		benchmark_complete++;
}
static uint16_t last_frame[FW * FH];
static const char *players = "cpu", *difficulty_option = "normal";
static bool environment(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_line;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = data;
		v->value = !strcmp(v->key, "fight_difficulty") ? difficulty_option : players;
		return true;
	}
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
/* State word k of a saved state (after the 16-byte header). */
static uint32_t get_word(const uint8_t *p, int k)
{
	p += 16 + 4 * k;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
/* ---- rules on exact ticks; expected numbers come from terry.json ---- */
#define K(k) (1u << RETRO_DEVICE_ID_JOYPAD_##k)
/* Words per fighter in a saved state, mirroring FIGHTER_WORDS in fighter.c. */
#define FIGHTER_WORDS_T 31
static struct assets pack;
static unsigned terry;
enum { POWER_WAVE = MOVES, BURN_KNUCKLE, RISING_TACKLE };
static unsigned haohmaru;
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* Characters c0 and c1 at world columns x0 and x1, the camera on their
 * midpoint. */
static void match(struct fight_state *s, unsigned c0, unsigned c1, int x0, int x1)
{
	fight_reset(s, 0);
	fight_start(s, &pack, c0, c1);
	s->stage = 1;
	s->p[0].x = QX(x0);
	s->p[1].x = QX(x1);
	s->cam = (uint32_t)clampi((x0 + x1) / 2 - FW / 2, 0, fight_width(s, &pack) - FW);
}
static void duel(struct fight_state *s, int x0, int x1) { match(s, terry, terry, x0, x1); }
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
	    {3, 3, 6, 30, 11, 9, 8, 14, 18, GUARD_HIGH},
	    {6, 5, 13, 70, 19, 17, 12, 22, 26, GUARD_HIGH},
	    {5, 4, 8, 35, 11, 9, 8, 16, 20, GUARD_HIGH},
	    {10, 6, 17, 80, 19, 17, 12, 24, 28, GUARD_HIGH},
	    {2, 3, 5, 25, 11, 9, 8, 12, 16, GUARD_HIGH},
	    {6, 4, 12, 65, 19, 17, 12, 20, 24, GUARD_HIGH},
	    {4, 4, 8, 30, 11, 9, 8, 14, 18, GUARD_LOW},
	    {9, 4, 18, 75, 19, 17, 12, 24, 28, GUARD_LOW}};
	static const unsigned cancel[MOVES] = {
	    CANCEL_CHAIN | CANCEL_SPECIAL, CANCEL_SPECIAL, CANCEL_SPECIAL, 0,
	    CANCEL_CHAIN | CANCEL_SPECIAL, CANCEL_SPECIAL, CANCEL_CHAIN | CANCEL_SPECIAL, 0};
	const struct character *c = &pack.chr[terry];
	assert(!strcmp(c->name, "TERRY") && c->moves == MOVES + 5 && c->projectiles == 1);
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
	       bk->hit.hitstun == 19 && bk->hit.blockstun == 17 && bk->groups == 1);
	assert(rt->special && rt->motion == CHARGE_28 && rt->startup == 6 && rt->active == 18 &&
	       rt->recovery == 33 && rt->hit.damage == 100 && rt->hit.chip == 20);
	const struct projectile *wave = &c->projectile[0];
	assert(wave->speed == 4 && wave->lifetime == 120 && wave->x == 60 && wave->y == 0 &&
	       wave->hit.damage == 70 && wave->hit.chip == 14 && wave->hit.hitstun == 31 &&
	       wave->hit.blockstun == 29 && wave->hit.hitstop == 12 && wave->hit.pushback == 20 &&
	       wave->hit.block_pushback == 24 && wave->blend == OPAQUE &&
	       wave->anim.count == 6 && wave->anim.total == 18);
}
/* Stand LP connects on its first active tick (call 4), both freeze for 8
 * ticks (5..12), the attacker recovers on call 4 + 8 + (3 + 6) = 21 and the
 * defender on 4 + 8 + 11 = 23: +2, as `pack.py data` prints. */
static void hit_timing(void)
{
	static const int slide[7] = {264, 267, 269, 271, 272, 273, 274};
	struct fight_state s;
	duel(&s, 200, 260);
	for (int n = 1; n <= 24; n++) {
		run(&s, n, K(Y), 0, 0);
		const struct fighter *a = &s.p[0], *d = &s.p[1];
		assert(d->hp == (n < 4 ? 1000 : 970));
		assert((a->move == STAND_LP + 1) == (n < 21));
		assert(a->freeze == (n >= 4 && n <= 12 ? 12 - n : 0) && d->freeze == a->freeze);
		assert(d->stun == (n < 4 ? 0 : n <= 12 ? 11 : n < 23 ? 23 - n : 0));
		assert(a->hit == (n >= 4 && n < 21));
		if (n >= 4 && n <= 12)
			assert(a->elapsed == 3);
		if (n >= 4 && n < 23)
			assert(in_action(&s, 1, HURT) && !d->block);
		assert(d->x == QX(n <= 12 ? 260 : n <= 19 ? slide[n - 13] : 274));
		assert(a->x == QX(200) && s.guards == 0 && s.hits == (n >= 4));
	}
}
/* Blocked stand LP: P1 walks back walk_back (8 quarter pixels) on call 1
 * before the attack threatens it, stance from call 2, blockstun 9 after
 * hitstop 8 ends on call 21 (+0), block pushback 18 px over calls 13..20
 * (offsets from 260). */
static void block_timing(void)
{
	static const int slide[8] = {5, 9, 12, 14, 15, 16, 17, 18};
	struct fight_state s;
	duel(&s, 200, 260);
	int back = pack.chr[terry].stats.walk_back;
	assert(back == 8);
	for (int n = 1; n <= 20; n++) {
		run(&s, n, K(Y), 0, K(RIGHT));
		const struct fighter *d = &s.p[1];
		assert(d->hp == 1000 && s.guards == (n >= 4) && s.hits == (n >= 4));
		assert(d->block == (n == 1 ? 0 : n < 4 ? 2 : 1));
		assert(d->stun == (n < 4 ? 0 : n <= 12 ? 9 : 21 - n));
		assert(d->x == QX(260) + back + QX(n <= 12 ? 0 : slide[n - 13]));
		if (n >= 2)
			assert(in_action(&s, 1, GUARD));
		assert(s.p[0].move != 0);
	}
	/* Blockstun and the swing end together on call 21; the swing still
	 * holds the guard stance that tick, so the walk resumes on call 22. */
	run(&s, 21, 0, 0, K(RIGHT));
	assert(!s.p[1].stun && !s.p[0].move && s.p[1].x == QX(278) + back);
	run(&s, 22, 0, 0, K(RIGHT));
	assert(!s.p[1].block && s.p[1].x == QX(278) + 2 * back);
}
/* In the corner the defender cannot slide: the attacker moves back the
 * same 14 pixels on the same ticks. Walls of the state's camera; on a stage
 * (arena k) the corner is the stage's right end, then its left end. */
static void staged(struct fight_state *s, unsigned k, unsigned c0, unsigned c1, int x0, int x1);
static void corner_at(int k)
{
	static const int slide[7] = {4, 7, 9, 11, 12, 13, 14};
	struct fight_state s;
	int lo, hi, width = k < 0 ? FW : pack.stage[k].width;
	if (k < 0)
		duel(&s, 0, 0);
	else
		staged(&s, (unsigned)k, terry, terry, width - 100, width - 40);
	s.cam = (uint32_t)(width - FW);
	fight_walls(&s, &pack, &lo, &hi);
	assert(hi == width - pack.sys[pack.chr[terry].system].margin || k >= 0);
	s.p[0].x = QX(hi - 60);
	s.p[1].x = QX(hi);
	for (int n = 1; n <= 20; n++) {
		run(&s, n, K(Y), 0, 0);
		assert(s.p[1].x == QX(hi) && s.p[1].hp == (n < 4 ? 1000 : 970));
		assert(s.p[0].x == QX(hi - 60 - (n <= 12 ? 0 : n <= 19 ? slide[n - 13] : 14)));
		assert(s.cam == (uint32_t)(width - FW));
	}
	/* Mirrored: P1 attacks P0 pinned at the left wall. */
	if (k < 0)
		duel(&s, 0, 0);
	else
		staged(&s, (unsigned)k, terry, terry, 40, 100);
	s.cam = 0;
	fight_walls(&s, &pack, &lo, &hi);
	s.p[0].x = QX(lo);
	s.p[1].x = QX(lo + 60);
	for (int n = 1; n <= 22; n++)
		step2(&s, 0, n == 1 ? K(Y) : 0);
	assert(s.p[0].x == QX(lo) && s.p[1].x == QX(lo + 60 + 14) && s.p[0].hp == 970 &&
	       s.cam == 0);
}
static void corner(void) { corner_at(-1); }
/* High, low and overhead against standing and crouching guard. */
static void guard_types(void)
{
	struct fight_state s;
	/* Crouch LK (low, startup 4) hits a standing guard on call 5... */
	duel(&s, 200, 260);
	for (int n = 1; n <= 5; n++)
		run(&s, n, K(B), K(DOWN), K(RIGHT));
	assert(s.p[0].move == CROUCH_LK + 1 && s.p[1].hp == 970 && s.p[1].stun == 11 &&
	       !s.guards && in_action(&s, 1, HURT));
	/* ...and is blocked low on call 5, blockstun 9, crouching guard. */
	duel(&s, 200, 260);
	for (int n = 1; n <= 4; n++) {
		run(&s, n, K(B), K(DOWN), K(DOWN) | K(RIGHT));
		assert(s.p[1].crouch && s.p[1].hp == 1000 && !s.p[1].stun);
	}
	run(&s, 5, K(B), K(DOWN), K(DOWN) | K(RIGHT));
	assert(s.guards == 1 && s.p[1].stun == 9 && s.p[1].block == 1 &&
	       in_action(&s, 1, CROUCH_GUARD) && s.p[1].x == QX(260));
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
	assert(s.hits == 2 && s.p[0].hp == 970 && s.p[1].hp == 970 && s.p[0].stun == 11 &&
	       s.p[1].stun == 11 && s.p[0].freeze == 8 && s.p[1].freeze == 8 &&
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
	struct stats windows = pack.chr[terry].stats;
	const struct stats *st = &windows;
	assert(st->quarter == 15 && st->dragon == 20 && st->charge == 30 && st->grace == 10 &&
	       st->release == 4);
	memset(&p, 0, sizeof(p));
	p.face = 1;
	/* 236 with the 2 at most 14 ticks before the newest direction. */
	set_history(&p, "236");
	assert(fight_motion(&p, st, MOTION_236) && !fight_motion(&p, st, MOTION_214) &&
	       !fight_motion(&p, st, MOTION_623));
	set_history(&p, "2555555555555" "36");
	assert(fight_motion(&p, st, MOTION_236));
	set_history(&p, "25555555555555" "36");
	assert(!fight_motion(&p, st, MOTION_236));
	/* In-between directions are allowed; order is not optional. */
	set_history(&p, "21236");
	assert(fight_motion(&p, st, MOTION_236));
	set_history(&p, "326");
	assert(!fight_motion(&p, st, MOTION_236));
	/* Directions follow facing: 214 for a left-facing fighter is D, DR, R. */
	set_history(&p, "214");
	assert(fight_motion(&p, st, MOTION_214));
	p.face = -1;
	assert(fight_motion(&p, st, MOTION_236) && !fight_motion(&p, st, MOTION_214));
	p.face = 1;
	/* 623 within 20 ticks; it is not a 236. */
	set_history(&p, "6222223");
	assert(fight_motion(&p, st, MOTION_623) && !fight_motion(&p, st, MOTION_236));
	set_history(&p, "6555555555555555555" "23");
	assert(!fight_motion(&p, st, MOTION_623));
	set_history(&p, "655555555555555555" "23");
	assert(fight_motion(&p, st, MOTION_623));
	/* Half circles within 20 ticks: 41236 also completes 236, a plain 236
	 * is no 41236; 63214 likewise with 214. 421 is not a 214, nor the
	 * other way round. All follow facing. */
	set_history(&p, "41236");
	assert(fight_motion(&p, st, MOTION_41236) && fight_motion(&p, st, MOTION_236) &&
	       !fight_motion(&p, st, MOTION_63214));
	set_history(&p, "4555555555555555555" "26");
	assert(!fight_motion(&p, st, MOTION_41236));
	set_history(&p, "455555555555555555" "26");
	assert(fight_motion(&p, st, MOTION_41236));
	set_history(&p, "5236");
	assert(!fight_motion(&p, st, MOTION_41236));
	set_history(&p, "63214");
	assert(fight_motion(&p, st, MOTION_63214) && fight_motion(&p, st, MOTION_214) &&
	       !fight_motion(&p, st, MOTION_41236) && !fight_motion(&p, st, MOTION_421));
	set_history(&p, "421");
	assert(fight_motion(&p, st, MOTION_421) && !fight_motion(&p, st, MOTION_214));
	set_history(&p, "214");
	assert(!fight_motion(&p, st, MOTION_421));
	p.face = -1;
	set_history(&p, "63214");
	assert(fight_motion(&p, st, MOTION_41236) && !fight_motion(&p, st, MOTION_63214));
	set_history(&p, "623");
	assert(fight_motion(&p, st, MOTION_421));
	p.face = 1;
	/* Charges: back held 30 (byte 0) or released within the grace (byte 1). */
	set_history(&p, "6");
	p.charge = 29;
	assert(!fight_motion(&p, st, CHARGE_46));
	p.charge = 30;
	assert(fight_motion(&p, st, CHARGE_46) && !fight_motion(&p, st, CHARGE_28));
	p.charge = 1u << 8;
	assert(fight_motion(&p, st, CHARGE_46));
	/* The release direction must be among the last 4 directions. */
	set_history(&p, "65555");
	assert(!fight_motion(&p, st, CHARGE_46));
	set_history(&p, "6555");
	assert(fight_motion(&p, st, CHARGE_46));
	set_history(&p, "9");
	p.charge = 30u << 16;
	assert(fight_motion(&p, st, CHARGE_28) && !fight_motion(&p, st, CHARGE_46));
	set_history(&p, "7");
	assert(fight_motion(&p, st, CHARGE_28));
	/* The windows are the fighter's: with the 2 nine ticks before the 6 a
	 * quarter window of 9 misses it and 10 sees it, a 40-tick charge needs
	 * 40, a release window of 2 misses a forward 2 ticks back. */
	set_history(&p, "25555555" "36");
	windows.quarter = 9;
	assert(!fight_motion(&p, st, MOTION_236));
	windows.quarter = 10;
	assert(fight_motion(&p, st, MOTION_236));
	set_history(&p, "6");
	p.charge = 30;
	windows.charge = 40;
	assert(!fight_motion(&p, st, CHARGE_46));
	p.charge = 40;
	assert(fight_motion(&p, st, CHARGE_46));
	set_history(&p, "655");
	windows.release = 2;
	assert(!fight_motion(&p, st, CHARGE_46));
	windows.release = 3;
	assert(fight_motion(&p, st, CHARGE_46));
}
/* Presses in the last buffer (4) recovery ticks come out on the first free
 * tick; earlier ones are dropped. A whiffed stand LP pressed on call 1 ends
 * on call 13. */
static void buffer(void)
{
	struct fight_state s;
	assert(pack.chr[terry].stats.buffer == 4 && pack.chr[terry].stats.jump.prejump == 4 &&
	       pack.chr[terry].stats.jump.vy == -51 && pack.chr[terry].stats.jump.gravity == 3);
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
	/* Up starts a 4-tick prejump; airborne from call 5. A press during it
	 * replaces the jump. */
	duel(&s, 100, 400);
	for (int n = 1; n <= 6; n++) {
		step2(&s, K(UP), 0);
		assert(s.p[0].squat == (n < 5 ? 5 - n : 0) && s.p[0].y == (n < 6 ? 0 : -12));
		assert(s.p[0].vy == (n < 5 ? 0 : n == 5 ? -51 : -48));
	}
	duel(&s, 100, 400);
	step2(&s, K(UP), 0);
	step2(&s, K(UP) | K(Y), 0);
	assert(s.p[0].move == STAND_LP + 1 && !s.p[0].squat);
	for (int n = 3; n <= 20; n++)
		step2(&s, 0, 0);
	assert(!s.p[0].y && !s.p[0].vy && !s.p[0].move);
}
/* Cancels. Stand LP hits on call 4 (hitstop 5..12); 236+Y entered during
 * the hitstop (button on call 9) cancels into Power Wave on call 13, which
 * launches on its elapsed 17 = call 30 at 200 + 60 = 260, already inside
 * the pushed-back P1 (274, lowest hurtbox from 274 - 31 + 6... = 249). */
static void cancels(void)
{
	struct fight_state s;
	duel(&s, 200, 260);
	for (int n = 1; n <= 32; n++) {
		uint32_t k = n == 1 ? K(Y) : motion(MOTION_236, n, 9, 1, K(Y));
		step2(&s, k, 0);
		if (n >= 4 && n <= 12)
			assert(s.p[0].move == STAND_LP + 1 && s.p[0].freeze == 12 - n);
		if (n >= 9 && n <= 12)
			assert(s.p[0].buffer == POWER_WAVE + 1 && s.p[0].age == 0);
		if (n >= 13)
			assert(s.p[0].move == POWER_WAVE + 1 && s.p[0].elapsed == n - 13);
		if (n < 30)
			assert(!live_shots(&s) && s.p[1].hp == (n < 4 ? 1000 : 970));
	}
	assert(s.specials == 1 && s.hits == 2 && s.p[1].hp == 900 && !live_shots(&s));
	/* No cancel without a hit: the same input after a whiff is dropped. */
	duel(&s, 100, 400);
	for (int n = 1; n <= 14; n++) {
		step2(&s, n == 1 ? K(Y) : motion(MOTION_236, n, 6, 1, K(Y)), 0);
		assert(s.p[0].move == (n < 13 ? STAND_LP + 1 : 0));
	}
	/* Stand HK has no cancel: hit on call 11, hitstop 12..23, the special
	 * pressed on call 16 is dropped after 4 unfrozen ticks (call 28) and
	 * HK ends on call 11 + 12 + 23 = 46. */
	duel(&s, 200, 260);
	for (int n = 1; n <= 46; n++) {
		step2(&s, n == 1 ? K(A) : motion(MOTION_236, n, 16, 1, K(Y)), 0);
		assert(s.p[0].move == (n < 46 ? STAND_HK + 1 : 0));
		assert((s.p[0].buffer != 0) == (n >= 16 && n < 28));
	}
	assert(s.p[1].hp == 920 && !s.specials);
	/* Chain: crouch LP hits on call 3 (hitstop 4..11); a second crouch LP
	 * pressed on call 5 starts on call 12. */
	duel(&s, 200, 250);
	for (int n = 1; n <= 12; n++)
		step2(&s, K(DOWN) | (n == 1 || n == 5 ? K(Y) : 0), 0);
	assert(s.p[0].move == CROUCH_LP + 1 && !s.p[0].elapsed && !s.p[0].hit &&
	       s.p[1].hp == 975);
	for (int n = 13; n <= 14; n++)
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
		for (int n = 1; n <= 66; n++) {
			/* P1 holds back once the wave is within guard range. */
			step2(&s, motion(MOTION_236, n, 5, 1, K(Y)), guard && n >= 40 ? K(RIGHT) : 0);
			const struct shot *sh = shot_of(&s, 0);
			assert(s.p[0].x == QX(100));
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
				assert(s.p[1].stun == (guard ? 29 : 31) && s.p[1].freeze == 12);
				assert(s.p[0].freeze == 0 && s.p[1].block == guard);
			}
		}
		/* Pushback 20 / 24 slides P1 after the hitstop; P0 stays. */
		assert(s.p[1].x > QX(300) && s.p[0].x == QX(100));
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
			assert(s.p[0].move == BURN_KNUCKLE + 1 && s.p[0].x == QX(x[n - 5]));
	}
	assert(!s.p[0].move && s.p[0].x == QX(185));
	/* P1 at 200 facing left: hurtbox from 200 - 30 = 170; the fist box
	 * [36, -84, 28, 24] at x + 36 .. x + 64 reaches it once x + 64 > 170. */
	duel(&s, 100, 200);
	for (int n = 1; n <= 18; n++)
		step2(&s, motion(MOTION_214, n, 5, 1, K(Y)), 0);
	/* It knocks down: no hitstun, P1 falls. */
	assert(s.p[1].hp == 910 && !s.p[1].stun && s.p[1].down == FALLING &&
	       s.p[0].freeze == 12 && s.p[0].elapsed == 13 && s.p[0].x == QX(114));
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
	assert(s.p[1].hp == 900 && s.p[0].y == -18 && s.p[0].freeze == 12);
}
/* Follow-ups (rekkas) on Iori's three-stage Aoi Hana and May Lee's Tatsuo
 * Kyaku branches. A first 214+P lands on call 5, so stage 1 shows elapsed
 * n - 6 when call n checks a cancel: a follow-up pressed on call n starts
 * there once n - 6 reaches open, from the buffer if pressed up to
 * stats.buffer calls before. */
static unsigned character(const char *name)
{
	unsigned c = 0;
	while (strcmp(pack.chr[c].name, name))
		assert(++c < pack.characters);
	return c;
}
static unsigned child(const struct character *c, unsigned parent, int motion)
{
	for (unsigned k = MOVES; k < c->moves; k++)
		if (c->move[k].after == parent + 1 && c->move[k].motion == motion)
			return k;
	abort();
}
/* Iori at 100 against a far P1 (or one at x1): 214+P on call 5, then the
 * keys of `second` on calls up to `last`; returns the first call that shows
 * move k + 1, or 0. */
static int aoi_run(unsigned iori, int x1, int second, uint32_t button, int last, unsigned k,
		   struct fight_state *s)
{
	int seen = 0;
	match(s, iori, terry, 100, x1);
	for (int n = 1; n <= last; n++) {
		uint32_t in = motion(MOTION_214, n, 5, 1, K(Y));
		if (second)
			in |= button == K(Y) ? motion(MOTION_214, n, second, 1, K(Y))
					       : n == second ? button : 0;
		step2(s, in, 0);
		if (!seen && s->p[0].move == (int)k + 1)
			seen = n;
	}
	return seen;
}
static void follow_ups(void)
{
	struct fight_state s, t;
	uint32_t w[STATE_WORDS], v[STATE_WORDS];
	unsigned iori = character("IORI"), maylee = character("MAYLEE");
	struct character *c = &pack.chr[iori];
	unsigned a1 = MOVES;
	while (c->move[a1].motion != MOTION_214 || c->move[a1].after)
		assert(++a1 < c->moves);
	unsigned a2 = child(c, a1, MOTION_214), a3 = child(c, a2, MOTION_214);
	const struct move *m1 = &c->move[a1], *m2 = &c->move[a2], *m3 = &c->move[a3];
	int open = m2->open, buf = c->stats.buffer, total = m1->anim.total;
	assert(m2->special && !m2->contact && m3->hit.guard == GUARD_OVERHEAD &&
	       m1->hit.guard != GUARD_OVERHEAD && open <= m2->close && m2->close < total);
	/* One 214+P plays stage 1 alone and ends. */
	assert(aoi_run(iori, 400, 0, 0, 5 + total + 4, a2, &s) == 0 && !s.p[0].move);
	assert(aoi_run(iori, 400, 0, 0, 5, a1, &s) == 5);
	/* A second one in the window starts stage 2 on that call; up to
	 * stats.buffer calls early it waits in the buffer, earlier it expires. */
	assert(aoi_run(iori, 400, 6 + open + 2, K(Y), 6 + open + 2, a2, &s) == 6 + open + 2 &&
	       !s.p[0].elapsed && !s.p[0].clock);
	assert(aoi_run(iori, 400, 6 + open - buf, K(Y), 6 + open, a2, &s) == 6 + open);
	assert(aoi_run(iori, 400, 5 + open - buf, K(Y), 5 + total + 4, a2, &s) == 0);
	/* Past close, or P alone, never. */
	assert(aoi_run(iori, 400, 7 + m2->close, K(Y), 5 + total + 4, a2, &s) == 0);
	assert(aoi_run(iori, 400, 6 + open + 2, K(X), 5 + total + 4, a2, &s) == 0);
	/* Stage 3 follows stage 2 (started on call f) the same way, only
	 * from it. */
	int f = 6 + open + 2, g = f + 1 + m3->open;
	match(&s, iori, terry, 100, 400);
	for (int n = 1; n <= g; n++)
		step2(&s, motion(MOTION_214, n, 5, 1, K(Y)) | motion(MOTION_214, n, f, 1, K(Y)) |
			      motion(MOTION_214, n, g, 1, K(Y)), 0);
	assert(s.p[0].move == (int)a3 + 1 && !s.p[0].elapsed);
	/* The state carries the move clock: a copy decoded mid stage 2 takes
	 * the same stage 3 press. */
	match(&s, iori, terry, 100, 400);
	for (int n = 1; n <= g + 6; n++) {
		if (n == g - 3) {
			fight_encode(&s, w);
			assert(fight_decode(&t, &pack, w) && t.p[0].clock == s.p[0].clock &&
			       t.p[0].clock > 0);
		}
		uint32_t in = motion(MOTION_214, n, 5, 1, K(Y)) | motion(MOTION_214, n, f, 1, K(Y)) |
				motion(MOTION_214, n, g, 1, K(Y));
		step2(&s, in, 0);
		if (n >= g - 3)
			step2(&t, in, 0);
	}
	fight_encode(&s, w);
	fight_encode(&t, v);
	assert(s.p[0].move == (int)a3 + 1 && !memcmp(w, v, sizeof(w)));
	/* With contact required a whiff has no stage 2; after a hit the press
	 * on the call after stage 1 shows open unfrozen starts it. */
	c->move[a2].contact = 1;
	assert(aoi_run(iori, 400, 6 + open + 2, K(Y), 5 + total + 4, a2, &s) == 0);
	int x1 = 100 + m1->reach, hit_at = 0;
	match(&s, iori, terry, 100, x1);
	for (int n = 1; n <= 60 && !hit_at; n++) {
		step2(&s, motion(MOTION_214, n, 5, 1, K(Y)), 0);
		if (s.p[0].move == (int)a1 + 1 && s.p[0].hit && !s.p[0].freeze &&
		    s.p[0].elapsed == open)
			hit_at = n + 1;
	}
	assert(hit_at && s.p[1].hp < pack.chr[terry].stats.life);
	assert(aoi_run(iori, x1, hit_at, K(Y), hit_at, a2, &s) == hit_at);
	c->move[a2].contact = 0;
	/* May Lee: Tatsuo Kyaku (236+K on call 5), then K on call 6 + open
	 * with up, down, forward or any other direction (none included) picks
	 * its branch; outside the window it does nothing. */
	const struct character *ml = &pack.chr[maylee];
	unsigned tk = MOVES;
	while (ml->move[tk].motion != MOTION_236 || ml->move[tk].after)
		assert(++tk < ml->moves);
	unsigned up = child(ml, tk, HOLD_8), down = child(ml, tk, HOLD_2),
		 fwd = child(ml, tk, HOLD_6), any = child(ml, tk, MOTION_ANY);
	static const uint32_t dir[5] = {K(UP), K(DOWN), 0, K(RIGHT), K(LEFT)};
	for (int d = 0; d < 5; d++) {
		unsigned want = d == 0 ? up : d == 1 ? down : d == 3 ? fwd : any;
		int at = 6 + ml->move[want].open;
		assert(ml->move[want].open == ml->move[any].open);
		for (int late = 0; late < 2; late++) {
			int press = late ? 7 + ml->move[want].close : at;
			match(&s, maylee, terry, 100, 400);
			int seen = 0;
			for (int n = 1; n <= press; n++) {
				step2(&s, motion(MOTION_236, n, 5, 1, K(B)) |
					      (n == press ? dir[d] | K(B) : 0), 0);
				seen |= s.p[0].move == (int)want + 1;
			}
			assert(late ? !seen && s.p[0].move != (int)want + 1
				    : s.p[0].move == (int)want + 1 && !s.p[0].elapsed);
		}
	}
}
/* The arcade ladder on a four-character roster made of Terry copies:
 * select, random unbeaten opponents, a loss, a clear and save/replay. */
/* Step through a ladder round's versus card and READY: nothing moves, a
 * held button is no press afterwards and ROUND_START sounds at FIGHT. */
static void intro(struct fight_state *s)
{
	assert(s->mode == MODE_FIGHT);
	struct fight_state t = *s;
	for (uint32_t n = s->timer; n < ROUND_TICKS; n++) {
		step2(s, K(A) | K(LEFT), K(B));
		assert(s->p[0].x == t.p[0].x && !s->p[0].move && !s->p[1].move &&
		       s->p[0].keys == (int32_t)(K(A) | K(LEFT)) &&
		       (s->timer == ROUND_TICKS || !s->sounds));
	}
	assert(s->timer == ROUND_TICKS && s->sounds == 1 && s->sound[0].id == SOUND_ROUND_START);
	/* A, still held, does not kick when the round begins. */
	step2(s, K(A), 0);
	assert(!s->p[0].move);
}
/* Win (or lose) a whole match: rounds until the next opponent, the result
 * or clear screen. */
static void win_round(struct fight_state *s, int winner)
{
again:
	if (s->timer < ROUND_TICKS)
		intro(s);
	/* Stand LP into a 10 hp loser: KO within a few calls, round over 180
	 * later. A CPU loser may guard the jab (its life still 10); then the
	 * jab is thrown again. */
	for (int tries = 0; tries < 8 && (tries == 0 || (s->mode == MODE_FIGHT &&
	     s->p[1 - winner].hp == 10)); tries++) {
		s->p[0].x = QX(200);
		s->p[1].x = QX(260);
		s->p[1 - winner].hp = 10;
		for (int n = 1; n <= 184; n++)
			step2(s, winner == 0 && n == 1 ? K(Y) : 0, winner == 1 && n == 1 ? K(Y) : 0);
	}
	/* Slower jabs (a sword LP) KO a few ticks later. */
	for (int n = 0; n < 60 && s->mode == MODE_FIGHT && s->p[1 - winner].hp <= 0; n++)
		step2(s, 0, 0);
	if (s->mode == MODE_FIGHT && s->bout > 1)
		goto again;
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
		uint64_t beaten = s.beaten | 1ull << s.chr[1];
		win_round(&s, 0);
		assert(s.beaten == beaten && s.round == 2 * stage);
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
/* A full roster of MAX_CHARACTERS Terry copies: the arcade ladder meets
 * every other character once and the beaten mask, past bit 31, survives a
 * save midway. */
static void full_ladder(void)
{
	struct character *saved = malloc(sizeof(pack.chr));
	assert(saved);
	memcpy(saved, pack.chr, sizeof(pack.chr));
	unsigned characters = pack.characters;
	for (int k = 0; k < MAX_CHARACTERS; k++) {
		pack.chr[k] = saved[terry];
		pack.chr[k].name[0] = (char)('A' + k % 26);
		pack.chr[k].name[1] = (char)('A' + k / 26);
	}
	pack.characters = MAX_CHARACTERS;
	struct fight_state s, replay;
	uint32_t w[STATE_WORDS], v[STATE_WORDS];
	fight_reset(&s, 0);
	step2(&s, K(B), 0);
	assert(s.mode == MODE_FIGHT && s.chr[0] == 0 && s.stage == 1);
	for (unsigned stage = 1; stage < MAX_CHARACTERS; stage++) {
		assert(s.mode == MODE_FIGHT && s.stage == stage && s.chr[1] &&
		       !((s.beaten >> s.chr[1]) & 1));
		if (stage == 48) {
			assert(s.beaten >> 32);
			fight_encode(&s, w);
			assert(fight_decode(&replay, &pack, w));
			fight_encode(&replay, v);
			assert(!memcmp(w, v, sizeof(w)) && replay.beaten == s.beaten);
		}
		win_round(&s, 0);
	}
	assert(s.mode == MODE_CLEAR && s.beaten == ~1ull);
	memcpy(pack.chr, saved, sizeof(pack.chr));
	pack.characters = characters;
	free(saved);
}
/* Does scene a start with b's commands (the same draws, then more)? */
static int extends(const struct scene *a, const struct scene *b)
{
	if (a->count <= b->count)
		return 0;
	for (unsigned k = 0; k < b->count; k++) {
		const struct command *x = &a->cmd[k], *y = &b->cmd[k];
		if (x->op != y->op || x->x != y->x || x->y != y->y || x->w != y->w || x->h != y->h ||
		    x->image != y->image || x->color != y->color)
			return 0;
	}
	return 1;
}
/* READY then FIGHT: the rules hold through the versus card and READY, a
 * button held into FIGHT is no press but a fresh one is, ROUND_START sounds
 * once at FIGHT, the banner draws on top of the plain round's commands
 * until BANNER_END, and a state in READY with anything in motion is
 * rejected. */
static void round_start(void)
{
	static struct scene sc, plain;
	struct fight_state s, t;
	uint32_t w[STATE_WORDS];
	fight_reset(&s, 0);
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_FIGHT && s.timer == 0);
	for (int n = 0; n < INTRO_TICKS; n++)
		step2(&s, K(X) | K(RIGHT), 0);
	assert(s.timer == INTRO_TICKS && !s.sounds);
	fight_cast(&pack, &s);
	t = s;
	t.timer = BANNER_END;
	fight_scene(&t, &pack, &plain);
	fight_scene(&s, &pack, &sc);
	assert(extends(&sc, &plain));
	int x0 = s.p[0].x, x1 = s.p[1].x;
	for (int n = INTRO_TICKS; n < ROUND_TICKS; n++) {
		assert(!s.sounds);
		step2(&s, K(X) | K(RIGHT), 0);
		assert(s.p[0].x == x0 && s.p[1].x == x1 && !s.p[0].move && !s.p[1].move &&
		       !s.p[0].buffer && !s.p[1].plan);
	}
	assert(s.timer == ROUND_TICKS && s.sounds == 1 && s.sound[0].id == SOUND_ROUND_START);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	fight_scene(&s, &pack, &sc);
	t = s;
	t.timer = BANNER_END;
	fight_scene(&t, &pack, &plain);
	assert(extends(&sc, &plain));
	/* X is still held: no press; RIGHT walks. */
	step2(&s, K(X) | K(RIGHT), 0);
	assert(!s.p[0].move && s.p[0].x > x0 && s.timer == ROUND_TICKS + 1 && !s.sounds);
	step2(&s, 0, 0);
	step2(&s, K(X), 0);
	assert(s.p[0].move == STAND_HP + 1);
	while (s.timer < BANNER_END)
		step2(&s, 0, 0);
	step2(&s, 0, 0);
	assert(s.timer == BANNER_END);
	fight_scene(&s, &pack, &sc);
	t = s;
	t.timer = BANNER_END - 1;
	fight_scene(&t, &pack, &plain);
	assert(extends(&plain, &sc));
	/* Past the banner, or a READY state with a buffered press. */
	fight_encode(&s, w);
	w[9] = BANNER_END + 1;
	assert(!fight_decode(&t, &pack, w));
	fight_reset(&s, 0);
	step2(&s, K(Y), 0);
	for (int n = 0; n < INTRO_TICKS + 4; n++)
		step2(&s, 0, 0);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	w[19 + 17] = STAND_LP + 1; /* P1 buffer */
	assert(!fight_decode(&t, &pack, w));
}
/* The combo words in a saved state (COMBO_AT in fighter.c): combo, shown,
 * show and best, two each. */
#define COMBO_W (19 + 2 * FIGHTER_WORDS_T + 6 * MAX_PROJECTILES + 7 + 4 + 5 * SPARK_SLOTS + 1 + MAX_SOUND_EVENTS)
#define LIST_W (COMBO_W + 8)
#define LEVEL_W (LIST_W + 1)
#define THROWS_W (LEVEL_W + 1)
/* Combos: crouch LP chains keep P1 in hitstun, the count shows from the
 * second hit and stays COMBO_TICKS after P1 recovers; LP into a Power Wave
 * that lands after the hitstun is two single hits; guarded hits count
 * nothing; mid-combo states round-trip and bad combo words are rejected. */
static void combos(void)
{
	static struct scene with, without;
	struct fight_state s, t;
	uint32_t w[STATE_WORDS], bad[STATE_WORDS];
	unsigned top = 0;
	int saved = 0;
	duel(&s, 200, 250);
	for (int n = 1; n <= 40; n++) {
		step2(&s, K(DOWN) | (n % 2 ? K(Y) : 0), 0);
		if (s.combo[0] >= 2) {
			assert(s.shown[0] == s.combo[0] && s.show[0] == COMBO_TICKS &&
			       s.p[1].stun && !s.p[1].block);
			if (!saved++) {
				fight_encode(&s, w);
				assert(fight_decode(&t, &pack, w));
				fight_cast(&pack, &s);
				fight_scene(&s, &pack, &with);
				t.shown[0] = t.show[0] = 0;
				fight_scene(&t, &pack, &without);
				assert(with.count > without.count);
				static const struct {
					int at;
					uint32_t value;
				} cases[] = {{0, 3}, {2, 1}, {6, MAX_COMBO + 1}, {4, COMBO_TICKS - 1},
					     {1, 1}, {7, MAX_COMBO + 1}, {5, 1}, {3, 2}};
				for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
					memcpy(bad, w, sizeof(w));
					bad[COMBO_W + cases[k].at] = cases[k].value;
					if (fight_decode(&t, &pack, bad)) {
						fprintf(stderr, "bad combo case %zu accepted\n", k);
						abort();
					}
				}
			}
		}
		top = s.combo[0] > top ? s.combo[0] : top;
		assert(!s.combo[1] && !s.best[1]);
	}
	assert(top >= 2 && s.best[0] == top && s.hits >= top && saved);
	/* Once the second hit lands nothing more is pressed: the count holds
	 * until P1 leaves hitstun, then fades. */
	duel(&s, 200, 250);
	for (int n = 1; s.combo[0] < 2; n++) {
		assert(n <= 40);
		step2(&s, K(DOWN) | (n % 2 ? K(Y) : 0), 0);
	}
	while (s.combo[0]) {
		assert(s.shown[0] == s.combo[0] && s.show[0] == COMBO_TICKS);
		step2(&s, 0, 0);
	}
	assert(s.shown[0] >= 2 && s.show[0] == COMBO_TICKS && !s.p[1].stun);
	for (int j = 1; j <= COMBO_TICKS; j++) {
		step2(&s, 0, 0);
		assert(s.show[0] == (uint32_t)(COMBO_TICKS - j) && !s.shown[0] == (j == COMBO_TICKS));
	}
	assert(s.best[0] == top);
	/* LP, then the cancelled Power Wave hits after P1 left hitstun. */
	duel(&s, 200, 260);
	for (int n = 1; n <= 30; n++) {
		step2(&s, n == 1 ? K(Y) : motion(MOTION_236, n, 9, 1, K(Y)), 0);
		assert(s.combo[0] <= 1 && !s.shown[0]);
	}
	assert(s.hits == 2 && s.best[0] == 1);
	/* Guarded: nothing. */
	duel(&s, 200, 250);
	for (int n = 1; n <= 40; n++)
		step2(&s, K(DOWN) | (n % 2 ? K(Y) : 0), K(RIGHT) | K(DOWN));
	assert(s.guards >= 1 && s.hits == s.guards && !s.best[0] && !s.shown[0]);
	/* A new round starts from nothing. */
	fight_start(&s, &pack, terry, terry);
	assert(!s.best[0] && !s.combo[0] && !s.shown[0] && !s.show[0]);
}
/* Command lists from the pack: Terry's five specials, Iori's Aoi Hana
 * stages nested under each other, Haohmaru's slash buttons; every
 * character's list names every special once, in characters the HUD font
 * has, and its pause and select screens fit the command budget. START
 * toggles the select list (the cursor still moves) and pauses a fight;
 * the list flag round-trips and 2 is rejected. */
static void move_lists(void)
{
	static struct scene sc;
	char name[LIST_LINES][MAX_MOVE_NAME + 1], text[LIST_LINES][LIST_TEXT];
	uint8_t depth[LIST_LINES];
	assert(fight_move_list(&pack.chr[terry], name, text, depth) == 5);
	assert(!strcmp(text[0], "\2\3\6 + P  PROJECTILE") && !strcmp(text[1], "\2\1\4 + P") &&
	       !strcmp(text[2], "[\2]\10 + P") && !depth[0] && !depth[1] && !depth[2]);
	assert(!strcmp(name[0], "POWER WAVE") && !strcmp(name[1], "BURN KNUCKLE") &&
	       !strcmp(name[2], "RISING TACKLE") && !strcmp(name[3], "CRACK SHOOT") &&
	       !strcmp(name[4], "POWER DUNK"));
	unsigned iori = character("IORI");
	assert(fight_move_list(&pack.chr[iori], name, text, depth) == 5);
	assert(!strcmp(text[1], "\2\1\4 + P") && !depth[1] && !strcmp(text[2], "\2\1\4 + P") &&
	       depth[2] == 1 && !strcmp(text[3], "\2\1\4 + P  OVERHEAD") && depth[3] == 2 &&
	       !strcmp(text[4], "\6\2\3 + P") && !depth[4]);
	assert(!strcmp(name[2], "AOI HANA 2") && !strcmp(name[3], "AOI HANA 3"));
	unsigned n = fight_move_list(&pack.chr[haohmaru], name, text, depth);
	assert(n >= 1 && strstr(text[0], " + S"));
	static const char font[] = "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ:-/+[]>.\1\2\3\4\6\7\10\11";
	struct fight_state s;
	for (unsigned c = 0; c < pack.characters; c++) {
		const struct character *ch = &pack.chr[c];
		n = fight_move_list(ch, name, text, depth);
		unsigned listed = 0;
		for (unsigned i = 0; i < n; i++) {
			assert(text[i][0] && depth[i] <= 3 && strspn(text[i], font) == strlen(text[i]));
			assert(strspn(name[i], font) == strlen(name[i]) && strlen(name[i]) <= MAX_MOVE_NAME);
			assert(!i || depth[i] <= depth[i - 1] + 1);
		}
		for (unsigned k = MOVES; k < ch->moves; k++)
			listed++;
		for (unsigned k = 0; k < MOVES; k++)
			for (unsigned j = MOVES; j < ch->moves; j++)
				if (ch->move[j].after == k + 1) {
					listed++;
					break;
				}
		if (n != listed) {
			fprintf(stderr, "move list: %s lists %u of %u\n", ch->name, n, listed);
			abort();
		}
		/* Pause in a mirror fight, and the select list. */
		fight_reset(&s, 0);
		fight_start(&s, &pack, c, c);
		s.stage = 1;
		step2(&s, K(START), 0);
		assert(s.paused);
		fight_cast(&pack, &s);
		fight_scene(&s, &pack, &sc);
		assert(sc.count < COMMANDS);
		/* Every name, command and tag (glyph runs 2 px high, at most 10 wide) stays inside the
		 * pause panel. */
		for (unsigned i = 0; i < sc.count; i++)
			if (sc.cmd[i].op == RECT && sc.cmd[i].y >= 54 && sc.cmd[i].h == 2 && sc.cmd[i].w <= 10 &&
			    sc.cmd[i].x + sc.cmd[i].w > FW - 44) {
				fprintf(stderr, "move list: %s past the pause panel (%d %d %d %d)\n", ch->name, sc.cmd[i].x, sc.cmd[i].y, sc.cmd[i].w, sc.cmd[i].h);
				abort();
			}
		fight_reset(&s, 0);
		s.chr[0] = s.chr[1] = c;
		s.list = 1;
		fight_cast(&pack, &s);
		fight_scene(&s, &pack, &sc);
		assert(sc.count < COMMANDS);
		/* And inside the select screen's panel (196..472 x 32..312). */
		for (unsigned i = 0; i < sc.count; i++)
			if (sc.cmd[i].op == RECT && sc.cmd[i].x >= 196 && sc.cmd[i].h == 2 && sc.cmd[i].w <= 10 &&
			    (sc.cmd[i].x + sc.cmd[i].w > 472 || sc.cmd[i].y + sc.cmd[i].h > 312)) {
				fprintf(stderr, "move list: %s past the select panel\n", ch->name);
				abort();
			}
	}
	uint32_t w[STATE_WORDS];
	struct fight_state t;
	fight_reset(&s, 0);
	step2(&s, K(START), 0);
	assert(s.list && !s.paused && s.mode == MODE_SELECT);
	step2(&s, K(RIGHT), 0);
	assert(s.chr[0] == 1 % pack.characters && s.list);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w) && t.list);
	w[LIST_W] = 2;
	assert(!fight_decode(&t, &pack, w));
	step2(&s, K(START), 0);
	assert(!s.list && !s.paused);
	step2(&s, 0, 0);
	step2(&s, K(START), 0);
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_FIGHT && s.list && !s.paused);
	step2(&s, K(START), 0);
	assert(s.paused);
}
/* Ticks P1 (side 1) lies and gets up after landing (fighter.c's
 * lie_ticks + GETUP). */
static int down_total(const struct character *c)
{
	int g = c->action[GETUP].total;
	return (c->stats.wakeup > g ? c->stats.wakeup : g);
}
/* Knockdowns: which moves knock down (pack defaults), Terry's sweep on an
 * idle P1: FALLING through the fall animation, LYING (the down pose, then
 * the get-up) for down_total ticks, then up; nothing hits the downed
 * fighter (strikes, Power Wave), a press in the last get-up ticks comes out
 * on the first free tick, a mid-knockdown state round-trips and bad ones
 * are refused; Burn Knuckle knocks down, Power Wave does not, and every
 * character has its three knockdown actions. */
static void knockdowns(void)
{
	const struct character *t = &pack.chr[terry];
	assert(t->move[CROUCH_HK].hit.flags & FLAG_KNOCKDOWN);
	assert(!(t->move[STAND_HK].hit.flags & FLAG_KNOCKDOWN));
	assert(t->move[BURN_KNUCKLE].hit.flags & FLAG_KNOCKDOWN);
	assert(t->move[RISING_TACKLE].hit.flags & FLAG_KNOCKDOWN);
	assert(!(t->move[POWER_WAVE].hit.flags & FLAG_KNOCKDOWN) &&
	       !(t->projectile[0].hit.flags & FLAG_KNOCKDOWN));
	unsigned iori = character("IORI");
	const struct character *io = &pack.chr[iori];
	unsigned aoi = 0, aoi2 = 0, aoi3 = 0;
	for (unsigned k = MOVES; k < io->moves; k++)
		if (!io->move[k].after && io->move[k].motion == MOTION_214)
			aoi = k;
	for (unsigned k = MOVES; k < io->moves; k++) {
		if (io->move[k].after == aoi + 1)
			aoi2 = k;
		if (aoi2 && io->move[k].after == aoi2 + 1)
			aoi3 = k;
	}
	assert(aoi2 && aoi3 && !(io->move[aoi].hit.flags & FLAG_KNOCKDOWN) &&
	       !(io->move[aoi2].hit.flags & FLAG_KNOCKDOWN) &&
	       (io->move[aoi3].hit.flags & FLAG_KNOCKDOWN));
	for (unsigned c = 0; c < pack.characters; c++)
		for (int k = FALL; k <= GETUP; k++)
			assert(pack.chr[c].action[k].count && pack.chr[c].action[k].total);
	struct fight_state s, u;
	uint32_t w[STATE_WORDS];
	duel(&s, 200, 250);
	int fall = t->action[FALL].total, total = down_total(t), n = 0;
	while (!s.p[1].down && n < 40)
		step2(&s, n++ ? K(DOWN) : K(DOWN) | K(A), 0);
	assert(s.p[1].down == FALLING && !s.p[1].stun && s.p[1].hp < 1000 && s.hits == 1);
	int hp = s.p[1].hp, fell = 0, lay = 0, rose = 0;
	/* Freeze through the hitstop, then fall, lie and get up while P0 jabs
	 * and throws Power Waves at it. */
	for (n = 0; s.p[1].down && n < 400; n++) {
		int x = s.p[1].down;
		uint32_t k0 = n % 30 == 5 ? K(Y) : n % 60 == 20 ? K(DOWN) : n % 60 == 22 ? K(DOWN) | K(RIGHT)
		       : n % 60 == 24 ? K(RIGHT) | K(Y) : 0;
		/* P1 presses LP in the last ticks of its get-up (a reversal). */
		uint32_t k1 = x == LYING && s.p[1].elapsed == total - 2 ? K(Y) : 0;
		step2(&s, k0, k1);
		assert(s.p[1].hp == hp);
		if (s.p[1].down == FALLING) {
			fell++;
			assert(in_action(&s, 1, FALL));
		} else if (s.p[1].down == LYING) {
			assert(s.p[1].y == 0 && s.p[1].elapsed < total);
			if (s.p[1].elapsed < total - t->action[GETUP].total)
				assert(in_action(&s, 1, DOWN));
			else
				assert(in_action(&s, 1, GETUP));
			lay++;
			if (lay == 10) {
				fight_encode(&s, w);
				assert(fight_decode(&u, &pack, w));
				uint32_t v[STATE_WORDS];
				fight_encode(&u, v);
				assert(!memcmp(w, v, sizeof(w)));
			}
		}
		rose = !s.p[1].down;
	}
	assert(rose && fell >= fall && lay == total && s.p[1].move == STAND_LP + 1 &&
	       s.p[1].elapsed == 0);
	/* Bad knockdown words: a phase 3, down with stun, lying in the air or
	 * past its end. */
	const int D = 19 + FIGHTER_WORDS_T;
	static const struct {
		int field;
		uint32_t value;
	} bad[] = {{22, 3}, {11, 5}, {1, (uint32_t)-10}, {9, 400}};
	fight_encode(&u, w);
	assert(u.p[1].down == LYING);
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		uint32_t v[STATE_WORDS];
		memcpy(v, w, sizeof(v));
		v[D + bad[i].field] = bad[i].value;
		struct fight_state keep = u;
		assert(!fight_decode(&u, &pack, v) && !memcmp(&keep, &u, sizeof(u)));
	}
	/* Burn Knuckle knocks down; a Power Wave only stuns. */
	duel(&s, 200, 280);
	for (n = 1; n <= 40 && !s.p[1].down; n++)
		step2(&s, motion(MOTION_214, n, 5, 1, K(Y)), 0);
	assert(s.p[1].down == FALLING);
	duel(&s, 100, 330);
	for (n = 1; n <= 120 && s.hits == 0; n++)
		step2(&s, motion(MOTION_236, n, 5, 1, K(Y)), 0);
	assert(s.hits == 1 && !s.p[1].down && s.p[1].stun);
	/* AI vs AI on Normal: knockdowns happen, a downed fighter never loses
	 * life (no OTG moves in the pack) and some hits land within a few
	 * ticks of a get-up (the AI's meaty). */
	fight_reset(&s, 1);
	unsigned downs = 0, meaty = 0, since[2] = {99, 99};
	for (n = 0; n < 30000; n++) {
		struct fight_state before = s;
		fight_step(&s, &pack, 0, 0, 0);
		if (s.mode != MODE_FIGHT || before.mode != MODE_FIGHT || s.round != before.round)
			continue;
		for (int i = 0; i < 2; i++) {
			downs += s.p[i].down == FALLING && !before.p[i].down;
			if (before.p[i].down && !s.p[i].down)
				since[i] = 0;
			else if (since[i] < 99)
				since[i]++;
			/* (A fighter can rise and be swept down again on one tick.) */
			if (before.p[i].down && s.p[i].down &&
			    !(before.p[i].down == LYING && s.p[i].down == FALLING))
				assert(s.p[i].hp == before.p[i].hp);
			meaty += s.p[i].hp < before.p[i].hp && since[i] <= 4;
		}
	}
	printf("knockdowns: %u in 30000 AI ticks, %u hits within 4 ticks of a get-up\n", downs,
	       meaty);
	assert(downs > 20 && meaty > 0);
}
/* v * factor / 1000 rounded half up, at least 1 when v is nonzero (as
 * fighter.c). */
static int per_mille(int v, int factor)
{
	int r = (v * factor + 500) / 1000;
	return v && !r ? 1 : r;
}
/* The game mode words in a saved state (GAME_AT in fighter.c): game,
 * picking, dummy, streak, record, carry, damage and last per side. */
#define GAME_W (COMBO_W + 14)
/* One tick with port 1 left to the rules (the CPU or the practice dummy). */
static void solo(struct fight_state *s, uint32_t k) { fight_step(s, &pack, k, 0, 0); }
/* Game modes. L cycles them on the select screen (the box overlay stays).
 * Survival: random opponents in a row, never the same twice, P1's life
 * carried with SURVIVAL_HEAL back per win (capped at full), the streak and
 * the session record on the result screen and kept across runs. Practice:
 * P1 and then the dummy picked (both cast), the dummy standing, crouching,
 * guarding high and low or the CPU (R), the life bar never emptied and
 * refilled once a combo ends, the last combo's damage shown. B in a pause
 * quits to select. Each state round-trips and impossible game words are
 * rejected. */
static void game_modes(void)
{
	static struct scene sc;
	struct fight_state s, t;
	uint32_t w[STATE_WORDS], v[STATE_WORDS];
	const int life = pack.chr[terry].stats.life;
	fight_reset(&s, 0);
	for (unsigned k = 1; k <= GAMES; k++) {
		step2(&s, K(L), 0);
		step2(&s, 0, 0);
		assert(s.game == k % GAMES && !s.boxes && s.mode == MODE_SELECT);
		fight_scene(&s, &pack, &sc);
	}
	/* Survival from Terry. */
	step2(&s, K(L), 0);
	s.chr[0] = s.chr[1] = terry;
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_FIGHT && s.game == GAME_SURVIVAL && !s.streak && !s.record &&
	       s.carry == (uint32_t)life && s.p[0].hp == life && s.chr[1] != terry &&
	       s.stage == 1 && !s.beaten && !s.timer);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	fight_encode(&t, v);
	assert(!memcmp(w, v, sizeof(w)));
	unsigned previous = s.chr[1];
	intro(&s);
	fight_scene(&s, &pack, &sc);
	s.p[0].hp = 300;
	win_round(&s, 0);
	int healed = 300 + life * SURVIVAL_HEAL / 1000;
	assert(s.mode == MODE_FIGHT && s.streak == 1 && s.record == 1 &&
	       s.carry == (uint32_t)healed && s.p[0].hp == healed && s.chr[1] != previous &&
	       s.chr[1] != terry && !s.timer && s.round == 1);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	/* A win at almost full life heals up to full only. */
	s.p[0].hp = life - 10;
	win_round(&s, 0);
	assert(s.streak == 2 && s.carry == (uint32_t)life && s.p[0].hp == life);
	win_round(&s, 1);
	assert(s.mode == MODE_RESULT && s.streak == 2 && s.record == 2 && !s.carry);
	fight_scene(&s, &pack, &sc);
	for (int n = 1; n <= 21; n++)
		step2(&s, n == 21 ? K(A) : 0, 0);
	assert(s.mode == MODE_SELECT && s.game == GAME_SURVIVAL && s.record == 2);
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_FIGHT && !s.streak && s.record == 2);
	/* Bad survival words: carry above P1's life or under its life now,
	 * a streak past the record. */
	fight_encode(&s, w);
	static const struct {
		int at;
		uint32_t value;
	} bad_survival[] = {{5, 5000}, {5, 1}, {3, 3}, {5, 0}};
	for (size_t i = 0; i < sizeof(bad_survival) / sizeof(bad_survival[0]); i++) {
		memcpy(v, w, sizeof(v));
		v[GAME_W + bad_survival[i].at] = bad_survival[i].value;
		assert(!fight_decode(&t, &pack, v));
	}
	/* B in a pause quits to select. */
	step2(&s, K(START), 0);
	step2(&s, K(B), 0);
	assert(s.mode == MODE_SELECT && !s.paused && !s.carry);
	/* Practice: P1, then the dummy (both sides wanted by the cache). */
	step2(&s, K(L), 0);
	assert(s.game == GAME_PRACTICE);
	s.chr[0] = s.chr[1] = terry;
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_SELECT && s.picking &&
	       s.chr[1] == (terry + 1) % pack.characters && s.chr[0] == terry);
	fight_want(&pack, &s);
	assert(pack.wanted == 3);
	fight_scene(&s, &pack, &sc);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	step2(&s, K(RIGHT), 0);
	assert(s.chr[1] == (terry + 2) % pack.characters && s.chr[0] == terry);
	/* L goes back to the arcade and to picking P1. */
	step2(&s, K(L), 0);
	assert(!s.picking && s.game == GAME_ARCADE && s.chr[1] == terry);
	step2(&s, 0, 0);
	step2(&s, K(L), 0);
	step2(&s, 0, 0);
	step2(&s, K(L), 0);
	step2(&s, K(Y), 0);
	s.chr[1] = terry;
	s.pal[1] = 1;
	step2(&s, 0, 0);
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_FIGHT && s.game == GAME_PRACTICE && s.chr[1] == terry &&
	       s.pal[1] == 1 && !s.picking && s.stage == 1 && s.dummy == DUMMY_STAND);
	intro(&s);
	/* A standing dummy takes a jab; the damage shows, the bar refills when
	 * the combo ends. */
	s.p[0].x = QX(200);
	s.p[1].x = QX(250);
	int n = 0;
	unsigned hits = s.hits, guards = s.guards;
	solo(&s, K(Y));
	while (s.hits == hits && n++ < 30)
		solo(&s, 0);
	assert(s.hits == hits + 1 && s.guards == guards && s.p[1].hp < life && s.last[0] == (uint32_t)(life - s.p[1].hp));
	uint32_t dealt = s.last[0];
	fight_scene(&s, &pack, &sc);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	while (s.p[1].stun && n++ < 100)
		solo(&s, 0);
	assert(s.p[1].hp == life && s.last[0] == dealt && !s.damage[0]);
	/* Nearly empty: the bar stops at 1 and nothing ends. */
	s.p[1].hp = 2;
	solo(&s, K(X));
	for (n = 0; n < 40 && s.hits < hits + 2; n++)
		solo(&s, 0);
	assert(s.hits == hits + 2 && s.p[1].hp >= 1 && !s.ended && s.last[0] > 1);
	fight_encode(&s, w);
	memcpy(v, w, sizeof(v));
	v[19 + FIGHTER_WORDS_T + 3] = 0; /* the dummy's life */
	assert(!fight_decode(&t, &pack, v));
	for (n = 0; n < 120; n++)
		solo(&s, 0);
	assert(s.p[1].hp == life && s.p[0].hp == life);
	/* R: crouching. */
	solo(&s, K(R));
	solo(&s, 0);
	assert(s.dummy == DUMMY_CROUCH && s.p[1].crouch);
	/* R: guarding, standing for a high and crouching for a low. */
	solo(&s, K(R));
	assert(s.dummy == DUMMY_GUARD);
	for (n = 0; n < 30; n++)
		solo(&s, 0);
	s.p[0].x = QX(200);
	s.p[1].x = QX(250);
	guards = s.guards;
	solo(&s, K(Y));
	for (n = 0; n < 30 && s.guards == guards; n++)
		solo(&s, 0);
	assert(s.guards == guards + 1 && !s.p[1].crouch && s.p[1].block == 1);
	for (n = 0; n < 40; n++)
		solo(&s, 0);
	s.p[0].x = QX(200);
	s.p[1].x = QX(250);
	guards = s.guards;
	hits = s.hits;
	solo(&s, K(DOWN) | K(B));
	for (n = 0; n < 30 && s.guards == guards; n++)
		solo(&s, K(DOWN));
	assert(s.guards == guards + 1 && s.hits == hits + 1 && s.p[1].crouch && s.p[1].block == 1);
	/* R: the CPU fights back; R again: standing. */
	solo(&s, K(R));
	assert(s.dummy == DUMMY_CPU);
	for (n = 0; n < 900; n++) {
		solo(&s, n % 7 ? 0 : K(Y));
		assert(s.mode == MODE_FIGHT && s.p[0].hp >= 1 && s.p[1].hp >= 1 && !s.ended);
	}
	solo(&s, K(R));
	assert(s.dummy == DUMMY_STAND);
	fight_scene(&s, &pack, &sc);
	/* Bad game words. */
	fight_encode(&s, w);
	static const struct {
		int at;
		uint32_t value;
	} bad[] = {{0, GAMES}, {1, 1}, {2, DUMMIES}, {4, MAX_STREAK + 1}, {5, 100}, {6, 5}};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		memcpy(v, w, sizeof(v));
		v[GAME_W + bad[i].at] = bad[i].value;
		struct fight_state keep = t;
		assert(!fight_decode(&t, &pack, v) && !memcmp(&keep, &t, sizeof(t)));
	}
	step2(&s, K(START), 0);
	step2(&s, K(B), 0);
	assert(s.mode == MODE_SELECT && s.game == GAME_PRACTICE && !s.picking);
}
/* The guard-all dummy guards every normal in its guard range at any
 * pace: letting go of back between swings and holding it again is no 44
 * backdash. */
static void guard_dummy(void)
{
	static const uint32_t press[4] = {K(Y), K(X), K(B), K(A)};
	struct fight_state s;
	for (unsigned c = 0; c < pack.characters; c++)
		for (int period = 6; period < 40; period += 3)
			for (int b = 0; b < 4; b++) {
				match(&s, c, c, 200, 250);
				s.game = GAME_PRACTICE;
				s.dummy = DUMMY_GUARD;
				unsigned missed = 0;
				for (int n = 0; n < 300; n++) {
					int near = abs(s.p[1].x - s.p[0].x) / 4 < pack.chr[c].stats.guard_range;
					unsigned open = s.hits - s.guards;
					solo(&s, n % period ? 0 : press[b]);
					missed += near && s.hits - s.guards != open;
				}
				if (s.backdashes || missed) {
					fprintf(stderr, "guard dummy %s every %d ticks: %u backdashes, %u hits\n",
						pack.chr[c].name, period, s.backdashes, missed);
					abort();
				}
			}
}
/* A Hard CPU that guards a swing (the AI's per-swing roll, 28 of 32)
 * never backdashes from it, however soon its last guard let go of back. */
static void cpu_guard(void)
{
	struct fight_state s;
	unsigned bad = 0, dashes = 0;
	for (unsigned c = 0; c < pack.characters; c++)
		for (int period = 8; period < 32; period += 4) {
			match(&s, c, c, 200, 250);
			s.level = LEVEL_HARD;
			for (int n = 0; n < 400; n++) {
				struct fight_state before = s;
				if (n % period == 0 && !s.p[0].move && !s.p[1].stun) {
					s.p[0].x = QX(200);
					s.p[1].x = QX(250);
				}
				fight_step(&s, &pack, n % period ? 0 : K(Y), 0, 0);
				const struct fighter *q = &before.p[0];
				if (s.p[1].dash != DASH_BACK || before.p[1].dash == DASH_BACK || !q->move)
					continue;
				dashes++;
				uint32_t h = ((before.tick + 1 - (uint32_t)q->elapsed) * 2654435761u + 1u) >> 27;
				bad += abs(q->x / 4 - before.p[1].x / 4) < pack.chr[c].stats.guard_range &&
				       h < 28;
			}
		}
	if (bad) {
		fprintf(stderr, "cpu guard: %u of %u backdashes from guarded swings\n", bad, dashes);
		abort();
	}
}
/* Difficulty: the level round-trips and 3 is rejected; on Easy every CPU
 * strike on an idle P1 deals 3/4 of its damage (human port 1 or Normal:
 * full); AI vs AI runs per level guard more and start more specials from
 * Easy to Hard, and only Hard cancels its normals into specials. */
static void difficulty(void)
{
	struct fight_state s, t;
	uint32_t w[STATE_WORDS];
	fight_reset(&s, 0);
	assert(s.level == LEVEL_NORMAL);
	s.level = LEVEL_HARD;
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w) && t.level == LEVEL_HARD);
	w[LEVEL_W] = LEVELS;
	assert(!fight_decode(&t, &pack, w));
	/* R on the select screen cycles Easy, Normal, Hard (a held R once);
	 * paused or in a fight it changes the stress scene instead. */
	fight_reset(&s, 0);
	static const uint32_t cycle[] = {LEVEL_HARD, LEVEL_EASY, LEVEL_NORMAL};
	for (int k = 0; k < 3; k++) {
		fight_step(&s, &pack, K(R), 0, 0);
		fight_step(&s, &pack, K(R), 0, 0);
		assert(s.level == cycle[k] && s.stress == 0 && s.mode == MODE_SELECT);
		fight_step(&s, &pack, 0, 0, 0);
	}
	fight_step(&s, &pack, K(START), 0, 0);
	fight_step(&s, &pack, 0, 0, 0);
	assert(s.list);
	fight_step(&s, &pack, K(R), 0, 0);
	fight_step(&s, &pack, 0, 0, 0);
	assert(s.level == LEVEL_HARD);
	fight_step(&s, &pack, K(Y), 0, 0);
	fight_step(&s, &pack, K(R), 0, 0);
	assert(s.mode == MODE_FIGHT && s.level == LEVEL_HARD && s.stress == 1);
	struct scene *sc = malloc(sizeof(*sc));
	assert(sc);
	fight_reset(&s, 0);
	s.level = LEVEL_HARD;
	fight_scene(&s, &pack, sc);
	struct fight_state u = s;
	u.level = LEVEL_EASY;
	struct scene *other = malloc(sizeof(*other));
	assert(other);
	fight_scene(&u, &pack, other);
	assert(sc->count != other->count || memcmp(sc->cmd, other->cmd, sc->count * sizeof(sc->cmd[0])));
	free(sc);
	free(other);
	for (int level = LEVEL_EASY; level <= LEVEL_NORMAL; level++) {
		unsigned checked = 0;
		fight_reset(&s, 0);
		s.level = (uint32_t)level;
		fight_start(&s, &pack, terry, terry);
		s.stage = 1;
		for (int n = 0; n < 6000 && !s.ended && checked < 5; n++) {
			struct fight_state before = s;
			fight_step(&s, &pack, 0, 0, 0);
			if (s.p[0].hp >= before.p[0].hp || live_shots(&before) || live_shots(&s) ||
			    !s.p[1].move)
				continue;
			const struct move *m = &pack.chr[terry].move[s.p[1].move - 1];
			if (m->spawn) /* a projectile launched and hit this tick */
				continue;
			int scale = level == LEVEL_EASY ? 750 : 1000;
			assert(before.p[0].hp - s.p[0].hp ==
			       per_mille(per_mille(m->hit.damage, scale), pack.chr[terry].stats.defense));
			checked++;
		}
		assert(checked == 5);
	}
	unsigned guards[LEVELS], specials[LEVELS], cancels[LEVELS];
	for (int level = 0; level < LEVELS; level++) {
		fight_reset(&s, 1);
		s.level = (uint32_t)level;
		cancels[level] = 0;
		for (int n = 0; n < 30000; n++) {
			struct fight_state before = s;
			fight_step(&s, &pack, 0, 0, 0);
			for (int i = 0; i < 2 && s.mode == MODE_FIGHT && before.mode == MODE_FIGHT; i++)
				cancels[level] += before.p[i].move && before.p[i].move <= MOVES &&
						  s.p[i].move > MOVES;
		}
		assert(s.level == (uint32_t)level);
		guards[level] = s.guards;
		specials[level] = s.specials;
		printf("difficulty %d: guards %u of %u contacts, specials %u, cancels %u\n", level,
		       s.guards, s.hits, s.specials, cancels[level]);
	}
	assert(guards[LEVEL_EASY] < guards[LEVEL_NORMAL] && guards[LEVEL_NORMAL] < guards[LEVEL_HARD]);
	assert(specials[LEVEL_EASY] < specials[LEVEL_NORMAL] &&
	       specials[LEVEL_NORMAL] < specials[LEVEL_HARD]);
	assert(cancels[LEVEL_HARD] > 0 && cancels[LEVEL_HARD] > cancels[LEVEL_NORMAL] &&
	       cancels[LEVEL_HARD] > cancels[LEVEL_EASY]);
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
	       W_PLAN, W_PLAN_TICK, W_CLOCK, W_DOWN, W_GRAB, W_SAFE, W_CHARGE };
	enum { G_MODE = 8, G_TIMER, G_BEATEN, G_STAGE, G_RUN, G_WIN0, G_WIN1, G_CHR0, G_CHR1,
	       G_PAL0, G_PAL1 };
	const int P = 19, S = 19 + 2 * FIGHTER_WORDS_T;
	int shot = S + 6 * (int)(shot_of(&s, 1) - s.shot);
	static const struct {
		int at, who, field;
		uint32_t value;
	} cases[] = {
	    {0, 1, W_MOVE, MOVES + 6},	   {0, 1, W_MOVE, 1},	       {0, 1, W_FREEZE, MAX_HITSTOP + 1},
	    {0, 1, W_STUN, MAX_STUN + 1},  {0, 1, W_SLIDE, (uint32_t)-MAX_SLIDE - 1},
	    {0, 1, W_SLIDE, MAX_SLIDE + 1},  {0, 1, W_KEYS, 0x10000},	   {0, 1, W_CROUCH, 2},	       {0, 1, W_BLOCK, 3},
	    {0, 0, W_ELAPSED, 100},	   {0, 0, W_HIT, 2},	       {0, 0, W_STUN, 5},
	    {0, 0, W_BLOCK, 1},	   {0, 1, W_HIT, 1},	       {0, 1, W_ELAPSED, 256},
	    {0, 0, W_X, QX(24) - 1},	   {0, 0, W_FACE, 0},	       {0, 1, W_HP, 1001},
	    {0, 1, W_X, QX(FW - 24) + 1},
	    {0, 0, W_WALK, 2},		   {0, 1, W_VY, 61},	       {0, 0, W_Y, (uint32_t)-30},
	    {0, 0, W_VY, 3},		   {0, 1, W_CORNER, 1},	       {0, 1, W_SQUAT, 3 + 1},
	    {0, 0, W_SQUAT, 1},	   {0, 0, W_BUFFER, MOVES + 6}, {0, 0, W_AGE, 4 + 1},
	    {0, 1, W_AGE, 1},		   {0, 1, W_PLAN, STAND_HK + 1}, {0, 1, W_PLAN, MOVES + 6},
	    {0, 1, W_PLAN_TICK, 1},	   {0, 1, W_CHARGE, (10 + 1) << 8},
	    {0, 1, W_CHARGE, 1 | 1 << 8}, {0, 0, W_CLOCK, 256},
	    {0, 0, W_GRAB, GRAB_TECH},	   {0, 0, W_GRAB, GRAB_HELD},  {0, 1, W_GRAB, GRAB_FORWARD},
	    {0, 0, W_SAFE, MAX_THROW_SAFE + 1}, {0, 0, W_SAFE, (uint32_t)-1},
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
	    {2, 0, 2, 2 * FW + 65},	   {2, 0, 2, (uint32_t)(-FW - 65)},
	    {2, 0, 0, 0},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		memcpy(bad, w, sizeof(w));
		int at = cases[i].at == 0 ? P + cases[i].who * FIGHTER_WORDS_T + cases[i].field
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
	bad[P + FIGHTER_WORDS_T + W_CROUCH] = 1;
	bad[P + FIGHTER_WORDS_T + W_Y] = (uint32_t)-10;
	assert(!fight_decode(&t, &pack, bad));
}
/* ---- camera, walls, sparks and sound events on the pack's stages ---- */
static unsigned stage_index(const char *name)
{
	for (unsigned k = 0; k < pack.stages; k++)
		if (!strcmp(pack.stage[k].name, name))
			return k;
	abort();
}
static int heard(const struct fight_state *s, int id, int side)
{
	int n = 0;
	for (unsigned k = 0; k < s->sounds; k++)
		n += s->sound[k].id == id && (side < 0 || s->sound[k].side == side);
	return n;
}
/* Run the clock out now: the next tick is time over. */
static void time_over(struct fight_state *s)
{
	s->clock = 1;
	step2(s, 0, 0);
	assert(s->ended == 1 && !s->clock && s->p[0].hp > 0 && s->p[1].hp > 0);
}
/* The KO or time over plays out into the next round. */
static void round_end(struct fight_state *s)
{
	for (int n = 0; n < 181 && s->ended; n++)
		step2(s, 0, 0);
	assert(!s->ended);
}
/* The round clock and best of three. The clock is full through the versus
 * card and READY, counts down from FIGHT, holds in a pause and runs out
 * into time over: the larger share of life wins (the loser's KO sound),
 * equal shares are a draw (no sound). An arcade match takes two rounds,
 * the next round starts at READY (ROUND N) with full life and the clock
 * full, a draw scores for both and two at once is a loss. Survival decides
 * a time over by life too (a draw replays the fight from the carried
 * life); practice has no clock. Every state round-trips and an impossible
 * clock or bout is rejected. */
static void round_clock(void)
{
	static struct scene sc;
	struct fight_state s, t;
	uint32_t w[STATE_WORDS], v[STATE_WORDS];
	fight_reset(&s, 0);
	s.chr[0] = s.chr[1] = terry;
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_FIGHT && s.game == GAME_ARCADE && s.clock == CLOCK_TICKS &&
	       s.bout == 1 && !s.wins[0] && !s.wins[1]);
	/* Bad clocks before FIGHT: anything but full. */
	fight_encode(&s, w);
	memcpy(v, w, sizeof(v));
	v[GAME_W + 10] = CLOCK_TICKS - 1;
	assert(!fight_decode(&t, &pack, v));
	intro(&s);
	assert(s.clock == CLOCK_TICKS - 1);
	for (int n = 0; n < 59; n++)
		step2(&s, 0, 0);
	assert(s.clock == CLOCK_TICKS - COUNT_TICKS && !s.ended);
	fight_scene(&s, &pack, &sc);
	/* A pause holds the clock. */
	step2(&s, K(START), 0);
	for (int n = 0; n < 30; n++)
		step2(&s, 0, 0);
	assert(s.paused && s.clock == CLOCK_TICKS - COUNT_TICKS);
	step2(&s, K(START), 0);
	assert(!s.paused);
	/* Bad running words: the clock past full, no bout, a fourth bout, a
	 * win in the first. */
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	static const struct {
		int at;
		uint32_t value;
	} bad[] = {{GAME_W + 10, CLOCK_TICKS + 1}, {GAME_W + 11, 0}, {GAME_W + 11, 4}, {13, 1},
		   {14, 1}};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		memcpy(v, w, sizeof(v));
		v[bad[i].at] = bad[i].value;
		struct fight_state keep = t;
		assert(!fight_decode(&t, &pack, v) && !memcmp(&keep, &t, sizeof(t)));
	}
	/* Round 1: a draw on time, a point each. */
	const int life0 = pack.chr[s.chr[0]].stats.life, life1 = pack.chr[s.chr[1]].stats.life;
	time_over(&s);
	assert(!heard(&s, SOUND_KO, -1));
	fight_scene(&s, &pack, &sc);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	round_end(&s);
	assert(s.mode == MODE_FIGHT && s.wins[0] == 1 && s.wins[1] == 1 && s.bout == 2 &&
	       s.timer == INTRO_TICKS && s.clock == CLOCK_TICKS && s.p[0].hp == life0 &&
	       s.p[1].hp == life1 && s.stage == 1);
	fight_scene(&s, &pack, &sc);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	/* Round 2: P1 ahead on life share when time runs out takes the match. */
	unsigned first = s.chr[1];
	intro(&s);
	s.p[0].hp = life0 * 3 / 4;
	s.p[1].hp = life1 / 2;
	time_over(&s);
	assert(heard(&s, SOUND_KO, 1) == 1);
	round_end(&s);
	assert(s.mode == MODE_FIGHT && s.stage == 2 && s.beaten == 1ull << first && s.bout == 1 &&
	       !s.wins[0] && !s.wins[1] && s.timer == 0 && s.clock == CLOCK_TICKS);
	/* Stage 2: two draws, two points each at once, are a loss. */
	intro(&s);
	time_over(&s);
	round_end(&s);
	assert(s.wins[0] == 1 && s.wins[1] == 1 && s.bout == 2);
	intro(&s);
	time_over(&s);
	round_end(&s);
	assert(s.mode == MODE_RESULT && !s.clock && !s.bout && !s.wins[0] && !s.wins[1]);
	fight_encode(&s, w);
	memcpy(v, w, sizeof(v));
	v[GAME_W + 11] = 1;
	assert(!fight_decode(&t, &pack, v));
	memcpy(v, w, sizeof(v));
	v[GAME_W + 10] = 1;
	assert(!fight_decode(&t, &pack, v));
	/* A KO in a round of a match: P2 takes a round, the next starts. */
	for (int n = 1; n <= 21; n++)
		step2(&s, n == 21 ? K(A) : 0, 0);
	assert(s.mode == MODE_SELECT);
	step2(&s, K(Y), 0);
	intro(&s);
	s.p[0].x = QX(200);
	s.p[1].x = QX(260);
	s.p[0].hp = 10;
	step2(&s, 0, K(Y));
	for (int n = 0; n < 60 && !s.ended; n++)
		step2(&s, 0, 0);
	assert(s.ended && !s.p[0].hp && s.clock);
	round_end(&s);
	assert(s.mode == MODE_FIGHT && !s.wins[0] && s.wins[1] == 1 && s.bout == 2);
	/* Survival: one round each, decided by life on time; a draw replays
	 * from the carried life. */
	step2(&s, K(START), 0);
	step2(&s, K(B), 0);
	step2(&s, K(L), 0);
	step2(&s, 0, 0);
	s.chr[0] = s.chr[1] = terry;
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_FIGHT && s.game == GAME_SURVIVAL && s.clock == CLOCK_TICKS);
	const int life = pack.chr[terry].stats.life;
	intro(&s);
	s.p[0].hp = life - 300;
	s.p[1].hp = pack.chr[s.chr[1]].stats.life / 2;
	time_over(&s);
	round_end(&s);
	int carried = life - 300 + life * SURVIVAL_HEAL / 1000;
	carried = carried < life ? carried : life;
	assert(s.mode == MODE_FIGHT && s.streak == 1 && s.carry == (uint32_t)carried &&
	       s.bout == 1 && !s.wins[0] && !s.wins[1] && s.clock == CLOCK_TICKS);
	unsigned opponent = s.chr[1];
	intro(&s);
	s.p[1].hp = (int32_t)((int64_t)s.p[0].hp * pack.chr[s.chr[1]].stats.life / life);
	if ((int64_t)s.p[1].hp * life == (int64_t)s.p[0].hp * pack.chr[s.chr[1]].stats.life) {
		time_over(&s);
		round_end(&s);
		assert(s.mode == MODE_FIGHT && s.streak == 1 && s.chr[1] == opponent &&
		       s.p[0].hp == carried && s.timer == 0);
		intro(&s);
	}
	s.p[0].hp = 100;
	time_over(&s);
	round_end(&s);
	assert(s.mode == MODE_RESULT && s.streak == 1);
	/* Practice: no clock. */
	for (int n = 1; n <= 21; n++)
		step2(&s, n == 21 ? K(A) : 0, 0);
	step2(&s, K(L), 0);
	step2(&s, K(Y), 0);
	step2(&s, 0, 0);
	step2(&s, K(Y), 0);
	assert(s.mode == MODE_FIGHT && s.game == GAME_PRACTICE && !s.clock && s.bout == 1);
	intro(&s);
	for (int n = 0; n < 100; n++)
		step2(&s, 0, 0);
	assert(!s.clock && !s.ended);
	fight_scene(&s, &pack, &sc);
	fight_encode(&s, w);
	memcpy(v, w, sizeof(v));
	v[GAME_W + 10] = 5;
	assert(!fight_decode(&t, &pack, v));
}
static const struct sound_event *event(const struct fight_state *s, int id)
{
	for (unsigned k = 0; k < s->sounds; k++)
		if (s->sound[k].id == id)
			return &s->sound[k];
	return NULL;
}
/* Both fighters inside the camera's walls, the walls inside the screen and
 * the camera inside the stage. */
static void on_screen(const struct fight_state *s)
{
	int lo, hi;
	fight_walls(s, &pack, &lo, &hi);
	assert(s->cam <= (uint32_t)(fight_width(s, &pack) - FW));
	assert(lo >= (int)s->cam && hi <= (int)s->cam + FW);
	for (int i = 0; i < 2; i++)
		assert(s->p[i].x >= QX(lo) && s->p[i].x <= QX(hi));
}
/* Characters c0 and c1 at world columns x0 and x1 on stage k, the camera on
 * their midpoint as the rules leave it; the state must decode. */
static void staged(struct fight_state *s, unsigned k, unsigned c0, unsigned c1, int x0, int x1)
{
	match(s, c0, c1, x0, x1);
	s->arena = k;
	s->cam = (uint32_t)clampi((x0 + x1) / 2 - FW / 2, 0, fight_width(s, &pack) - FW);
	uint32_t w[STATE_WORDS];
	fight_encode(s, w);
	assert(fight_decode(s, &pack, w));
}
/* Strength tier of a hit as the sound and spark tables use it. */
static int tier_of(const struct hit *h, int k)
{
	if (h->cls == CLASS_LIGHT)
		return 0;
	if (h->cls == CLASS_HEAVY || h->cls == CLASS_COMMAND || h->cls == CLASS_THROW)
		return 1;
	if (h->cls == CLASS_SPECIAL || h->cls == CLASS_SUPER)
		return 2;
	return k < 0 || k >= MOVES ? 2 : k % 2;
}
/* The ladder's stage, the start camera, walking apart into the screen wall
 * (or the stage wall), the stage ends, corners at both stage ends, a
 * pushback transfer at a screen wall and projectiles leaving the camera's
 * window. */
static void camera_tests(void)
{
	struct fight_state s;
	for (unsigned c = 0; c < pack.characters; c++) {
		fight_reset(&s, 0);
		fight_start(&s, &pack, terry, c);
		unsigned want = pack.chr[c].home ? pack.chr[c].home - 1u : c % pack.stages;
		int w = pack.stage[want].width;
		assert(s.arena == want && fight_width(&s, &pack) == w &&
		       s.cam == (uint32_t)(w / 2 - FW / 2) && s.p[0].x == QX(w / 2 - 100) &&
		       s.p[1].x == QX(w / 2 + 100));
		assert(s.sounds == 1 && s.sound[0].id == SOUND_ROUND_START);
		on_screen(&s);
	}
	unsigned korea = stage_index("KOREA"), mizuki = stage_index("MIZUKI");
	assert(pack.chr[haohmaru].home == mizuki + 1);
	for (unsigned k = 0; k < pack.stages; k++) {
		const struct stage *st = &pack.stage[k];
		struct system *sy = &pack.sys[st->system];
		uint16_t saved = sy->play_width;
		for (int narrow = 0; narrow < 2; narrow++) {
			sy->play_width = narrow ? 384 : saved;
			int w = st->width, span = sy->play_width - 2 * sy->margin;
			staged(&s, k, terry, terry, w / 2 - 100, w / 2 + 100);
			/* P0 backs off: the camera keeps the midpoint centred until the
			 * window's far wall (or the stage wall) stops P0. */
			for (int n = 0; n < 400; n++) {
				step2(&s, K(LEFT), 0);
				on_screen(&s);
				int mid = (s.p[0].x + s.p[1].x) / 8;
				assert(s.cam == (uint32_t)clampi(mid - FW / 2, 0, w - FW));
			}
			int stop = w / 2 + 100 - span, lo, hi;
			if (stop < sy->margin)
				stop = sy->margin;
			assert(s.p[1].x == QX(w / 2 + 100) && s.p[0].x == QX(stop));
			fight_walls(&s, &pack, &lo, &hi);
			assert(lo == stop);
			/* Both go left: the stage's left end. */
			for (int n = 0; n < 900; n++) {
				step2(&s, K(LEFT), K(LEFT));
				on_screen(&s);
			}
			assert(s.cam == 0 && s.p[0].x == QX(sy->margin + (narrow ? 48 : 0)));
			/* Both go right: the right end. */
			for (int n = 0; n < 1500; n++) {
				step2(&s, K(RIGHT), K(RIGHT));
				on_screen(&s);
			}
			assert(s.cam == (uint32_t)(w - FW) &&
			       s.p[1].x == QX(w - sy->margin - (narrow ? 48 : 0)));
		}
		sy->play_width = saved;
		corner_at((int)k);
	}
	/* A stand LP that reaches 420 px hits P1 6 px from the right screen
	 * wall in the middle of Korea: P1 slides into the wall and the rest
	 * pushes P0 back, though neither is near a stage wall. */
	const struct move *lp = &pack.chr[terry].move[STAND_LP];
	struct box saved[64];
	unsigned first = pack.step[lp->anim.first].box, boxes = 0;
	for (unsigned i = 0; i < lp->anim.count; i++) {
		const struct step *st = &pack.step[lp->anim.first + i];
		boxes = st->box + st->hurt + st->hit - first;
	}
	assert(boxes <= 64);
	memcpy(saved, pack.box + first, sizeof(struct box) * boxes);
	for (unsigned i = 0; i < lp->anim.count; i++) {
		const struct step *st = &pack.step[lp->anim.first + i];
		for (unsigned h = 0; h < st->hit; h++)
			pack.box[st->box + st->hurt + h].w = 500;
	}
	staged(&s, korea, terry, terry, 166, 586);
	int lo, hi;
	fight_walls(&s, &pack, &lo, &hi);
	assert(s.cam == 136 && hi == 592);
	for (int n = 1; n <= 30; n++) {
		run(&s, n, K(Y), 0, 0);
		on_screen(&s);
		assert(s.cam > 0 && s.cam < (uint32_t)(pack.stage[korea].width - FW));
		fight_walls(&s, &pack, &lo, &hi);
		if (n == 4)
			assert(s.p[1].hp < 1000 && s.p[1].corner && s.p[1].slide);
	}
	assert(s.p[1].x == QX(hi) && s.p[0].x < QX(166) && s.p[1].x < QX(586 + 14));
	memcpy(pack.box + first, saved, sizeof(struct box) * boxes);
	/* Projectiles leave 64 px beyond the camera's window, not the stage's
	 * first screen. */
	staged(&s, korea, terry, terry, 276, 476);
	assert(s.cam == 136);
	int edge = 136 + FW + 64, speed = pack.chr[terry].projectile[0].speed;
	s.shot[0] = (struct shot){1, 0, edge - speed, 0, 1, 10};
	step2(&s, 0, 0);
	assert(s.cam == 136 && s.shot[0].x == edge);
	step2(&s, 0, 0);
	assert(!live_shots(&s));
	s.shot[0] = (struct shot){1, 0, 136 - 64 + speed, 0, -1, 10};
	step2(&s, 0, 0);
	assert(s.shot[0].x == 136 - 64);
	step2(&s, 0, 0);
	assert(!live_shots(&s));
	(void)mizuki;
}
/* The spark slot holding kind (spark index + 1), or -1. */
static int spark_of(const struct fight_state *s, unsigned kind)
{
	for (int k = 0; k < SPARK_SLOTS; k++)
		if (s->spark[k].kind == (int32_t)kind)
			return k;
	return -1;
}
/* P0 presses button on call 1 against P1 (guarding when guard is set)
 * 60 px away on stage k; returns the call of the contact, checking the
 * whoosh, hit or guard events, the sparks at the contact point (inside
 * P0's active hitbox), the shake and that state words carry it all. */
static int strike(unsigned k, unsigned c0, unsigned move, uint32_t button, int guard)
{
	struct fight_state s, t;
	uint32_t w[STATE_WORDS], v[STATE_WORDS];
	int centre = pack.stage[k].width / 2;
	staged(&s, k, c0, terry, centre - 30, centre + 30);
	const struct character *c = &pack.chr[c0];
	const struct move *m = &c->move[move];
	const struct system *sy = &pack.sys[c->system];
	int t0 = tier_of(&m->hit, (int)move), slash = m->slash, contact = 0;
	for (int n = 1; n <= 40 && !contact; n++) {
		run(&s, n, button, 0, guard ? K(RIGHT) : 0);
		const struct sound_event *e = event(&s, SOUND_WHOOSH_LIGHT + t0);
		assert((n == 1) == (e != NULL));
		if (e)
			assert(e->side == 0 && e->chr == c0 && e->system == c->system &&
			       e->move == move + 1 && s.p[0].move == (int)move + 1);
		if (!s.hits)
			continue;
		contact = n;
		int id = guard ? (slash ? SOUND_SLASH_GUARD : SOUND_GUARD)
			       : (slash ? SOUND_SLASH_LIGHT : SOUND_HIT_LIGHT) + t0;
		e = event(&s, id);
		assert(e && e->side == 0 && e->move == move + 1 && heard(&s, id, -1) == 1);
		int key = guard ? (slash ? FX_SLASH_GUARD : FX_GUARD)
				: (slash ? FX_SLASH_LIGHT : FX_HIT_LIGHT) + t0;
		assert(sy->effect[key]);
		int slot = spark_of(&s, sy->effect[key]);
		assert(slot >= 0 && !s.spark[slot].age && s.spark[slot].flip == 0);
		if (slash && !guard && sy->effect[FX_BLOOD_LIGHT + t0])
			assert(spark_of(&s, sy->effect[FX_BLOOD_LIGHT + t0]) >= 0);
		/* P0 is frozen in the step that hit: its hitbox holds the spark. */
		const struct step *st = &pack.step[fight_pose(&s, &pack, 0)];
		int inside = 0;
		for (unsigned h = 0; h < st->hit; h++) {
			struct area r = fight_box(&pack.box[st->box + st->hurt + h], s.p[0].x / 4,
						  s.p[0].y, s.p[0].face);
			inside |= s.spark[slot].x >= r.x && s.spark[slot].x <= r.x + r.w &&
				  s.spark[slot].y >= r.y && s.spark[slot].y <= r.y + r.h;
		}
		assert(inside);
		assert(s.shake == (!guard && t0 ? (uint32_t)SHAKE_HEAVY : 0u));
		/* Sparks, shake and sounds survive a save and replay the same. */
		fight_encode(&s, w);
		assert(fight_decode(&t, &pack, w));
		fight_encode(&t, v);
		assert(!memcmp(w, v, sizeof(w)) && t.sounds == s.sounds && t.shake == s.shake);
		for (int j = 0; j < 30; j++) {
			step2(&s, 0, guard ? K(RIGHT) : 0);
			step2(&t, 0, guard ? K(RIGHT) : 0);
			fight_encode(&s, w);
			fight_encode(&t, v);
			assert(!memcmp(w, v, sizeof(w)));
		}
		/* Sparks age every fight tick and leave after their anim. */
		const struct spark *an = &pack.spark[sy->effect[key] - 1];
		if (an->total <= 30)
			assert(spark_of(&s, sy->effect[key]) < 0);
	}
	assert(contact);
	return contact;
}
static void spark_sound_tests(void)
{
	struct fight_state s, t;
	unsigned korea = stage_index("KOREA"), mizuki = stage_index("MIZUKI");
	/* Strikes: KOF hit and guard sparks, Samurai Shodown slash and blood,
	 * light and heavy. */
	assert(strike(korea, terry, STAND_LP, K(Y), 0) == 4);
	assert(strike(korea, terry, STAND_LP, K(Y), 1) == 4);
	strike(korea, terry, STAND_HP, K(X), 0);
	assert(pack.chr[haohmaru].move[STAND_LP].slash && pack.chr[haohmaru].move[STAND_HP].slash &&
	       !pack.chr[haohmaru].move[STAND_LK].slash && !pack.chr[terry].move[STAND_LP].slash);
	strike(mizuki, haohmaru, STAND_LP, K(Y), 0);
	strike(mizuki, haohmaru, STAND_LP, K(Y), 1);
	strike(mizuki, haohmaru, STAND_HP, K(X), 0);
	strike(mizuki, haohmaru, STAND_LK, K(B), 0);
	/* A full pool replaces its oldest spark (filled with the longest one). */
	const struct system *sy = &pack.sys[pack.chr[terry].system];
	unsigned kind = sy->effect[FX_HIT_LIGHT], longest = 1;
	for (unsigned k = 1; k <= pack.sparks; k++)
		if (pack.spark[k - 1].total > pack.spark[longest - 1].total)
			longest = k;
	assert(pack.spark[longest - 1].total > 12 && longest != kind);
	staged(&s, korea, terry, terry, 346, 406);
	for (int k = 0; k < SPARK_SLOTS; k++)
		s.spark[k] = (struct spark_slot){(int32_t)longest, 300, 150, k == 5 ? 6 : k % 3, 1};
	for (int n = 1; n <= 4; n++)
		run(&s, n, K(Y), 0, 0);
	assert(s.hits == 1 && s.spark[5].kind == (int32_t)kind && !s.spark[5].age);
	for (int k = 0; k < SPARK_SLOTS; k++)
		if (k != 5)
			assert(s.spark[k].kind == (int32_t)longest && s.spark[k].age == k % 3 + 4);
	/* Jump and land. */
	staged(&s, korea, terry, terry, 276, 476);
	int jumped = 0, landed = 0, air = 0;
	for (int n = 1; n <= 90; n++) {
		step2(&s, n <= 2 ? K(UP) : 0, 0);
		air |= s.p[0].y < 0;
		jumped += heard(&s, SOUND_JUMP, 0);
		if (heard(&s, SOUND_JUMP, 0))
			assert(!landed && event(&s, SOUND_JUMP)->chr == terry);
		landed += heard(&s, SOUND_LAND, 0);
		if (heard(&s, SOUND_LAND, 0))
			assert(jumped && !s.p[0].y);
		assert(!heard(&s, SOUND_JUMP, 1) && !heard(&s, SOUND_LAND, 1));
	}
	assert(jumped == 1 && landed == 1 && air);
	/* Power Wave: launch on the call the wave appears, then a clash. */
	staged(&s, korea, terry, terry, 276, 556);
	int launched = 0, clashed = 0;
	for (int n = 1; n <= 60; n++) {
		step2(&s, motion(MOTION_236, n, 5, 1, K(Y)), motion(MOTION_236, n, 5, -1, K(Y)));
		int now = heard(&s, SOUND_LAUNCH, -1);
		if (now) {
			assert(now == 2 && live_shots(&s) == 2 && !launched);
			assert(event(&s, SOUND_LAUNCH)->move == POWER_WAVE + 1);
			launched = n;
		}
		if (heard(&s, SOUND_CLASH, -1)) {
			assert(!live_shots(&s) && !clashed);
			clashed = n;
		}
	}
	assert(launched == 22 && clashed > launched && s.clashes == 1);
	/* KO: the KO event, the long shake and the white flash, then a new
	 * round starts with its event. */
	staged(&s, korea, terry, terry, 346, 406);
	s.p[1].hp = 10;
	for (int n = 1; n <= 4; n++)
		run(&s, n, K(Y), 0, 0);
	assert(!s.p[1].hp && heard(&s, SOUND_KO, 1) == 1 && s.shake == SHAKE_KO &&
	       s.flash == FLASH_TICKS);
	static struct scene sc;
	fight_cast(&pack, &s);
	fight_scene(&s, &pack, &sc);
	assert(sc.cmd[0].op == RECT && sc.cmd[0].color == 0xffff && sc.cmd[0].w == FW &&
	       !sc.layers && !sc.sparks);
	for (int n = 0; n < FLASH_TICKS; n++)
		step2(&s, 0, 0);
	assert(!s.flash && s.shake == SHAKE_KO - FLASH_TICKS);
	fight_scene(&s, &pack, &sc);
	assert(sc.layers > 0);
	int starts = 0;
	for (int n = 0; n < 200 + ROUND_TICKS && !starts; n++) {
		step2(&s, 0, 0);
		starts = heard(&s, SOUND_ROUND_START, 0);
	}
	assert(starts == 1 && s.mode == MODE_FIGHT && !s.ended && s.timer == ROUND_TICKS);
	/* A paused tick emits nothing and changes nothing. */
	staged(&s, korea, terry, terry, 346, 406);
	step2(&s, K(START), 0);
	step2(&s, 0, 0);
	t = s;
	step2(&s, K(Y), 0);
	assert(s.paused && !s.sounds && !s.p[0].move && s.tick == t.tick);
}
/* Camera, arena, shake, flash, sparks and sounds in the state words: stage
 * states round-trip and impossible values are rejected without touching the
 * destination. */
static void stage_state(void)
{
	struct fight_state s, t, keep;
	uint32_t w[STATE_WORDS], bad[STATE_WORDS];
	unsigned korea = stage_index("KOREA");
	staged(&s, korea, terry, terry, 150, 210);
	for (int n = 1; n <= 30 && !s.hits; n++)
		run(&s, n, K(Y), 0, 0);
	int slot = 0;
	while (slot < SPARK_SLOTS && !s.spark[slot].kind)
		slot++;
	assert(s.hits == 1 && s.sounds && slot < SPARK_SLOTS && s.cam == 0);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	const int S = 19 + 2 * FIGHTER_WORDS_T, X = S + 6 * MAX_PROJECTILES + 7, SP = X + 4,
		  SO = SP + 5 * SPARK_SLOTS;
	assert(w[SP + 5 * slot] == (uint32_t)s.spark[slot].kind && w[SO] == s.sounds &&
	       w[X] == korea && w[X + 1] == s.cam && w[X + 2] == s.shake);
	int free_slot = 0;
	while (s.spark[free_slot].kind)
		free_slot++;
	int lo, hi;
	fight_walls(&s, &pack, &lo, &hi);
	const struct spark *an = &pack.spark[s.spark[slot].kind - 1];
	uint32_t ev = w[SO + 1];
	struct bad_word {
		int at;
		uint32_t value;
	} cases[] = {
	    {X, pack.stages},
	    {X + 1, (uint32_t)(pack.stage[korea].width - FW + 1)},
	    /* P0 left of the camera's left wall */
	    {X + 1, (uint32_t)(s.p[0].x / 4 - lo + 1)},
	    {X + 2, SHAKE_KO + 1},
	    {X + 3, FLASH_TICKS + 1},
	    {SP + 5 * slot, pack.sparks + 1},
	    {SP + 5 * slot, (uint32_t)-1},
	    {SP + 5 * slot + 3, an->total},
	    {SP + 5 * slot + 4, 2},
	    {SP + 5 * slot + 1, (uint32_t)(pack.stage[korea].width + MAX_SIDE + 1)},
	    {SP + 5 * free_slot + 1, 5},
	    {SO, MAX_SOUND_EVENTS + 1},
	    {SO + 1 + (int)s.sounds, 1},
	    {SO + 1, (ev & ~255u) | SOUNDS},
	    {SO + 1, (ev & ~(127u << 9)) | pack.characters << 9},
	    {SO + 1, (ev & ~(255u << 16)) | (pack.systems + 1) << 16},
	    {SO + 1, (ev & 0xffffffu) | 250u << 24},
	};
	int n = (int)(sizeof(cases) / sizeof(cases[0]));
	for (int i = 0; i < n; i++) {
		memcpy(bad, w, sizeof(w));
		bad[cases[i].at] = cases[i].value;
		keep = t;
		if (fight_decode(&t, &pack, bad)) {
			fprintf(stderr, "bad stage state case %d accepted\n", i);
			abort();
		}
		assert(!memcmp(&keep, &t, sizeof(t)));
	}
	/* Outside a fight the arena, camera, shake, flash and sparks are 0. */
	staged(&s, korea, terry, terry, 150, 210);
	s.mode = MODE_RESULT;
	s.bout = 0;
	s.p[0].x = QX(200);
	s.p[1].x = QX(260);
	s.arena = s.cam = 0;
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	for (int i = 0; i < 4; i++) {
		memcpy(bad, w, sizeof(w));
		bad[X + i] = 1;
		assert(!fight_decode(&t, &pack, bad));
	}
	memcpy(bad, w, sizeof(w));
	bad[SP] = 1;
	assert(!fight_decode(&t, &pack, bad));
}
/* ---- stats and systems (systems.json, terry.json, haohmaru.json) ---- */
/* State word of P0's vy, as laid out in state_words. */
enum { P0_VY = 19 + 2 };
/* Walk 10 ticks forward, then 10 back. */
static void walk_test(unsigned c, int fwd, int back)
{
	struct fight_state s;
	match(&s, c, terry, 100, 400);
	for (int n = 0; n < 10; n++)
		step2(&s, K(RIGHT), 0);
	assert(s.p[0].x == QX(100) + 10 * fwd && s.p[0].walk == 1);
	for (int n = 0; n < 10; n++)
		step2(&s, K(LEFT), 0);
	assert(s.p[0].x == QX(100) + 10 * fwd - 10 * back && s.p[0].walk == -1);
}
/* Up on call 1: takeoff on call 1 + prejump, the peak and the landing call. */
static void jump_test(unsigned c, int prejump, int peak, int airborne)
{
	struct fight_state s;
	match(&s, c, terry, 100, 400);
	int top = 0, takeoff = 0, land = 0;
	for (int n = 1; n <= 80; n++) {
		step2(&s, n == 1 ? K(UP) : 0, 0);
		if (!takeoff && s.p[0].vy)
			takeoff = n;
		if (takeoff && !land && !s.p[0].y && !s.p[0].vy)
			land = n;
		top = s.p[0].y < top ? s.p[0].y : top;
	}
	assert(takeoff == 1 + prejump && top == -peak && land == takeoff + airborne);
}
/* Calls until P0's first contact (at most 40), with P1 idle. */
static int first_hit(struct fight_state *s, uint32_t button)
{
	for (int n = 1; n <= 40; n++) {
		step2(s, n == 1 ? button : 0, 0);
		if (s->hits)
			return n;
	}
	return 0;
}
static void stat_tests(void)
{
	struct fight_state s, back;
	uint32_t w[STATE_WORDS];
	const struct character *t = &pack.chr[terry], *h = &pack.chr[haohmaru];
	const struct system *kof = &pack.sys[t->system], *ss = &pack.sys[h->system];
	/* Terry has only his walk speeds of his own, the rest is kof2002's
	 * defaults. Haohmaru is assigned samsho5sp by systems.json. */
	assert(!strcmp(kof->name, "kof2002") && kof->damage_scale == 1000 &&
	       !strcmp(ss->name, "samsho5sp") && ss->damage_scale == 800);
	assert(t->stats.life == 1000 && t->stats.defense == 1000 && t->stats.weight == 1000 &&
	       t->stats.walk == 12 && t->stats.walk_back == 8 && t->stats.height == HEIGHT_MID &&
	       t->stats.jump.vy == -51 && t->stats.jump.gravity == 3 && t->stats.jump.vx == 8 &&
	       t->stats.jump.vx_back == 8 && t->stats.jump.peak == 108 && t->stats.push == 46 &&
	       t->stats.guard_range == 160);
	assert(h->stats.life == 1000 && h->stats.walk == 9 && h->stats.walk_back == 8 &&
	       h->stats.jump.vy == -42 && h->stats.jump.gravity == 2 && h->stats.jump.vx == 7 &&
	       h->stats.jump.prejump == 1 && h->stats.jump.peak == 110);
	/* Class tables: kof2002 light 11/9 hitstop 8, samsho5sp heavy 22/20/12. */
	assert(kof->cls[CLASS_LIGHT].hitstun == 11 && kof->cls[CLASS_LIGHT].blockstun == 9 &&
	       kof->cls[CLASS_LIGHT].hitstop == 8 && kof->cls[CLASS_LIGHT].has == 3 &&
	       kof->cls[CLASS_COMMAND].has == 3 && !kof->cls[CLASS_THROW].has &&
	       ss->cls[CLASS_HEAVY].hitstun == 22 && ss->cls[CLASS_HEAVY].hitstop == 12);
	/* samsho5sp scales damage at load: stand LP 55 in haohmaru.json -> 44. */
	assert(h->move[STAND_LP].hit.damage == 44 && t->move[STAND_LP].hit.damage == 30);
	/* Walk: Terry 10 x 12/4 px forward and 10 x 8/4 back, Haohmaru 9 and 8. */
	walk_test(terry, 12, 8);
	walk_test(haohmaru, 9, 8);
	/* Jump: Terry -51/3 after 4 prejump ticks peaks at 108 px and lands 35
	 * ticks later; Haohmaru -42/2 after 1 peaks at 110 and lands after 43. */
	jump_test(terry, 4, 108, 35);
	jump_test(haohmaru, 1, 110, 43);
	/* Damage scale in a fight: Haohmaru's stand LP takes 44 from Terry,
	 * Terry's takes 30 from Haohmaru. */
	match(&s, haohmaru, terry, 200, 250);
	assert(first_hit(&s, K(Y)) && s.p[1].hp == 1000 - 44);
	match(&s, terry, haohmaru, 200, 260);
	assert(first_hit(&s, K(Y)) == 4 && s.p[1].hp == 1000 - 30);
	/* The rest edits Terry's stats in the loaded pack and restores them. */
	struct character saved = pack.chr[terry];
	struct stats *st = &pack.chr[terry].stats;
	/* Life 1200: both start at 1200, stand LP leaves 1170, the life bar
	 * is 1170 * 200 / 1200 = 195 px; that state is refused once the life
	 * is back to 1000. */
	st->life = 1200;
	duel(&s, 200, 260);
	assert(s.p[0].hp == 1200 && s.p[1].hp == 1200);
	assert(first_hit(&s, K(Y)) == 4 && s.p[1].hp == 1170);
	static struct scene scene;
	fight_scene(&s, &pack, &scene);
	int bar = -1;
	for (unsigned k = 0; k < scene.count; k++)
		if (scene.cmd[k].op == RECT && scene.cmd[k].x == 262 && scene.cmd[k].y == 18)
			bar = scene.cmd[k].w;
	assert(bar == 195);
	fight_encode(&s, w);
	assert(fight_decode(&back, &pack, w));
	st->life = 1000;
	assert(!fight_decode(&back, &pack, w));
	/* Defense: 1500 takes 45 from the 30-point stand LP, 800 takes 24. */
	st->defense = 1500;
	duel(&s, 200, 260);
	assert(first_hit(&s, K(Y)) == 4 && s.p[1].hp == 955);
	st->defense = 800;
	duel(&s, 200, 260);
	assert(first_hit(&s, K(Y)) == 4 && s.p[1].hp == 976);
	st->defense = 1000;
	/* Weight: 2000 halves stand LP's 14 px pushback, 500 doubles it. */
	static const int weights[2][2] = {{2000, 267}, {500, 288}};
	for (int k = 0; k < 2; k++) {
		st->weight = (int16_t)weights[k][0];
		duel(&s, 200, 260);
		for (int n = 1; n <= 30; n++)
			run(&s, n, K(Y), 0, 0);
		assert(s.p[1].x == QX(weights[k][1]) && s.p[0].x == QX(200));
	}
	st->weight = 1000;
	/* Push distance: bodies 20 px apart are pushed to 46 around the
	 * midpoint 210 (187 and 233), with push 60 to 180 and 240. */
	duel(&s, 200, 220);
	step2(&s, 0, 0);
	assert(s.p[0].x == QX(187) && s.p[1].x == QX(233));
	st->push = 60;
	duel(&s, 200, 220);
	step2(&s, 0, 0);
	assert(s.p[0].x == QX(180) && s.p[1].x == QX(240));
	st->push = 46;
	/* Guard range: P1 holding back 60 px from a stand LP shows the guard
	 * stance with range 160 and just walks back with range 40. */
	duel(&s, 200, 260);
	step2(&s, K(Y), K(RIGHT));
	step2(&s, 0, K(RIGHT));
	assert(s.p[1].block == 2);
	st->guard_range = 40;
	duel(&s, 200, 260);
	step2(&s, K(Y), K(RIGHT));
	step2(&s, 0, K(RIGHT));
	assert(!s.p[1].block && s.p[1].x == QX(260) + 2 * st->walk_back);
	st->guard_range = 160;
	/* Buffer 2: of presses on calls 10 and 11 during a whiffed stand LP
	 * (ends on call 13), only the later one comes out (with buffer 4 both
	 * do, see buffer()). */
	st->buffer = 2;
	for (int press = 10; press <= 11; press++) {
		duel(&s, 100, 400);
		for (int n = 1; n <= 13; n++)
			step2(&s, n == 1 ? K(Y) : n == press ? K(X) : 0, 0);
		assert(press == 11 ? s.p[0].move == STAND_HP + 1 : !s.p[0].move);
	}
	st->buffer = 4;
	/* Quarter-pixel x survives a state round trip; the jump bounds are the
	 * character's: airborne Terry accepts vy -51 .. 87 (the fall from
	 * MAX_HEIGHT), not -52 or 88. */
	duel(&s, 100, 400);
	step2(&s, K(RIGHT) | K(UP), 0);
	for (int n = 0; n < 6; n++)
		step2(&s, 0, 0);
	assert(s.p[0].x == QX(100) + 12 && s.p[0].y < 0 && t->stats.jump.fall == 87);
	fight_encode(&s, w);
	assert(fight_decode(&back, &pack, w) && !memcmp(&back, &s, sizeof(s)));
	static const struct {
		uint32_t vy;
		int ok;
	} vys[] = {{(uint32_t)-51, 1}, {(uint32_t)-52, 0}, {87, 1}, {88, 0}};
	for (size_t k = 0; k < sizeof(vys) / sizeof(vys[0]); k++) {
		w[P0_VY] = vys[k].vy;
		assert(fight_decode(&back, &pack, w) == vys[k].ok);
	}
	/* Terry's 4-tick prejump is a valid state for him, not with 3;
	 * Haohmaru's is a single tick. */
	match(&s, terry, haohmaru, 100, 400);
	step2(&s, K(UP), K(UP));
	assert(s.p[0].squat == 4 && s.p[1].squat == 1);
	fight_encode(&s, w);
	assert(fight_decode(&back, &pack, w));
	st->jump.prejump = 3;
	assert(!fight_decode(&back, &pack, w));
	pack.chr[terry] = saved;
}
/* The Auto pack's AI uses every move of every character (all normals and
 * specials), throws projectiles, guards at both heights, and finishes
 * ladder runs. Contacts are counted on the tick a fighter enters hitstop
 * with fresh stun. */
/* Normal throws: forward or back with HP or HK within the system's range
 * grabs on the press tick, holds the other fighter (THROW/THROWN) and
 * knocks it down when the THROW animation is over; HP or HK in the tech
 * window breaks it, two at once break each other; nobody in blockstun, a
 * prejump or its safe ticks is thrown (the press is the normal then); no
 * tech on the samsho5sp system; the AI throws and techs. */
/* Run and backdash (KOF 2002 for Terry, Samurai Shodown for Haohmaru):
 * 66 runs at walk speed for run_startup ticks then at run speed, holds
 * through run_min, skids run_stop ticks with a falling speed once forward
 * is let go; attacks and UP cancel a run and a run never throws. 44
 * waits back_startup ticks, flies back_air ticks back exactly
 * back_distance pixels on its arc and lands for back_stop; it cannot be
 * thrown, guard is impossible and a strike knocks it out of the air;
 * samsho5sp coasts run_grace ticks and evades strikes for back_invuln
 * ticks. Mid-run and mid-backdash states round-trip, bad ones are
 * rejected. */
static void dashes(void)
{
	const struct character *t = &pack.chr[terry];
	const struct dash_rule *d = &pack.sys[t->system].dash;
	const struct stats *st = &t->stats;
	struct fight_state s, u;
	uint32_t w[STATE_WORDS], v[STATE_WORDS];
	assert(st->run == (st->walk * d->run_scale + 500) / 1000 && st->run > st->walk);
	assert(t->action[RUN].count && t->action[BACKDASH].count);
	/* A tap needs a release in between and both taps within the window. */
	duel(&s, 100, 400);
	step2(&s, K(RIGHT), 0);
	step2(&s, K(RIGHT), 0);
	assert(!s.p[0].dash && s.p[0].walk == 1);
	duel(&s, 100, 400);
	step2(&s, K(RIGHT), 0);
	for (unsigned n = 0; n < d->window; n++)
		step2(&s, 0, 0);
	step2(&s, K(RIGHT), 0);
	assert(!s.p[0].dash && !s.runs);
	/* 6, 5, 6: running from the third tick. */
	duel(&s, 100, 400);
	step2(&s, K(RIGHT), 0);
	step2(&s, 0, 0);
	int x = s.p[0].x;
	step2(&s, K(RIGHT), 0);
	assert(s.p[0].dash == DASH_RUN && !s.p[0].elapsed && s.runs == 1 &&
	       s.p[0].x == x + st->walk && in_action(&s, 0, RUN) && !s.p[0].walk);
	for (int n = 1; n < 30; n++) {
		x = s.p[0].x;
		/* Let go before run_startup + run_min: it keeps running. */
		step2(&s, n == 5 ? 0 : K(RIGHT), 0);
		assert(s.p[0].dash == DASH_RUN && s.p[0].elapsed == n &&
		       s.p[0].x - x == (n < d->run_startup ? st->walk : st->run));
	}
	fight_encode(&s, w);
	assert(fight_decode(&u, &pack, w));
	fight_encode(&u, v);
	assert(!memcmp(w, v, sizeof(w)));
	/* Let go: the skid slows down, then the fighter is free. */
	for (int n = 0; n < d->run_stop; n++) {
		x = s.p[0].x;
		step2(&s, 0, 0);
		assert(s.p[0].dash == DASH_STOP && s.p[0].elapsed == n &&
		       s.p[0].x - x == st->run * (d->run_stop - n) / (d->run_stop + 1));
	}
	x = s.p[0].x;
	step2(&s, 0, 0);
	assert(!s.p[0].dash && s.p[0].x == x);
	/* An attack cancels the run; so does UP (into a jump). */
	duel(&s, 100, 400);
	step2(&s, K(RIGHT), 0);
	step2(&s, 0, 0);
	for (int n = 0; n < 8; n++)
		step2(&s, K(RIGHT), 0);
	step2(&s, K(RIGHT) | K(Y), 0);
	assert(!s.p[0].dash && s.p[0].move == STAND_LP + 1);
	duel(&s, 100, 400);
	step2(&s, K(RIGHT), 0);
	step2(&s, 0, 0);
	step2(&s, K(RIGHT), 0);
	step2(&s, K(RIGHT) | K(UP), 0);
	assert(!s.p[0].dash && s.p[0].squat);
	/* Running into a throwable opponent with forward + HP is the normal. */
	const int body = st->push;
	duel(&s, 200, 200 + body);
	s.p[0].dash = DASH_RUN;
	s.p[0].elapsed = 20;
	step2(&s, K(RIGHT) | K(X), 0);
	assert(!s.p[0].grab && !s.p[1].grab && s.p[0].move == STAND_HP + 1);
	/* 4, 5, 4: the backdash, from its start tick to landing. */
	duel(&s, 200, 400);
	step2(&s, K(LEFT), 0);
	step2(&s, 0, 0);
	x = s.p[0].x;
	int total = d->back_startup + d->back_air + d->back_stop, air = 0;
	for (int n = 0; n < total; n++) {
		step2(&s, n ? 0 : K(LEFT), 0);
		int j = n - d->back_startup;
		assert(s.p[0].dash == DASH_BACK && s.p[0].elapsed == n && in_action(&s, 0, BACKDASH));
		assert(heard(&s, SOUND_JUMP, 0) == (j == 0) && heard(&s, SOUND_LAND, 0) == (j == d->back_air - 1));
		if (j >= 0 && j < d->back_air - 1) {
			assert(s.p[0].y < 0 || (j == 0 && s.p[0].y == 0));
			air++;
		} else
			assert(!s.p[0].y);
		if (n == d->back_startup + 6) {
			fight_encode(&s, w);
			assert(fight_decode(&u, &pack, w));
			fight_encode(&u, v);
			assert(!memcmp(w, v, sizeof(w)));
		}
	}
	assert(air == d->back_air - 1 && s.p[0].x == x - QX(d->back_distance) && s.backdashes == 1);
	step2(&s, 0, 0);
	assert(!s.p[0].dash && !s.p[0].y && !s.p[0].vy);
	/* Bad dash words: past the phases, a skid too long, a backdash off its
	 * arc, running in the air, KOF has no coast. */
	{
		const int P = 19, D = 30, Y = 1, E = 9;
		static const struct {
			int field;
			uint32_t value;
		} bad[] = {{D, DASH_PHASES}, {D, (uint32_t)-1}, {E, 255}, {Y, (uint32_t)-1},
			   {D, DASH_COAST}};
		fight_decode(&u, &pack, w);
		for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			memcpy(v, w, sizeof(v));
			v[P + bad[i].field] = bad[i].value;
			struct fight_state keep = u;
			assert(!fight_decode(&u, &pack, v) && !memcmp(&keep, &u, sizeof(u)));
		}
		memcpy(v, w, sizeof(v));
		v[P + D] = DASH_STOP;
		v[P + E] = d->run_stop;
		v[P + Y] = 0;
		assert(!fight_decode(&u, &pack, v));
	}
	/* A backdash cannot be thrown, even on the ground in its startup, and
	 * cannot guard; a strike knocks it out of the air. */
	duel(&s, 200, 200 + body);
	step2(&s, 0, K(RIGHT));
	step2(&s, 0, 0);
	step2(&s, 0, K(RIGHT));
	step2(&s, K(RIGHT) | K(X), 0);
	assert(s.p[1].dash == DASH_BACK && s.p[1].elapsed == 1 && !s.p[0].grab &&
	       s.p[0].move == STAND_HP + 1);
	int airborne_hit = 0;
	for (int gap = body; gap < body + 60 && !airborne_hit; gap += 4)
		for (int press = 1; press < 12 && !airborne_hit; press++) {
			duel(&s, 200, 200 + gap);
			for (int n = 1; n < 40 && !s.hits; n++) {
				int y = s.p[1].y;
				step2(&s, n == press ? K(X) : 0, n == 1 || n == 3 ? K(RIGHT) : 0);
				if (s.hits && y < 0) {
					assert(!s.guards && !s.p[1].dash && s.p[1].stun);
					airborne_hit = 1;
				}
			}
		}
	assert(airborne_hit);
	for (int n = 0; n < 60 && s.p[1].y; n++)
		step2(&s, 0, 0);
	assert(!s.p[1].y && !s.p[1].vy);
	/* samsho5sp: a held dash coasts run_grace ticks and forward again keeps
	 * it running; the first back_invuln backdash ticks evade strikes. */
	const struct dash_rule *ds = &pack.sys[pack.chr[haohmaru].system].dash;
	assert(ds->run_grace && ds->back_invuln && !ds->back_startup);
	match(&s, haohmaru, haohmaru, 100, 400);
	step2(&s, K(RIGHT), 0);
	step2(&s, 0, 0);
	for (int n = 0; n < 12; n++)
		step2(&s, K(RIGHT), 0);
	step2(&s, 0, 0);
	assert(s.p[0].dash == DASH_COAST);
	step2(&s, K(RIGHT), 0);
	assert(s.p[0].dash == DASH_RUN);
	for (int n = 0; n <= ds->run_grace; n++)
		step2(&s, 0, 0);
	assert(s.p[0].dash == DASH_STOP);
	/* Haohmaru's stand LP from 90 px lands on its first active tick; a
	 * backdash begun on that tick is not hit then or on the next. */
	const struct move *lp = &pack.chr[haohmaru].move[STAND_LP];
	int gap = 0;
	for (; gap < 200; gap++) {
		match(&s, haohmaru, haohmaru, 200, 200 + gap);
		for (int n = 0; n <= lp->startup && !s.hits; n++)
			step2(&s, n ? 0 : K(Y), 0);
		if (!s.hits)
			break;
	}
	assert(gap > 0 && lp->startup >= 2);
	match(&s, haohmaru, haohmaru, 200, 200 + gap - 1);
	for (int n = 0; n <= lp->startup + 1; n++) {
		uint32_t back = n == lp->startup - 2 || n == lp->startup ? K(RIGHT) : 0;
		step2(&s, n ? 0 : K(Y), back);
		if (n == lp->startup)
			assert(s.p[1].dash == DASH_BACK && !s.p[1].elapsed);
		assert(!s.hits);
	}
}
static void throws(void)
{
	const struct character *t = &pack.chr[terry];
	const struct throw_rule *r = &pack.sys[t->system].throw;
	const int body = t->stats.push, total = t->action[THROW].total;
	assert(r->range == 4 && r->tech == 6 && r->wakeup_safe == 8 && !r->stun_safe &&
	       r->hit.damage == 100 && r->hit.cls == CLASS_THROW &&
	       (r->hit.flags & FLAG_KNOCKDOWN) && total >= 30 && total <= MAX_MOVE_TICKS);
	for (unsigned c = 0; c < pack.characters; c++)
		for (int k = THROW; k <= THROWN; k++)
			assert(pack.chr[c].action[k].count && pack.chr[c].action[k].total);
	struct fight_state s, u;
	uint32_t w[STATE_WORDS], v[STATE_WORDS];
	/* Forward: grabbed on the press tick, held in front, thrown. */
	duel(&s, 200, 200 + body + r->range);
	step2(&s, K(RIGHT) | K(X), 0);
	assert(s.p[0].grab == GRAB_FORWARD && s.p[1].grab == GRAB_HELD && !s.p[0].move &&
	       s.p[1].x == QX(200 + body) && s.p[1].face == -1 &&
	       heard(&s, SOUND_WHOOSH_HEAVY, 0));
	assert(in_action(&s, 0, THROW) && in_action(&s, 1, THROWN));
	for (int n = 1; n < total; n++) {
		step2(&s, 0, 0);
		assert(s.p[0].grab == GRAB_FORWARD && s.p[1].hp == 1000 && s.p[0].elapsed == n);
		if (n == 10) {
			fight_encode(&s, w);
			assert(fight_decode(&u, &pack, w));
			fight_encode(&u, v);
			assert(!memcmp(w, v, sizeof(w)));
		}
	}
	step2(&s, 0, 0);
	assert(!s.p[0].grab && !s.p[1].grab && s.p[1].down == FALLING &&
	       s.p[1].hp == 1000 - per_mille(100, t->stats.defense) && s.throws == 1 &&
	       s.p[1].slide > 0 && s.p[0].freeze == r->hit.hitstop &&
	       heard(&s, SOUND_HIT_HEAVY, 0));
	/* Bad throw words: a held fighter without a thrower, the two out of
	 * step or facing the same way, a tech flag left over, a thrower past
	 * its animation. */
	const int P = 19, G = 23;
	static const struct {
		int who, field;
		uint32_t value;
	} bad[] = {{0, G, GRAB_NONE}, {1, 9, 3}, {1, G, GRAB_TECH}, {1, 4, 1}, {0, 9, 255}};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		memcpy(v, w, sizeof(v));
		v[P + bad[i].who * FIGHTER_WORDS_T + bad[i].field] = bad[i].value;
		struct fight_state keep = u;
		assert(!fight_decode(&u, &pack, v) && !memcmp(&keep, &u, sizeof(u)));
	}
	/* Rising from the throw's knockdown starts wakeup_safe ticks. */
	int n = 0;
	while (s.p[1].down && n++ < 400)
		step2(&s, 0, 0);
	assert(!s.p[1].down && s.p[1].safe == r->wakeup_safe);
	for (n = 0; s.p[1].safe; n++)
		step2(&s, 0, 0);
	assert(n == r->wakeup_safe);
	/* Back: the one held lands behind the thrower. */
	duel(&s, 200, 200 + body);
	step2(&s, K(LEFT) | K(A), 0);
	assert(s.p[0].grab == GRAB_BACK);
	for (n = 0; s.p[0].grab && n < 300; n++)
		step2(&s, 0, 0);
	assert(s.p[1].down && s.p[1].x < s.p[0].x && s.p[1].slide < 0);
	/* Tech: HP on the second tick held breaks it, no damage. */
	duel(&s, 200, 200 + body);
	step2(&s, K(RIGHT) | K(X), 0);
	step2(&s, 0, 0);
	step2(&s, 0, K(X));
	assert(!s.p[0].grab && !s.p[1].grab && s.techs == 1 && s.p[1].hp == 1000 &&
	       s.p[0].stun == r->tech_stun && s.p[0].block == 1 && s.p[1].block == 1 &&
	       s.p[0].slide == -r->tech_push && s.p[1].slide == r->tech_push &&
	       heard(&s, SOUND_GUARD, 0));
	/* Too late: a press once the window is over does nothing. */
	duel(&s, 200, 200 + body);
	step2(&s, K(RIGHT) | K(X), 0);
	for (n = 0; n < r->tech; n++)
		step2(&s, 0, 0);
	step2(&s, 0, K(X));
	assert(s.p[1].grab == GRAB_HELD && !s.techs);
	/* Both at once break each other. */
	duel(&s, 200, 200 + body);
	step2(&s, K(RIGHT) | K(X), K(RIGHT) | K(A));
	assert(!s.p[0].grab && !s.p[1].grab && s.techs == 1 && s.p[0].stun && s.p[1].stun);
	/* A throw beats a strike begun on the same tick. */
	duel(&s, 200, 200 + body);
	step2(&s, K(RIGHT) | K(X), K(Y));
	assert(s.p[1].grab == GRAB_HELD && !s.p[1].move);
	for (n = 0; s.p[1].grab && n < 300; n++)
		step2(&s, 0, 0);
	assert(s.hits == 1 && s.p[1].down);
	/* Out of range, in blockstun, in a prejump or safe: the normal. */
	duel(&s, 200, 200 + body + r->range + 1);
	step2(&s, K(RIGHT) | K(X), 0);
	assert(!s.p[0].grab && s.p[0].move == STAND_HP + 1);
	duel(&s, 200, 200 + body);
	s.p[1].stun = 5;
	s.p[1].block = 1;
	step2(&s, K(RIGHT) | K(X), K(RIGHT));
	assert(!s.p[0].grab && s.p[0].move == STAND_HP + 1);
	duel(&s, 200, 200 + body);
	step2(&s, 0, K(UP));
	step2(&s, K(RIGHT) | K(X), K(UP));
	assert(!s.p[0].grab && s.p[0].move == STAND_HP + 1);
	duel(&s, 200, 200 + body);
	s.p[1].safe = 2;
	step2(&s, K(RIGHT) | K(X), 0);
	assert(!s.p[0].grab && s.p[0].move == STAND_HP + 1);
	/* Down-forward is the crouching normal. */
	duel(&s, 200, 200 + body);
	step2(&s, K(RIGHT) | K(DOWN) | K(X), 0);
	assert(!s.p[0].grab && s.p[0].move == CROUCH_HP + 1);
	/* samsho5sp: no tech, a wider range. */
	const struct throw_rule *ss = &pack.sys[pack.chr[haohmaru].system].throw;
	assert(!ss->tech && ss->range == 12 && ss->wakeup_safe == 17 && ss->stun_safe == 17);
	int hb = pack.chr[haohmaru].stats.push;
	match(&s, haohmaru, haohmaru, 200, 200 + hb + ss->range);
	step2(&s, K(RIGHT) | K(X), 0);
	step2(&s, 0, 0);
	step2(&s, 0, K(X));
	assert(s.p[1].grab == GRAB_HELD && !s.techs);
	/* AI vs AI on Normal throws and breaks throws. */
	fight_reset(&s, 1);
	for (n = 0; n < 30000; n++)
		fight_step(&s, &pack, 0, 0, 0);
	printf("throws: %u thrown and %u broken in 30000 AI ticks\n", s.throws, s.techs);
	assert(s.throws > 10 && s.techs > 0);
}
static void ai_coverage(void)
{
	struct fight_state s;
	unsigned hits = 0, blocks = 0, crouch_blocks = 0, crouch_hits = 0, shots = 0;
	uint32_t started[MAX_CHARACTERS] = {0}, ids = 0, kinds = 0, arenas = 0;
	uint32_t w[STATE_WORDS], v[STATE_WORDS];
	struct fight_state twin;
	fight_reset(&s, 1);
	/* Charge specials are often cut short by a hit, so a roster that has
	 * not started everything by then gets up to twice as many ticks again
	 * (with 24 characters a rare special can need the extra time). */
	const int base = 30000 + 15000 * (int)pack.characters;
	int missing = 1;
	for (int t = 0; t < base || (missing && t < 3 * base); t++) {
		if (t >= base && t % 1000 == 0) {
			missing = 0;
			for (unsigned c = 0; c < pack.characters; c++)
				missing |= started[c] != (1u << pack.chr[c].moves) - 1;
		}
		struct fight_state before = s;
		fight_step(&s, &pack, 0, 0, 0);
		/* A copy decoded mid-run replays every tick word for word: camera,
		 * sparks, shake and sound events included. */
		if (t == 20000) {
			fight_encode(&s, w);
			assert(fight_decode(&twin, &pack, w));
		} else if (t > 20000 && t < 26000) {
			fight_step(&twin, &pack, 0, 0, 0);
			fight_encode(&s, w);
			fight_encode(&twin, v);
			assert(!memcmp(w, v, sizeof(w)));
		}
		assert(s.sounds <= MAX_SOUND_EVENTS);
		for (unsigned k = 0; k < s.sounds; k++) {
			const struct sound_event *e = &s.sound[k];
			assert(e->id < SOUNDS && e->chr == s.chr[e->side] &&
			       e->system == pack.chr[e->chr].system && e->move <= pack.chr[e->chr].moves);
			ids |= 1u << e->id;
		}
		if (s.mode != MODE_FIGHT || before.mode != MODE_FIGHT)
			continue;
		on_screen(&s);
		arenas |= 1u << s.arena;
		for (int k = 0; k < SPARK_SLOTS; k++)
			if (s.spark[k].kind && !s.spark[k].age)
				kinds |= 1u << (s.spark[k].kind - 1);
		for (int k = 0; k < MAX_PROJECTILES; k++)
			if (s.shot[k].kind && s.shot[k].age)
				assert(s.shot[k].x >= (int)before.cam - 64 &&
				       s.shot[k].x <= (int)before.cam + FW + 64);
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
	/* The AI runs in from afar and backdashes from swings it does not
	 * guard. */
	printf("ai: %u runs and %u backdashes\n", s.runs, s.backdashes);
	assert(s.runs > 20 && s.backdashes > 20);
	/* Every stage hosted fights; the common sound events and both effect
	 * sets (KOF hits and guards, Samurai Shodown slashes and blood) were
	 * seen. */
	const uint32_t need = 1u << SOUND_WHOOSH_LIGHT | 1u << SOUND_WHOOSH_HEAVY |
			      1u << SOUND_WHOOSH_SPECIAL | 1u << SOUND_HIT_LIGHT |
			      1u << SOUND_HIT_HEAVY | 1u << SOUND_GUARD | 1u << SOUND_SLASH_LIGHT |
			      1u << SOUND_SLASH_HEAVY | 1u << SOUND_SLASH_GUARD | 1u << SOUND_LAND |
			      1u << SOUND_LAUNCH | 1u << SOUND_KO | 1u << SOUND_ROUND_START;
	if ((ids & need) != need || arenas != (1u << pack.stages) - 1) {
		fprintf(stderr, "ai: sounds %x arenas %x\n", ids, arenas);
		abort();
	}
	const struct system *kof = &pack.sys[pack.chr[terry].system],
			    *ss = &pack.sys[pack.chr[haohmaru].system];
	const int want[] = {kof->effect[FX_HIT_LIGHT], kof->effect[FX_HIT_HEAVY],
			    kof->effect[FX_GUARD],     ss->effect[FX_SLASH_LIGHT],
			    ss->effect[FX_SLASH_GUARD], ss->effect[FX_BLOOD_LIGHT]};
	for (int k = 0; k < 6; k++)
		if (!want[k] || !((kinds >> (want[k] - 1)) & 1)) {
			fprintf(stderr, "ai: sparks %x\n", kinds);
			abort();
		}
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
/* The select grid on rosters up to MAX_CHARACTERS of Terry copies with
 * MAX_NAME-letter names: up and down keep the column and wrap, a column the
 * partial last row lacks lands on the last entry, RIGHT and DOWN reach
 * every entry, and on every cursor (P1 and the practice dummy) the grid
 * stays in its band (y 28..170, inside the screen) with the cursor's cell
 * and up to GRID_ROWS rows in view, clear of the poses and the names
 * below. Writes roster previews of a scrolled grid and a dummy pick. */
static void grid_frame(struct fight_state *s, unsigned n)
{
	static struct scene sc;
	int cur = (int)s->chr[s->picking], top = fight_grid_top(cur, (int)n);
	int rows = (int)(n + GRID_COLUMNS - 1) / GRID_COLUMNS, cells = 0, cursor = 0;
	assert(top >= 0 && cur / GRID_COLUMNS >= top && cur / GRID_COLUMNS < top + GRID_ROWS &&
	       (rows <= GRID_ROWS ? top == 0 : top + GRID_ROWS <= rows));
	fight_cast(&pack, s);
	fight_scene(s, &pack, &sc);
	assert(sc.count < COMMANDS);
	for (unsigned k = 0; k < sc.count; k++) {
		const struct command *c = &sc.cmd[k];
		assert(c->x >= 0 && c->y >= 0 && c->x + c->w <= FW && c->y + c->h <= FH);
		if (c->op != RECT || (c->w == FW && c->h == FH) || c->y >= 300)
			continue;
		/* The title row, the grid band or the names and hints from y 240. */
		assert(c->y + c->h <= 24 || c->y >= 240 || (c->y >= 28 && c->y + c->h <= 170));
		cells += c->color == 0x2124 && c->w == 108;
		cursor += c->color == (s->picking ? 0x07ff : 0x07e0) && c->y >= 28 && c->y < 170 &&
			  c->h <= 2;
		if (c->y >= 240 || c->y + c->h <= 24)
			continue;
		for (unsigned j = 0; j < sc.count; j++) {
			const struct command *p = &sc.cmd[j];
			if (p->op == SPRITE)
				assert(p->y >= c->y + c->h || p->y + p->h <= c->y ||
				       p->x >= c->x + c->w || p->x + p->w <= c->x);
		}
	}
	int shown = (int)n - top * GRID_COLUMNS;
	assert(cells == (shown < GRID_ROWS * GRID_COLUMNS ? shown : GRID_ROWS * GRID_COLUMNS));
	if (cursor != 2 || sc.misses) {
		fprintf(stderr, "grid: %d of %u at %d (picking %u): %d cursor rects, %u misses\n",
			cur, n, top, (unsigned)s->picking, cursor, (unsigned)sc.misses);
		abort();
	}
}
static void roster_grid(void)
{
	struct character *saved = malloc(sizeof(pack.chr));
	assert(saved);
	memcpy(saved, pack.chr, sizeof(pack.chr));
	unsigned characters = pack.characters;
	for (int k = 0; k < MAX_CHARACTERS; k++) {
		pack.chr[k] = saved[terry];
		memset(pack.chr[k].name, 'W', MAX_NAME);
		pack.chr[k].name[0] = (char)('A' + k % 26);
		pack.chr[k].name[1] = (char)('A' + k / 26);
		pack.chr[k].name[MAX_NAME] = 0;
	}
	/* Moves on a partial last row (29: row 7 holds only entry 28). */
	assert(fight_grid_move(1, 29, K(DOWN)) == 5 && fight_grid_move(25, 29, K(DOWN)) == 28 &&
	       fight_grid_move(28, 29, K(DOWN)) == 0 && fight_grid_move(2, 29, K(UP)) == 28 &&
	       fight_grid_move(0, 29, K(UP)) == 28 && fight_grid_move(28, 29, K(UP)) == 24 &&
	       fight_grid_move(28, 29, K(RIGHT)) == 0 && fight_grid_move(0, 29, K(LEFT)) == 28 &&
	       fight_grid_move(3, 64, K(UP)) == 63 && fight_grid_move(2, 3, K(DOWN)) == 2 &&
	       fight_grid_move(0, 1, K(UP) | K(LEFT)) == 0);
	static const unsigned sizes[] = {1, 5, 24, 29, 33, 41, MAX_CHARACTERS};
	for (unsigned z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
		unsigned n = sizes[z], rows = (n + GRID_COLUMNS - 1) / GRID_COLUMNS;
		pack.characters = n;
		struct fight_state s;
		uint64_t seen = 0;
		/* DOWN visits every row's first column, then wraps to the top. */
		fight_reset(&s, 0);
		for (unsigned r = 0; r <= rows; r++) {
			assert(s.mode == MODE_SELECT && s.chr[0] == r % rows * GRID_COLUMNS);
			grid_frame(&s, n);
			step2(&s, K(DOWN), 0);
			step2(&s, 0, 0);
		}
		/* RIGHT reaches every entry and wraps. */
		fight_reset(&s, 0);
		for (unsigned k = 0; k <= n; k++) {
			assert(s.mode == MODE_SELECT && s.chr[0] == k % n);
			seen |= 1ull << s.chr[0];
			grid_frame(&s, n);
			step2(&s, K(RIGHT), 0);
			step2(&s, 0, 0);
		}
		assert(seen == (n == 64 ? ~0ull : (1ull << n) - 1));
		/* A practice dummy picked away from P1. */
		fight_reset(&s, 0);
		for (int k = 0; k < 2; k++) {
			step2(&s, K(L), 0);
			step2(&s, 0, 0);
		}
		for (unsigned k = 0; k < n * 3 / 5; k++) {
			step2(&s, K(RIGHT), 0);
			step2(&s, 0, 0);
		}
		step2(&s, K(Y), 0);
		step2(&s, 0, 0);
		assert(s.game == GAME_PRACTICE && s.picking);
		for (int k = 0; k < 3; k++) {
			grid_frame(&s, n);
			step2(&s, K(UP), 0);
			step2(&s, 0, 0);
		}
		if (n == MAX_CHARACTERS) {
			snapshot(&s, "roster-dummy");
			s.picking = 0;
			s.chr[1] = s.chr[0] = 37;
			s.pal[1] = 0;
			s.game = GAME_ARCADE;
			grid_frame(&s, n);
			snapshot(&s, "roster");
		}
	}
	memcpy(pack.chr, saved, sizeof(pack.chr));
	pack.characters = characters;
	free(saved);
}
/* Select, a command list on the select screen, READY, FIGHT, a Power Wave
 * in flight with the box overlay, a lost run, a cleared one, a hit on each
 * stage, a combo count and a pause with its command list, for human
 * review. */
static void previews(void)
{
	struct fight_state s;
	fight_reset(&s, 0);
	for (int n = 0; n < 20; n++)
		step2(&s, 0, 0);
	snapshot(&s, "select");
	/* Iori's command list on the select screen and in a pause. */
	s.chr[0] = s.chr[1] = character("IORI");
	step2(&s, K(START), 0);
	snapshot(&s, "commands");
	step2(&s, 0, 0);
	step2(&s, K(START), 0);
	step2(&s, 0, 0);
	/* READY over the stage, then FIGHT. */
	step2(&s, K(Y), 0);
	while (s.timer < INTRO_TICKS + 10)
		step2(&s, 0, 0);
	snapshot(&s, "ready");
	while (s.timer < ROUND_TICKS + 2)
		step2(&s, 0, 0);
	snapshot(&s, "fight");
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
	/* ROUND 2 after P1 took the first round (its win mark lit), then the
	 * clock in the last ten. */
	fight_reset(&s, 0);
	step2(&s, K(Y), 0);
	intro(&s);
	s.p[1].hp = 1;
	s.p[0].x = QX(200);
	s.p[1].x = QX(260);
	step2(&s, K(Y), 0);
	while (s.mode == MODE_FIGHT && s.bout == 1)
		step2(&s, 0, 0);
	while (s.timer < INTRO_TICKS + 10)
		step2(&s, 0, 0);
	snapshot(&s, "round2");
	while (s.timer < BANNER_END)
		step2(&s, 0, 0);
	s.clock = 9 * COUNT_TICKS;
	snapshot(&s, "clock");
	/* Survival on the select screen, then picking a practice dummy and
	 * the practice HUD with a combo's damage. */
	fight_reset(&s, 0);
	step2(&s, K(L), 0);
	s.record = 12;
	snapshot(&s, "survival");
	step2(&s, 0, 0);
	step2(&s, K(L), 0);
	step2(&s, K(Y), 0);
	assert(s.picking);
	snapshot(&s, "dummy");
	step2(&s, 0, 0);
	step2(&s, K(Y), 0);
	assert(s.game == GAME_PRACTICE && s.mode == MODE_FIGHT);
	intro(&s);
	while (s.timer < BANNER_END)
		step2(&s, 0, 0);
	s.p[0].x = QX(200);
	s.p[1].x = QX(250);
	step2(&s, K(Y), 0);
	while (!s.last[0])
		step2(&s, 0, 0);
	snapshot(&s, "practice");
	/* A KOF hit spark on Korea and a slash with blood on the Mizuki shrine,
	 * two ticks after the contact. */
	static const char *const stages[2] = {"KOREA", "MIZUKI"};
	for (int k = 0; k < 2; k++) {
		unsigned arena = stage_index(stages[k]);
		int centre = pack.stage[arena].width / 2;
		staged(&s, arena, k ? haohmaru : terry, terry, centre - 30, centre + 30);
		int n = 1;
		for (; n <= 40 && !s.hits; n++)
			run(&s, n, K(X), 0, 0);
		assert(s.hits);
		step2(&s, 0, 0);
		step2(&s, 0, 0);
		snapshot(&s, k ? "mizuki" : "korea");
	}
	/* A crouch LP chain's count on screen. */
	unsigned korea = stage_index("KOREA");
	int centre = pack.stage[korea].width / 2;
	staged(&s, korea, terry, terry, centre - 25, centre + 25);
	for (int n = 1; n <= 40 && s.shown[0] < 2; n++)
		step2(&s, K(DOWN) | (n % 2 ? K(Y) : 0), 0);
	assert(s.shown[0] >= 2);
	snapshot(&s, "combo");
	/* Terry's sweep: P1 falling, then lying. */
	staged(&s, korea, terry, terry, centre - 25, centre + 25);
	for (int n = 0; n < 60 && s.p[1].down != FALLING; n++)
		step2(&s, n ? K(DOWN) : K(DOWN) | K(A), 0);
	for (int n = 0; n < 20; n++)
		step2(&s, 0, 0);
	snapshot(&s, "falling");
	while (s.p[1].down == FALLING)
		step2(&s, 0, 0);
	step2(&s, 0, 0);
	snapshot(&s, "down");
	/* Terry throws Terry: held partway through Grasping Upper. */
	staged(&s, korea, terry, terry, centre - 23, centre + 23);
	step2(&s, K(RIGHT) | K(X), 0);
	assert(s.p[0].grab == GRAB_FORWARD);
	for (int n = 0; n < 12; n++)
		step2(&s, 0, 0);
	snapshot(&s, "throw");
	s.chr[0] = character("IORI");
	staged(&s, korea, s.chr[0], terry, centre - 60, centre + 60);
	step2(&s, K(START), 0);
	snapshot(&s, "pause");
}
static void logic_tests(const uint8_t *data, size_t len)
{
	int automatic;
	assert(fight_assets(&pack, data, len, &automatic));
	terry = 0;
	while (strcmp(pack.chr[terry].name, "TERRY"))
		assert(++terry < pack.characters);
	haohmaru = 0;
	while (strcmp(pack.chr[haohmaru].name, "HAOHMARU"))
		assert(++haohmaru < pack.characters);
	/* The rules on exact ticks run on the flat fallback arena (480 wide,
	 * the camera fixed at 0); the stage tests put the stages back. */
	unsigned stages = pack.stages;
	assert(stages >= 2);
	pack.stages = 0;
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
	follow_ups();
	ladder();
	full_ladder();
	roster_grid();
	game_modes();
	guard_dummy();
	cpu_guard();
	round_clock();
	round_start();
	combos();
	move_lists();
	difficulty();
	knockdowns();
	throws();
	dashes();
	state_words();
	stat_tests();
	pack.stages = stages;
	camera_tests();
	spark_sound_tests();
	stage_state();
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
	/* The bank next to the pack loads, so the replay below covers music
	 * and effects (built by make next to argv[1]). */
	assert(sound_banks == 1);
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
	assert(n == 16 + STATE_WORDS * 4 + AUDIO_STATE_BYTES);
	memcpy(copy, end, n);
	copy[8] ^= 1;
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTATE1", 8); /* FGSTATE1 from the fixed-sprite POC */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTATE2", 8); /* FGSTATE2, before boxes, moves and hitstop */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTATE3", 8); /* FGSTATE3, before buffer, specials, projectiles, ladder */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTATE4", 8); /* FGSTATE4, pixel x and fixed stats */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTATE5", 8); /* FGSTATE5, before the camera, sparks and sound events */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTATE6", 8); /* FGSTATE6, before the intro and follow-up inputs */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTATE7", 8); /* FGSTATE7, before READY and FIGHT */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTATE8", 8); /* FGSTATE8, before combos */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTATE9", 8); /* before the select command list */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTAT10", 8); /* before the difficulty */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTAT11", 8); /* before knockdowns */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTAT12", 8); /* before throws */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTAT13", 8); /* before run and backdash */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTAT14", 8); /* before the game modes */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTAT15", 8); /* before the clock and best of three */
	assert(!retro_unserialize(copy, n));
	memcpy(copy, end, n);
	memcpy(copy, "FGSTAT16", 8); /* before the 64-bit beaten mask */
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
	/* Port 1 drives the ladder's opponent once P1 picked a fighter and the
	 * versus card is over. */
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	retro_run();
	keys[0] = 0;
	for (int i = 0; i < ROUND_TICKS; i++)
		retro_run();
	keys[1] = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	retro_run();
	save(start, n);
	keys[1] = 0;
	retro_reset();
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	retro_run();
	keys[0] = 0;
	for (int i = 0; i <= ROUND_TICKS; i++)
		retro_run();
	save(end, n);
	assert(memcmp(start, end, n));
	/* A disconnected port must neither move nor attack. */
	retro_set_controller_port_device(1, RETRO_DEVICE_NONE);
	retro_reset();
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	retro_run();
	keys[0] = 0;
	for (int i = 0; i < ROUND_TICKS; i++)
		retro_run();
	keys[1] = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	retro_run();
	save(copy, n);
	assert(!memcmp(end, copy, n));
	retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
	keys[1] = 0;
	retro_unload_game();
	players = "cpu";
	/* The difficulty option: the state takes it at load and on the select
	 * screen, a running ladder keeps its level, reset applies the option. */
	difficulty_option = "hard";
	assert(retro_load_game(&g));
	retro_run();
	save(copy, n);
	assert(get_word(copy, LEVEL_W) == LEVEL_HARD);
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	retro_run();
	keys[0] = 0;
	difficulty_option = "easy";
	retro_run();
	save(copy, n);
	assert(get_word(copy, LEVEL_W) == LEVEL_HARD && !level_logs);
	retro_reset();
	save(copy, n);
	assert(get_word(copy, LEVEL_W) == LEVEL_EASY && level_logs == 1);
	/* R picks Normal on the select screen and it stays while the option
	 * does not change; a changed option is applied there again. */
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_R;
	retro_run();
	keys[0] = 0;
	retro_run();
	retro_run();
	save(copy, n);
	assert(get_word(copy, LEVEL_W) == LEVEL_NORMAL && level_logs == 2);
	difficulty_option = "hard";
	retro_run();
	save(copy, n);
	assert(get_word(copy, LEVEL_W) == LEVEL_HARD && level_logs == 3);
	/* A ladder run loaded with one opponent beaten after five rounds of
	 * the session, then quit: the log counts the run's rounds since the
	 * load and the opponent beaten before the quit cleared it. */
	keys[0] = 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	retro_run();
	keys[0] = 0;
	retro_run();
	save(copy, n);
	assert(get_word(copy, 8) == MODE_FIGHT && get_word(copy, 10) == 0);
	unsigned other = 0;
	while (other == get_word(copy, 15) || other == get_word(copy, 16))
		other++;
	const uint32_t patch[][2] = {{7, 5}, {10, 1u << other}, {11, 2}};
	for (int i = 0; i < 3; i++)
		put32(copy + 16 + 4 * patch[i][0], patch[i][1]);
	assert(retro_unserialize(copy, n));
	static const unsigned quit[] = {RETRO_DEVICE_ID_JOYPAD_START, 16, RETRO_DEVICE_ID_JOYPAD_B};
	for (int i = 0; i < 3; i++) {
		keys[0] = quit[i] < 16 ? 1u << quit[i] : 0;
		retro_run();
	}
	keys[0] = 0;
	save(copy, n);
	assert(get_word(copy, 8) == MODE_SELECT);
	if (!strstr(run_over, "arcade run over (quit) after 0 rounds, 1 wins")) {
		fprintf(stderr, "run over log: %s", run_over);
		abort();
	}
	retro_unload_game();
	difficulty_option = "normal";
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
	puts("fighter-test: pack decoder, box/move/special/projectile/spark/stage "
	     "validation, pixel-exact renderer and stage layers, box flip and overlay, "
	     "effect budget, camera and walls, sparks, sound events, content, "
	     "RGB565/XRGB8888, audio, ports, replay, pause, state validation, frame "
	     "data, hit/block/hitstop ticks, pushback and corner, guard heights, "
	     "trades, combos, knockdowns, throws, run and backdash, survival and practice, round clock, move lists, difficulty, commands, buffer, prejump, cancels, follow-ups, projectiles and clash, "
	     "specials, ladder, systems and stats, AI coverage and stress cycle "
	     "passed");
	return 0;
}
