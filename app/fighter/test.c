/* Native libretro callback harness: malformed content, deterministic state,
 * output formats, audio and a complete automated scene cycle, plus the rules
 * on exact ticks: frame data, hit/block/hitstop/hitstun timing, pushback and
 * corner transfer, guard heights, trades, state round-trip and rejection,
 * and the AI's use of every normal, with expected numbers taken from
 * assets/fighter/terry.json. test-render.c adds the pack decoder, box and
 * pixel-exact renderer checks. ASan/UBSan run this without a window or
 * device. */
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
static void duel(struct fight_state *s, int x0, int x1)
{
	fight_reset(s, 0);
	s->p[0].x = x0;
	s->p[1].x = x1;
}
/* n counts fight_step calls from 1; P0 presses `press` on call 1. */
static void run(struct fight_state *s, int n, uint32_t press, uint32_t hold0, uint32_t hold1)
{
	fight_step(s, &pack, (n == 1 ? press : 0) | hold0, hold1, 1);
}
static int in_action(const struct fight_state *s, int i, int action)
{
	const struct anim *an = &pack.chr[i].action[action];
	unsigned k = fight_pose(s, &pack, i);
	return k >= an->first && k < an->first + an->count;
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
	for (int p = 0; p < 2; p++)
		for (int k = 0; k < MOVES; k++) {
			const struct move *m = &pack.chr[p].move[k];
			const int *w = want[k];
			assert(m->startup == w[0] && m->active == w[1] && m->recovery == w[2] &&
			       m->anim.total == w[0] + w[1] + w[2] && m->damage == w[3] &&
			       m->chip == 0 && m->hitstun == w[4] && m->blockstun == w[5] &&
			       m->hitstop == w[6] && m->pushback == w[7] &&
			       m->block_pushback == w[8] && m->guard == w[9] && m->groups == 1);
		}
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
		fight_step(&s, &pack, 0, n == 1 ? K(Y) : 0, 1);
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
	pack.chr[0].move[CROUCH_LP].guard = GUARD_OVERHEAD;
	duel(&s, 200, 250);
	for (int n = 1; n <= 3; n++)
		run(&s, n, K(Y), K(DOWN), K(DOWN) | K(RIGHT));
	assert(!s.guards && s.p[1].hp == 975 && s.p[1].stun == 11 &&
	       in_action(&s, 1, CROUCH_HURT));
	duel(&s, 200, 250);
	for (int n = 1; n <= 3; n++)
		run(&s, n, K(Y), K(DOWN), K(RIGHT));
	assert(s.guards == 1 && s.p[1].hp == 1000 && !s.p[1].crouch);
	pack.chr[0].move[CROUCH_LP].guard = GUARD_HIGH;
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
		fight_step(&s, &pack, n == 1 ? K(Y) : 0, n == 1 ? K(Y) : 0, 1);
	assert(s.hits == 2 && s.p[0].hp == 970 && s.p[1].hp == 970 && s.p[0].stun == 12 &&
	       s.p[1].stun == 12 && s.p[0].freeze == 6 && s.p[1].freeze == 6 &&
	       !s.p[0].move && !s.p[1].move);
}
/* Mid-hitstop state round-trips, replays identically and rejects
 * impossible values without touching the destination. */
static void state_words(void)
{
	struct fight_state s, t, keep;
	uint32_t w[STATE_WORDS], v[STATE_WORDS], bad[STATE_WORDS];
	duel(&s, 200, 260);
	for (int n = 1; n <= 6; n++)
		run(&s, n, K(Y), 0, 0);
	assert(s.p[0].freeze == 4 && s.p[1].slide == 14);
	fight_encode(&s, w);
	assert(fight_decode(&t, &pack, w));
	fight_encode(&t, v);
	assert(!memcmp(w, v, sizeof(w)));
	for (int n = 7; n <= 40; n++) {
		uint32_t press = n % 5 ? 0 : K(X);
		fight_step(&s, &pack, press, K(LEFT), 1);
		fight_step(&t, &pack, press, K(LEFT), 1);
	}
	fight_encode(&s, w);
	fight_encode(&t, v);
	assert(!memcmp(w, v, sizeof(w)));
	/* Back to the mid-hitstop state for the rejection cases. */
	duel(&s, 200, 260);
	for (int n = 1; n <= 6; n++)
		run(&s, n, K(Y), 0, 0);
	fight_encode(&s, w);
	enum { W_X, W_Y, W_VY, W_HP, W_FACE, W_WALK, W_BLOCK, W_CROUCH, W_MOVE, W_ELAPSED,
	       W_HIT, W_STUN, W_FREEZE, W_SLIDE, W_KEYS };
	static const struct {
		int who, field;
		uint32_t value;
	} cases[] = {
	    {1, W_MOVE, MOVES + 1},     {1, W_MOVE, 1},		{1, W_FREEZE, MAX_HITSTOP + 1},
	    {1, W_STUN, MAX_STUN + 1},  {1, W_SLIDE, MAX_PUSH + 1}, {1, W_SLIDE, (uint32_t)-MAX_PUSH - 1},
	    {1, W_KEYS, 0x10000},	      {1, W_CROUCH, 2},		{1, W_BLOCK, 2},
	    {1, W_BLOCK, 3},	      {0, W_ELAPSED, 12},		{0, W_HIT, 2},
	    {0, W_STUN, 5},	      {0, W_BLOCK, 1},		{1, W_HIT, 1},
	    {1, W_ELAPSED, 256},	      {0, W_X, WALL_LEFT - 1},	{0, W_FACE, 0},
	    {1, W_HP, 1001},	      {0, W_WALK, 2},		{0, W_VY, 61},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		memcpy(bad, w, sizeof(w));
		bad[8 + cases[i].who * 15 + cases[i].field] = cases[i].value;
		keep = t;
		if (fight_decode(&t, &pack, bad)) {
			fprintf(stderr, "bad state case %zu accepted\n", i);
			abort();
		}
		assert(!memcmp(&keep, &t, sizeof(t)));
	}
	/* Crouching in the air and nonzero reserved words are impossible too. */
	memcpy(bad, w, sizeof(w));
	bad[8 + 15 + W_CROUCH] = 1;
	bad[8 + 15 + W_Y] = (uint32_t)-10;
	assert(!fight_decode(&t, &pack, bad));
	memcpy(bad, w, sizeof(w));
	bad[STATE_WORDS - 1] = 1;
	assert(!fight_decode(&t, &pack, bad));
}
/* The Auto pack's AI uses all eight normals, lands hits and guards at
 * both heights, and finishes rounds. Contacts are counted on the tick a
 * fighter enters hitstop with fresh stun. */
static void ai_coverage(void)
{
	struct fight_state s;
	unsigned started = 0, hits = 0, blocks = 0, crouch_blocks = 0, crouch_hits = 0;
	fight_reset(&s, 1);
	for (int t = 0; t < 12000; t++) {
		struct fight_state before = s;
		fight_step(&s, &pack, 0, 0, 0);
		for (int i = 0; i < 2; i++) {
			const struct fighter *p = &s.p[i];
			if (p->move && p->move != before.p[i].move)
				started |= 1u << (p->move - 1);
			if (p->stun && !p->elapsed && p->freeze && !before.p[i].freeze) {
				blocks += p->block == 1;
				hits += p->block != 1;
				crouch_blocks += p->block == 1 && p->crouch;
				crouch_hits += p->block != 1 && p->crouch;
			}
		}
	}
	assert(started == (1u << MOVES) - 1 && blocks > 20 && hits > 20 &&
	       crouch_blocks > 5 && crouch_hits > 5 && s.round > 0);
}
static void logic_tests(const uint8_t *data, size_t len)
{
	int automatic;
	assert(fight_assets(&pack, data, len, &automatic));
	frame_data();
	hit_timing();
	block_timing();
	corner();
	guard_types();
	trade();
	state_words();
	ai_coverage();
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
	keys[1] = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	retro_run();
	save(start, n);
	keys[1] = 0;
	retro_reset();
	retro_run();
	save(end, n);
	assert(memcmp(start, end, n));
	/* A disconnected port must neither move nor attack. */
	retro_set_controller_port_device(1, RETRO_DEVICE_NONE);
	keys[1] = 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	retro_reset();
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
	logic_tests(data, len);
	retro_unload_game();
	retro_deinit();
	free(start);
	free(end);
	free(copy);
	free(data);
	puts("fighter-test: pack decoder, box/move validation, pixel-exact renderer, "
	     "box flip and overlay, effect budget, content, RGB565/XRGB8888, audio, "
	     "ports, replay, pause, state validation, frame data, hit/block/hitstop "
	     "ticks, pushback and corner, guard heights, trades, AI coverage and "
	     "stress cycle passed");
	return 0;
}
