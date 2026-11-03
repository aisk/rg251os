/* Standard libretro fighter core: FIGHTP07 content, input, deterministic
 * FGSTATE7 saves, audio and software frames. Timing below measures work only;
 * the frontend owns pacing. Logs the system profiles, the roster with each
 * character's system and resolved stats, every move's frame data and class
 * (follow-ups with their parent and window) and the resident memory at
 * load, and the stages and spark sets. The render cache is filled a slice
 * at a time: each run names what the state shows, expands at most
 * CACHE_BUDGET pixels ahead, builds the scene and expands only what it
 * still missed before building it again, so a select cursor move costs
 * one character and a match's cast spreads over the versus card. Each fill
 * is logged when ready (runs taken, pixels, total and worst cache time per
 * run, frames expanded on demand, fills superseded and the resident
 * regions), and a state loaded mid-fight logs its blocking fill. Reports
 * each scene (0 = two fighters, 1-3 = +8, +24, +48 requested alpha/additive
 * effects, capped by the per-frame effect pixel budget) every 600 runs,
 * also with debug off: logic/render/audio/cache times, how many effects
 * were drawn of those requested, and the ladder and special/clash
 * counters.
 *
 * Sound comes from the bank next to the content (<pack>.fightsnd, found
 * from retro_game_info's path; audio.c): after each unpaused tick the rule
 * events and the mode pick effects and music, and 800 mixed frames go out
 * per run (silence without a bank, which only logs why). The save state is
 * the rule state followed by the audio block. At load it logs the bank
 * (size, CRC, SFX, tracks, cues, resident memory, load time) and each
 * track; every 600 runs the voices started and stolen, the peak, the music
 * position and loops and the MP2 frames, seeks and decode time. Options
 * fight_volume, fight_music_volume and fight_sfx_volume set the levels.
 *
 * Buttons (RetroPad ids; the RG351P shell labels match, SNES layout):
 * Y (left) light punch, X (top) heavy punch, B (bottom) light kick,
 * A (right) heavy kick; hold DOWN for the crouching version. Specials are
 * motions plus a punch (Terry: 236 Power Wave, 214 Burn Knuckle, hold down
 * then up Rising Tackle; Iori's 214 Aoi Hana takes 214+P twice more as
 * it hits). Back guards (down-back guards low), UP jumps, L
 * toggles the hurt/hit box overlay, R cycles the stress scene, START
 * pauses; A or B skips the KO screen. In the select screen the d-pad moves
 * and any attack button picks. The desktop option fight_players=two lets
 * port 1 drive the ladder's opponent; the device only has port 0. */
#include "libretro.h"
#include "audio.h"
#include "fighter.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static retro_environment_t env;
static retro_video_refresh_t video;
static retro_audio_sample_batch_t batch;
static retro_audio_sample_t sample;
static retro_input_poll_t poll_input;
static retro_input_state_t input;
static retro_log_printf_t logger;
static struct assets assets;
static struct fight_state state;
static struct scene scene;
static uint16_t fb[FW * FH];
static uint32_t xrgb[FW * FH];
static int16_t audio[1600];
/* The sound bank, mixer and music next to the pack (audio.c). */
static struct audio sound;
static char sound_path[4096];
static int loaded, format32, two_players;
static unsigned devices[2] = {RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
static unsigned benchmark_runs;
static uint64_t samples[4][600];
static unsigned sample_count, scene_mode;
static uint64_t effects_requested, effects_drawn, sparks_drawn, layers_drawn;
static unsigned effects_peak;
/* Render cache work per run: at most CACHE_BUDGET pixels of prefetch plus
 * the frames the scene needs right away (render.c). */
#define CACHE_BUDGET 49152u
/* The current cast's fill: runs, cache time, pixels and frames expanded on
 * demand since it was asked for, and casts dropped before they were ready. */
static struct {
	int active;
	unsigned runs, demand, superseded;
	uint64_t us, max_us;
	uint32_t pixels;
} fill;
static uint64_t tick_us(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return 0;
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000;
}
static int compare(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}
static const char *side_name(int i, char *buf, size_t n)
{
	if (!(assets.wanted >> i & 1) || assets.cast[i][0] < 0)
		return "-";
	snprintf(buf, n, "%s palette %d", assets.chr[assets.cast[i][0]].name, assets.cast[i][1]);
	return buf;
}
/* Point the cache at the state; a change starts a new fill. */
static void want(void)
{
	if (!fight_want(&assets, &state))
		return;
	if (fill.active)
		fill.superseded++;
	fill.active = 1;
	fill.runs = fill.demand = 0;
	fill.us = fill.max_us = 0;
	fill.pixels = 0;
}
/* Count one run's cache work; log the fill once everything is expanded. */
static void filled(uint64_t us, uint32_t pixels)
{
	if (!fill.active)
		return;
	fill.runs++;
	fill.us += us;
	fill.max_us = us > fill.max_us ? us : fill.max_us;
	fill.pixels += pixels;
	if (!fight_ready(&assets))
		return;
	char p1[32], p2[32];
	if (logger)
		logger(RETRO_LOG_INFO,
		       "fight: cast %s vs %s on %s ready in %u runs (%s): expanded %u KiB, cache us "
		       "total %llu max %llu per run, %u frames on demand, %u casts superseded; "
		       "resident RGB565 KiB p1 %u p2 %u stage %u effects %u\n",
		       side_name(0, p1, sizeof(p1)), side_name(1, p2, sizeof(p2)),
		       (assets.wanted & 4) && assets.cast_stage >= 0
			   ? assets.stage[assets.cast_stage].name
			   : "-",
		       fill.runs, state.mode == MODE_FIGHT ? "fight" : "screen",
		       (unsigned)(fill.pixels * 2 / 1024), (unsigned long long)fill.us,
		       (unsigned long long)fill.max_us, fill.demand, fill.superseded,
		       assets.cast_pixels[0] * 2 / 1024, assets.cast_pixels[1] * 2 / 1024,
		       assets.stage_pixels * 2 / 1024, assets.effect_pixels * 2 / 1024);
	fill.active = 0;
	fill.superseded = 0;
}
static void report(void)
{
	if (!sample_count)
		return;
	if (logger) {
		const char *names[] = {"logic", "render", "audio", "cache"};
		for (int i = 0; i < 4; i++) {
			uint64_t sum = 0;
			for (unsigned j = 0; j < sample_count; j++)
				sum += samples[i][j];
			qsort(samples[i], sample_count, sizeof(uint64_t), compare);
			logger(RETRO_LOG_INFO,
			       "fight: scene %u %s us avg/p95/p99/max "
			       "%llu/%llu/%llu/%llu, %u runs\n",
			       scene_mode, names[i],
			       (unsigned long long)(sum / sample_count),
			       (unsigned long long)
				   samples[i][(sample_count - 1) * 95 / 100],
			       (unsigned long long)
				   samples[i][(sample_count - 1) * 99 / 100],
			       (unsigned long long)samples[i][sample_count - 1],
			       sample_count);
		}
		logger(RETRO_LOG_INFO,
		       "fight: scene %u effects drawn/requested %llu/%llu, "
		       "dropped %llu, peak %u per frame, budget %u px, sparks %llu, stage "
		       "sprites %llu, stage %s cam %u, %u runs\n",
		       scene_mode, (unsigned long long)effects_drawn,
		       (unsigned long long)effects_requested,
		       (unsigned long long)(effects_requested - effects_drawn),
		       effects_peak, EFFECT_PIXEL_BUDGET, (unsigned long long)sparks_drawn,
		       (unsigned long long)layers_drawn,
		       assets.cast_stage >= 0 ? assets.stage[assets.cast_stage].name : "FLAT",
		       state.cam, sample_count);
		logger(RETRO_LOG_INFO,
		       "fight: tick %u round %u hits %u guards %u specials %u clashes %u "
		       "mode %u run %u stage %u beaten %08x %s vs %s; frontend perf "
		       "measures rotate/present/audio output\n",
		       state.tick, state.round, state.hits, state.guards, state.specials,
		       state.clashes, state.mode, state.run, state.stage, state.beaten,
		       assets.chr[state.chr[0]].name, assets.chr[state.chr[1]].name);
	}
	if (logger && sound.bank) {
		const struct audio_stats *st = &sound.stats;
		unsigned frames = st->frames ? st->frames : 1;
		logger(RETRO_LOG_INFO,
		       "fight: scene %u sound starts %u steals %u silent %u voices peak %u; "
		       "music %s pos %u loops %u, MP2 frames %u seeks %u restores %u decode "
		       "us avg/max %llu/%llu, %u runs\n",
		       scene_mode, st->starts, st->steals, st->silent, st->peak,
		       sound.s.track ? sound.track[sound.s.track - 1].name : "none",
		       sound.s.pos, sound.s.loops, st->frames, st->seeks, st->restores,
		       (unsigned long long)(st->decode_us / frames),
		       (unsigned long long)st->decode_max_us, sample_count);
	}
	audio_stats_reset(&sound);
	sample_count = 0;
	effects_requested = effects_drawn = sparks_drawn = layers_drawn = 0;
	effects_peak = 0;
}
void retro_set_environment(retro_environment_t cb)
{
	env = cb;
	static const struct retro_variable vars[] = {
	    {"fight_players", "Players; cpu|two"},
	    {"fight_volume", "Master volume; 100|90|80|70|60|50|40|30|20|10|0"},
	    {"fight_music_volume", "Music volume; 60|70|80|90|100|0|10|20|30|40|50"},
	    {"fight_sfx_volume", "Effects volume; 100|90|80|70|60|50|40|30|20|10|0"},
	    {NULL, NULL}};
	if (env)
		env(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { sample = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { batch = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_input = cb; }
void retro_set_input_state(retro_input_state_t cb) { input = cb; }
void retro_init(void)
{
	struct retro_log_callback log = {0};
	logger = NULL;
	if (env && env(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
		logger = log.log;
}
void retro_deinit(void)
{
	report();
	audio_close(&sound);
	loaded = 0;
}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_get_system_info(struct retro_system_info *i)
{
	memset(i, 0, sizeof(*i));
	i->library_name = "Fighter P0";
	i->library_version = "0.4";
	i->valid_extensions = "fight";
	i->block_extract = true;
}
void retro_get_system_av_info(struct retro_system_av_info *i)
{
	memset(i, 0, sizeof(*i));
	i->geometry = (struct retro_game_geometry){FW, FH, FW, FH, 1.5f};
	i->timing.fps = 60;
	i->timing.sample_rate = 48000;
}
/* A volume option in percent; anything but 0..100 gives the default. */
static unsigned volume(const char *key, unsigned fallback)
{
	struct retro_variable v = {key, NULL};
	char *end;
	if (!env || !env(RETRO_ENVIRONMENT_GET_VARIABLE, &v) || !v.value || !*v.value)
		return fallback;
	unsigned long n = strtoul(v.value, &end, 10);
	return *end || n > 100 ? fallback : (unsigned)n;
}
static void options(void)
{
	struct retro_variable v = {"fight_players", NULL};
	two_players = env && env(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value &&
		      !strcmp(v.value, "two");
	sound.master = volume("fight_volume", 100);
	sound.music_level = volume("fight_music_volume", 60);
	sound.sfx_level = volume("fight_sfx_volume", 100);
}
/* Load the sound bank next to the content; without it the game is silent. */
static void open_sound(const struct retro_game_info *g)
{
	char why[160];
	uint64_t t0 = tick_us();
	if (!audio_open(&sound, g->path, &assets, why, sizeof(why), sound_path,
			sizeof(sound_path))) {
		if (logger)
			logger(RETRO_LOG_WARN, "fight: no sound: bank %s: %s; playing silent\n",
			       *sound_path ? sound_path : "(none)", why);
		return;
	}
	if (!logger)
		return;
	uint64_t adpcm = 0, clip_ms = 0, mp2 = 0, seconds = 0;
	for (unsigned i = 0; i < sound.clips; i++) {
		adpcm += (sound.clip[i].samples + 255) / 256 * 132;
		clip_ms += sound.clip[i].samples * 1000ull / sound.clip[i].rate;
	}
	for (unsigned i = 0; i < sound.tracks; i++) {
		mp2 += (uint64_t)sound.track[i].frames * sound.track[i].frame_bytes;
		seconds += sound.track[i].samples / AUDIO_RATE;
	}
	logger(RETRO_LOG_INFO,
	       "fight: sound bank %s %u KiB crc %08x loaded in %llu ms: %u SFX (%llu KiB "
	       "ADPCM, %llu.%llu s), %u tracks (%llu KiB MP2, %llu s), %u cues, %u music "
	       "cues, %u unmatched scopes; resident %u KiB; volume %u music %u sfx %u\n",
	       sound_path, (unsigned)(sound.size / 1024), sound.crc,
	       (unsigned long long)((tick_us() - t0) / 1000), sound.clips,
	       (unsigned long long)(adpcm / 1024), (unsigned long long)(clip_ms / 1000),
	       (unsigned long long)(clip_ms % 1000 / 100), sound.tracks,
	       (unsigned long long)(mp2 / 1024), (unsigned long long)seconds, sound.cues,
	       sound.music, sound.unmatched, (unsigned)(audio_resident(&sound) / 1024),
	       sound.master, sound.music_level, sound.sfx_level);
	for (unsigned i = 0; i < sound.tracks; i++)
		logger(RETRO_LOG_INFO,
		       "fight: track %u %s %u frames x %u bytes, %s %u..%u, level %u\n", i,
		       sound.track[i].name, sound.track[i].frames, sound.track[i].frame_bytes,
		       sound.track[i].loops ? "loop" : "once", sound.track[i].loop_start,
		       sound.track[i].loop_end, sound.track[i].level);
}
bool retro_load_game(const struct retro_game_info *g)
{
	int automatic = 0;
	loaded = 0;
	if (!g || !fight_assets(&assets, g->data, g->size, &automatic)) {
		if (logger)
			logger(RETRO_LOG_ERROR,
			       "fight: invalid FIGHTP07 package (magic, length, hash, "
			       "limits, frame, palette, system, stats, character, move, "
			       "projectile or step data)\n");
		return false;
	}
	enum retro_pixel_format f = RETRO_PIXEL_FORMAT_RGB565;
	format32 = 0;
	if (!env || !env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f)) {
		f = RETRO_PIXEL_FORMAT_XRGB8888;
		if (!env || !env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f))
			return false;
		format32 = 1;
	}
	fight_reset(&state, automatic);
	options();
	open_sound(g);
	sample_count = 0;
	effects_requested = effects_drawn = 0;
	effects_peak = 0;
	scene_mode = 0;
	loaded = 1;
	benchmark_runs = 0;
	if (logger) {
		logger(RETRO_LOG_INFO,
		       "fight: FIGHTP07 package %08x, %u characters, %u frames, %u "
		       "palettes, %u steps, %u boxes, 480x320 60Hz 48kHz, software %s, %s; "
		       "L boxes R stress START pause\n",
		       assets.hash, assets.characters, assets.frames, assets.palettes,
		       assets.steps, assets.boxes, format32 ? "XRGB8888" : "RGB565",
		       automatic ? "automatic ladder, 4-scene cycle" : "arcade ladder vs CPU");
		/* Resident bytes in use (the static limits are larger). */
		unsigned rules = (unsigned)(assets.steps * sizeof(struct step) +
					    assets.boxes * sizeof(struct box) +
					    assets.characters * sizeof(struct character));
		logger(RETRO_LOG_INFO,
		       "fight: resident KiB frame indices %u spans %u rows %u rules %u "
		       "effects RGB565 %u (%u effects); static limit %u KiB\n",
		       assets.indices / 1024,
		       (unsigned)(assets.spans * sizeof(struct span) / 1024),
		       (unsigned)(assets.rows * 4 / 1024), rules / 1024,
		       assets.effect_pixels * 2 / 1024, assets.effects,
		       (unsigned)(sizeof(assets) / 1024));
		logger(RETRO_LOG_INFO, "fight: %u spark anims (%u steps), %u stages%s\n",
		       assets.sparks, assets.spark_steps, assets.stages,
		       assets.stages ? "" : ", flat background");
		for (unsigned k = 0; k < assets.stages; k++) {
			const struct stage *st = &assets.stage[k];
			unsigned anims = 0;
			for (unsigned i = 0; i < st->layers; i++)
				anims += assets.layer[st->first_layer + i].anims;
			logger(RETRO_LOG_INFO,
			       "fight: stage %u %s width %u ground %u system %s, %u layers, %u "
			       "anims, expands to %u KiB RGB565 in %u images, %u spans\n",
			       k, st->name, st->width, st->ground, assets.sys[st->system].name,
			       st->layers, anims, st->pixels * 2 / 1024, st->images, st->spans);
		}
		static const char *const names[MOVES] = {
		    "stand LP", "stand HP", "stand LK", "stand HK",
		    "crouch LP", "crouch HP", "crouch LK", "crouch HK"};
		static const char *const motions[MOTIONS] = {
		    "236", "214", "623", "[4]6", "[2]8", "any", "5", "8", "2", "6", "4",
		    "41236", "63214", "421"};
		static const char *const classes[CLASSES] = {
		    "none", "light", "heavy", "command", "special", "super", "throw"};
		static const char *const heights[HEIGHTS] = {"low", "mid", "high"};
		for (unsigned k = 0; k < assets.systems; k++) {
			const struct system *sy = &assets.sys[k];
			const struct stats *st = &sy->stats;
			logger(RETRO_LOG_INFO,
			       "fight: system %u %s damage scale %u push %d guard range %d "
			       "input buffer/236/623/charge/grace/release %u/%u/%u/%u/%u/%u\n",
			       k, sy->name, sy->damage_scale, st->push, st->guard_range,
			       st->buffer, st->quarter, st->dragon, st->charge, st->grace,
			       st->release);
		}
		for (unsigned c = 0; c < assets.characters; c++) {
			const struct character *ch = &assets.chr[c];
			const struct stats *st = &ch->stats;
			logger(RETRO_LOG_INFO,
			       "fight: character %u %s, %u moves, %u projectiles, cast "
			       "RGB565 %u KiB in %u images\n",
			       c, ch->name, ch->moves, ch->projectiles, ch->pixels * 2 / 1024,
			       ch->images);
			logger(RETRO_LOG_INFO,
			       "fight: %s system %s life %d defense %d weight %d height %s "
			       "walk %d/%d jump vy %d gravity %d drift %d/%d prejump %d "
			       "peak %d\n",
			       ch->name, assets.sys[ch->system].name, st->life, st->defense,
			       st->weight, heights[st->height], st->walk, st->walk_back,
			       st->jump.vy, st->jump.gravity, st->jump.vx, st->jump.vx_back,
			       st->jump.prejump, st->jump.peak);
			for (unsigned k = 0; k < ch->moves; k++) {
				const struct move *m = &ch->move[k];
				char special[32], window[64] = "";
				if (m->special)
					snprintf(special, sizeof(special), "special %s+%x",
						 motions[m->motion], m->buttons);
				/* A follow-up names its parent's move index. */
				if (m->after)
					snprintf(window, sizeof(window),
						 ", follow-up of move %u window %u..%u%s", m->after - 1u,
						 m->open, m->close, m->contact ? " on contact" : "");
				logger(RETRO_LOG_INFO,
				       "fight: %s %s startup/active/recovery %u/%u/%u "
				       "damage %u chip %u hit/block/stop %u/%u/%u push %u/%u "
				       "guard %u reach %d cancel %u spawn %u class %s%s\n",
				       ch->name, m->special ? special : names[k], m->startup,
				       m->active, m->recovery, m->hit.damage, m->hit.chip,
				       m->hit.hitstun, m->hit.blockstun, m->hit.hitstop,
				       m->hit.pushback, m->hit.block_pushback, m->hit.guard,
				       m->reach, m->cancel, m->spawn, classes[m->hit.cls], window);
			}
		}
	}
	fill.active = 0;
	fill.superseded = 0;
	want();
	return true;
}
void retro_unload_game(void)
{
	report();
	audio_close(&sound);
	loaded = 0;
}
void retro_reset(void)
{
	report();
	fight_reset(&state, state.automatic);
	audio_reset(&sound);
	want();
	benchmark_runs = 0;
}
void retro_run(void)
{
	if (!loaded)
		return;
	bool update = false;
	if (env && env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &update) && update)
		options();
	if (poll_input)
		poll_input();
	uint32_t keys[2] = {0, 0};
	if (input)
		for (unsigned p = 0; p < 2; p++)
			for (unsigned k = 0;
			     devices[p] == RETRO_DEVICE_JOYPAD && k < 16; k++)
				if (input(p, RETRO_DEVICE_JOYPAD, 0, k))
					keys[p] |= 1u << k;
	uint32_t old_tick = state.tick;
	uint64_t t0 = tick_us();
	fight_step(&state, &assets, keys[0], keys[1], two_players);
	uint64_t t1 = tick_us();
	if (state.automatic && state.tick != old_tick && ++benchmark_runs == 36000 &&
	    logger)
		logger(
		    RETRO_LOG_INFO,
		    "fight: benchmark complete: 36000 active runs this session, "
		    "15 cycles; collect frontend.txt and perf.csv; scenes continue\n");
	if (state.stress != scene_mode) {
		report();
		scene_mode = state.stress;
	}
	/* The cache: prefetch within the budget, build the scene, and if it
	 * still misses frames (or the stage), expand those and build it again. */
	uint64_t c0 = tick_us();
	want();
	uint32_t cached = fight_prefetch(&assets, CACHE_BUDGET);
	uint64_t c1 = tick_us();
	fight_scene(&state, &assets, &scene);
	uint64_t d0 = tick_us(), d1 = d0;
	if (scene.misses || scene.stage_missing) {
		fill.demand += scene.misses;
		cached += fight_demand(&assets, &scene);
		d1 = tick_us();
		fight_scene(&state, &assets, &scene);
	}
	uint64_t cache_us = (c1 - c0) + (d1 - d0);
	filled(cache_us, cached);
	effects_requested += scene.effects_requested;
	effects_drawn += scene.effects_drawn;
	sparks_drawn += scene.sparks;
	layers_drawn += scene.layers;
	if (scene.effects_requested > effects_peak)
		effects_peak = scene.effects_requested;
	fight_render(&assets, &scene, fb);
	if (format32)
		for (int i = 0; i < FW * FH; i++) {
			unsigned c = fb[i], r = c >> 11, g = (c >> 5) & 63, b = c & 31;
			xrgb[i] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) |
				  (b << 3 | b >> 2);
		}
	uint64_t t2 = tick_us();
	if (!state.paused)
		audio_tick(&sound, &state, &assets);
	audio_mix(&sound, audio, 800, !state.paused);
	uint64_t t3 = tick_us();
	samples[0][sample_count] = t1 - t0;
	samples[1][sample_count] = t2 - c0 - cache_us;
	samples[2][sample_count] = t3 - t2;
	samples[3][sample_count] = cache_us;
	if (++sample_count == 600)
		report();
	if (video)
		video(format32 ? (const void *)xrgb : (const void *)fb, FW, FH,
		      FW * (format32 ? 4 : 2));
	if (batch)
		batch(audio, 800);
	else if (sample)
		for (int i = 0; i < 800; i++)
			sample(audio[i * 2], audio[i * 2 + 1]);
}
static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
/* The rule state, then the audio block (audio.h). */
size_t retro_serialize_size(void) { return 16 + STATE_WORDS * 4 + AUDIO_STATE_BYTES; }
bool retro_serialize(void *data, size_t size)
{
	if (!loaded || !data || size < retro_serialize_size())
		return false;
	uint8_t *p = data;
	uint32_t w[STATE_WORDS];
	fight_encode(&state, w);
	memcpy(p, "FGSTATE7", 8);
	put32(p + 8, assets.hash);
	put32(p + 12, state.automatic);
	for (int i = 0; i < STATE_WORDS; i++)
		put32(p + 16 + 4 * i, w[i]);
	audio_encode(&sound, p + 16 + STATE_WORDS * 4);
	return true;
}
bool retro_unserialize(const void *data, size_t size)
{
	const uint8_t *p = data;
	uint32_t w[STATE_WORDS];
	if (!loaded || !p || size != retro_serialize_size() ||
	    memcmp(p, "FGSTATE7", 8) || get32(p + 8) != assets.hash ||
	    get32(p + 12) != state.automatic)
		return false;
	for (int i = 0; i < STATE_WORDS; i++)
		w[i] = get32(p + 16 + 4 * i);
	if (w[6] != state.automatic)
		return false;
	struct fight_state restored;
	/* The audio block applies only after the rule state checked out; a
	 * block saved with another bank (or none) restores as silence. */
	if (!fight_decode(&restored, &assets, w) ||
	    !audio_decode(&sound, p + 16 + STATE_WORDS * 4))
		return false;
	report();
	state = restored;
	want();
	/* Mid-fight the scene needs the whole stage at once: fill everything
	 * now rather than over the next runs. */
	if (state.mode == MODE_FIGHT && state.timer >= INTRO_TICKS && !fight_ready(&assets)) {
		uint64_t t0 = tick_us();
		uint32_t n = fight_prefetch(&assets, UINT32_MAX);
		uint64_t us = tick_us() - t0;
		if (logger)
			logger(RETRO_LOG_INFO,
			       "fight: state loaded mid-fight, blocking cache fill %u KiB in %llu us\n",
			       (unsigned)(n * 2 / 1024), (unsigned long long)us);
		filled(us, n);
	}
	return true;
}
void retro_set_controller_port_device(unsigned port, unsigned device)
{
	if (port < 2)
		devices[port] =
		    device == RETRO_DEVICE_JOYPAD ? device : RETRO_DEVICE_NONE;
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id)
{
	(void)id;
	return NULL;
}
size_t retro_get_memory_size(unsigned id)
{
	(void)id;
	return 0;
}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool enabled, const char *code)
{
	(void)i;
	(void)enabled;
	(void)code;
}
bool retro_load_game_special(unsigned type, const struct retro_game_info *g, size_t n)
{
	(void)type;
	(void)g;
	(void)n;
	return false;
}
