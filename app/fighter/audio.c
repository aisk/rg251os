/* The fighter's sound (see audio.h): reads and checks a FIGHTSND bank
 * (header, CRC-32, every table entry, every ADPCM block header and every
 * MP2 frame header), resolves its cue scopes against the pack's systems,
 * characters and (music only) stages, turns each tick's sound events into voices (the most
 * specific matching cue of each kind: character + move, character, system
 * + move, system, any + move, any; the variant picked from the tick),
 * follows the game mode with the music (select, fight by the arena's stage
 * or else the opponent's system and the ladder stage, a fade on KO into the winner's jingle,
 * continue, clear) and mixes 8 voices (ADPCM decoded a 256-sample block at
 * a time, linear interpolation to 48 kHz, panned) and the music with the
 * master, music and effect levels, saturating to int16. */
#include "audio.h"
#include "mp2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLOCK 256
#define BLOCK_BYTES (4 + BLOCK / 2)
#define CHUNK 800
#define KIND_SFX 0
#define KIND_VOICE 1
#define KIND_ANNOUNCER 2
#define FLAG_OTHER 4
/* Music fades out over this many output frames on a KO. */
#define KO_FADE (AUDIO_RATE * 3 / 4)
#define ANY_SYSTEM 0xffffffffu
/* No arena: music rows scoped to a stage never match. */
#define NO_ARENA (-1)

static const int16_t steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,
    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,
    73,    80,    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int8_t index_step[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p)
{
	return get16(p) | get16(p + 2) << 16;
}
static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t now_us(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return 0;
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000;
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
	static uint32_t table[256];
	if (!table[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	uint32_t c = 0xffffffffu;
	while (n--)
		c = table[(c ^ *p++) & 255] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

void audio_adpcm_block(const uint8_t *block, int16_t *out, unsigned count)
{
	int pred = (int16_t)get16(block), index = block[2] > 88 ? 88 : block[2];
	for (unsigned i = 0; i < count && i < BLOCK; i++) {
		unsigned code = (block[4 + i / 2] >> (i & 1 ? 4 : 0)) & 15;
		int step = steps[index], d = step >> 3;
		if (code & 4)
			d += step;
		if (code & 2)
			d += step >> 1;
		if (code & 1)
			d += step >> 2;
		pred += code & 8 ? -d : d;
		pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
		index += index_step[code & 7];
		index = index < 0 ? 0 : index > 88 ? 88 : index;
		out[i] = (int16_t)pred;
	}
}

void audio_close(struct audio *au)
{
	free(au->bank);
	mp2_destroy(au->mp2);
	au->bank = NULL;
	au->mp2 = NULL;
	au->size = 0;
	au->crc = 0;
	au->clips = au->tracks = au->cues = au->music = au->unmatched = 0;
	audio_reset(au);
}

void audio_reset(struct audio *au)
{
	memset(&au->s, 0, sizeof(au->s));
	for (int i = 0; i < AUDIO_VOICES; i++)
		au->block[i] = -1;
	au->mp2_track = 0;
	au->frame = UINT32_MAX;
}

void audio_stats_reset(struct audio *au) { memset(&au->stats, 0, sizeof(au->stats)); }

static int fail(char *why, size_t n, const char *text, unsigned index)
{
	if (why && n)
		snprintf(why, n, text, index);
	return 0;
}

/* A NUL-terminated name in a 16-byte field; empty allowed when may_empty. */
static int name_ok(const uint8_t *p, int may_empty)
{
	size_t n = strnlen((const char *)p, 16);
	if (n == 16 || (!n && !may_empty))
		return 0;
	for (size_t i = n; i < 16; i++)
		if (p[i])
			return 0;
	return 1;
}

static void scope(const uint8_t *p, const struct assets *a, uint8_t *kind, uint8_t *index,
		  unsigned *unmatched)
{
	const char *s = (const char *)p;
	*kind = 3;
	*index = 0;
	if (!*s) {
		*kind = 0;
		return;
	}
	for (unsigned k = 0; k < a->systems; k++)
		if (!strcmp(s, a->sys[k].name)) {
			*kind = 1;
			*index = (uint8_t)k;
			return;
		}
	for (unsigned c = 0; c < a->characters; c++)
		if (!strcmp(s, a->chr[c].name)) {
			*kind = 2;
			*index = (uint8_t)c;
			return;
		}
	for (unsigned k = 0; k < a->stages; k++)
		if (!strcmp(s, a->stage[k].name)) {
			*kind = 4;
			*index = (uint8_t)k;
			return;
		}
	++*unmatched;
}

static const unsigned mp2_kbps[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128,
				      160, 192, 224, 256, 320, 384};

int audio_bank(struct audio *au, uint8_t *d, size_t size, const struct assets *a,
	       char *why, size_t n)
{
	audio_close(au);
	if (!d)
		return fail(why, n, "no data%.0u", 0);
	if (size < 32 || memcmp(d, "FIGHTSND", 8) || get32(d + 8) != 1) {
		free(d);
		return fail(why, n, "not a FIGHTSND version 1 bank%.0u", 0);
	}
	if (get32(d + 12) != size || size > AUDIO_MAX_BANK) {
		free(d);
		return fail(why, n, "length differs from the header%.0u", 0);
	}
	uint32_t crc = crc32(d + 20, size - 20);
	if (crc != get32(d + 16)) {
		free(d);
		return fail(why, n, "CRC-32 mismatch%.0u", 0);
	}
	unsigned clips = get16(d + 20), tracks = get16(d + 22), cues = get16(d + 24),
		 music = get16(d + 26);
	size_t data = get32(d + 28),
	       table = 32 + 32 * (size_t)clips + 48 * (size_t)tracks + 32 * (size_t)cues +
		       32 * (size_t)music;
	int ok = 0;
	if (clips > AUDIO_MAX_CLIPS || tracks > AUDIO_MAX_TRACKS || cues > AUDIO_MAX_CUES ||
	    music > AUDIO_MAX_MUSIC) {
		fail(why, n, "too many clips, tracks or cues%.0u", 0);
		goto out;
	}
	if (data < table || data > size) {
		fail(why, n, "tables overlap the data area%.0u", 0);
		goto out;
	}
	const uint8_t *p = d + 32;
	for (unsigned i = 0; i < clips; i++, p += 32) {
		struct clip *c = &au->clip[i];
		uint32_t off = get32(p + 16), samples = get32(p + 20), rate = get32(p + 24);
		uint64_t blocks = ((uint64_t)samples + BLOCK - 1) / BLOCK;
		if (!name_ok(p, 0) || get32(p + 28) || rate < 4000 || rate > AUDIO_RATE ||
		    !samples || samples > AUDIO_MAX_CLIP_SAMPLES || off < data ||
		    off + blocks * BLOCK_BYTES > size) {
			fail(why, n, "clip %u: bad name, rate, length or offset", i);
			goto out;
		}
		for (uint64_t b = 0; b < blocks; b++)
			if (d[off + b * BLOCK_BYTES + 2] > 88 || d[off + b * BLOCK_BYTES + 3]) {
				fail(why, n, "clip %u: bad ADPCM block header", i);
				goto out;
			}
		memcpy(c->name, p, 16);
		c->data = d + off;
		c->samples = samples;
		c->rate = rate;
		c->step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_RATE);
	}
	for (unsigned i = 0; i < tracks; i++, p += 48) {
		struct track *t = &au->track[i];
		uint32_t off = get32(p + 16), frames = get32(p + 20), fb = get32(p + 24),
			 ls = get32(p + 28), le = get32(p + 32), flags = get32(p + 36),
			 level = get32(p + 40);
		uint64_t samples = (uint64_t)frames * MP2_SAMPLES;
		int loops = flags & 1;
		if (!name_ok(p, 0) || get32(p + 44) || (flags & ~1u) || !frames || fb < 96 ||
		    fb > 1152 || off < data || off + (uint64_t)frames * fb > size ||
		    samples > UINT32_MAX / 2 || level > 4000 ||
		    (loops ? ls >= le || le > samples || le - ls < MP2_SAMPLES : ls || le)) {
			fail(why, n, "track %u: bad name, frames, loop or offset", i);
			goto out;
		}
		for (uint32_t f = 0; f < frames; f++) {
			const uint8_t *h = d + off + (size_t)f * fb;
			unsigned kbps = mp2_kbps[h[2] >> 4 < 15 ? h[2] >> 4 : 0];
			if (h[0] != 0xff || (h[1] & 0xfe) != 0xfc || ((h[2] >> 2) & 3) != 1 ||
			    (h[2] & 2) || !kbps || 144 * kbps / 48 != fb) {
				fail(why, n, "track %u: not MPEG-1 Layer II 48 kHz frames of "
					     "one size", i);
				goto out;
			}
		}
		memcpy(t->name, p, 16);
		t->data = d + off;
		t->frames = frames;
		t->frame_bytes = fb;
		t->samples = (uint32_t)samples;
		t->loops = (uint32_t)loops;
		t->loop_start = ls;
		t->loop_end = le;
		t->level = level;
	}
	au->unmatched = 0;
	for (unsigned i = 0; i < cues; i++, p += 32) {
		struct cue *c = &au->cue[i];
		unsigned count = p[19];
		if (!name_ok(p, 1) || p[16] >= SOUNDS || (p[17] & ~7u) || (p[17] & 3) > 2 ||
		    p[18] > MAX_MOVES || !count || count > 4 || get16(p + 28) > 1024 ||
		    get16(p + 30)) {
			fail(why, n, "cue %u: bad event, flags, move, variants or gain", i);
			goto out;
		}
		for (unsigned k = 0; k < 4; k++) {
			unsigned clip = get16(p + 20 + 2 * k);
			if (k < count ? clip >= clips && clip != 0xffff : clip != 0xffff) {
				fail(why, n, "cue %u: bad clip", i);
				goto out;
			}
			c->clip[k] = (uint16_t)clip;
		}
		c->event = p[16];
		c->flags = p[17];
		c->move = p[18];
		c->count = (uint8_t)count;
		c->gain = (uint16_t)get16(p + 28);
		scope(p, a, &c->kind, &c->scope, &au->unmatched);
		if (c->kind == 4) {
			/* Effects follow characters and systems only. */
			c->kind = 3;
			au->unmatched++;
		}
	}
	for (unsigned i = 0; i < music; i++, p += 32) {
		struct music_row *r = &au->row[i];
		unsigned count = p[17];
		if (!name_ok(p, 1) || p[16] >= MUSIC_CUES || !count || count > 6 ||
		    get16(p + 30)) {
			fail(why, n, "music cue %u: bad cue or tracks", i);
			goto out;
		}
		for (unsigned k = 0; k < 6; k++) {
			unsigned t = get16(p + 18 + 2 * k);
			if (k < count ? t >= tracks : t != 0) {
				fail(why, n, "music cue %u: bad track", i);
				goto out;
			}
			r->track[k] = (uint16_t)t;
		}
		r->cue = p[16];
		r->count = (uint8_t)count;
		scope(p, a, &r->kind, &r->scope, &au->unmatched);
		if (r->kind == 2) {
			/* Music follows systems only. */
			r->kind = 3;
			au->unmatched++;
		}
	}
	if (tracks && !(au->mp2 = mp2_create())) {
		fail(why, n, "no memory for the MP2 decoder%.0u", 0);
		goto out;
	}
	ok = 1;
out:
	if (!ok) {
		free(d);
		mp2_destroy(au->mp2);
		au->mp2 = NULL;
		return 0;
	}
	au->bank = d;
	au->size = size;
	au->crc = crc;
	au->clips = clips;
	au->tracks = tracks;
	au->cues = cues;
	au->music = music;
	audio_reset(au);
	return 1;
}

int audio_open(struct audio *au, const char *content, const struct assets *a, char *why,
	       size_t n, char *path, size_t path_size)
{
	audio_close(au);
	if (path_size)
		*path = 0;
	if (!content || !*content)
		return fail(why, n, "the frontend gave no content path%.0u", 0);
	const char *slash = strrchr(content, '/'), *dot = strrchr(content, '.');
	size_t stem = dot && (!slash || dot > slash) ? (size_t)(dot - content) : strlen(content);
	if (snprintf(path, path_size, "%.*s.fightsnd", (int)stem, content) >= (int)path_size)
		return fail(why, n, "path too long%.0u", 0);
	FILE *f = fopen(path, "rb");
	if (!f)
		return fail(why, n, "cannot open it%.0u", 0);
	long len = -1;
	if (!fseek(f, 0, SEEK_END))
		len = ftell(f);
	if (len < 0 || (unsigned long)len > AUDIO_MAX_BANK || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return fail(why, n, "unreadable or larger than 64 MiB%.0u", 0);
	}
	uint8_t *d = malloc(len ? (size_t)len : 1);
	if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) {
		free(d);
		fclose(f);
		return fail(why, n, "read error%.0u", 0);
	}
	fclose(f);
	return audio_bank(au, d, (size_t)len, a, why, n);
}

/* ---- events ---- */

static int contact(unsigned id)
{
	return id == SOUND_HIT_LIGHT || id == SOUND_HIT_HEAVY || id == SOUND_HIT_SPECIAL ||
	       id == SOUND_GUARD || id == SOUND_SLASH_LIGHT || id == SOUND_SLASH_HEAVY ||
	       id == SOUND_SLASH_SPECIAL || id == SOUND_SLASH_GUARD || id == SOUND_CLASH ||
	       id == SOUND_KO;
}

static int voice_slot(struct audio *au, unsigned kind, unsigned id, unsigned side)
{
	if (kind == KIND_VOICE)
		return SLOT_VOICE + (int)side;
	if (kind == KIND_ANNOUNCER)
		return SLOT_ANNOUNCER;
	if (!contact(id))
		return SLOT_SFX + (int)side;
	static const int pool[3] = {SLOT_CONTACT, SLOT_CONTACT + 1, SLOT_SPARE};
	int best = pool[0];
	for (int i = 0; i < 3; i++) {
		if (!au->s.voice[pool[i]].clip)
			return pool[i];
		if (au->s.voice[pool[i]].age > au->s.voice[best].age)
			best = pool[i];
	}
	return best;
}

static void event(struct audio *au, const struct fight_state *s, const struct assets *a,
		  const struct sound_event *ev)
{
	for (unsigned kind = 0; kind < 3; kind++) {
		int best = -1, score = -1;
		unsigned side = 0;
		for (unsigned i = 0; i < au->cues; i++) {
			const struct cue *c = &au->cue[i];
			if (c->event != ev->id || (c->flags & 3) != kind || c->kind == 3 ||
			    (c->move && c->move != ev->move))
				continue;
			unsigned t = (c->flags & FLAG_OTHER) ? !ev->side : ev->side & 1;
			unsigned chr = s->chr[t];
			if (chr >= a->characters)
				continue;
			int sc = c->move ? 1 : 0;
			if (c->kind == 1) {
				if (c->scope != a->chr[chr].system)
					continue;
				sc += 2;
			} else if (c->kind == 2) {
				if (c->scope != chr)
					continue;
				sc += 4;
			}
			if (sc > score) {
				score = sc;
				best = (int)i;
				side = t;
			}
		}
		if (best < 0)
			continue;
		const struct cue *c = &au->cue[best];
		uint32_t h = (s->tick * 2654435761u) ^ (ev->id * 40503u) ^ (side * 977u);
		unsigned clip = c->clip[(h >> 16) % c->count];
		if (clip == 0xffff) {
			au->stats.silent++;
			continue;
		}
		int slot = voice_slot(au, kind, ev->id, side);
		struct voice *v = &au->s.voice[slot];
		if (v->clip)
			au->stats.steals++;
		au->stats.starts++;
		v->clip = clip + 1;
		v->pos = v->frac = v->age = 0;
		v->gain = c->gain / 2 > 255 ? 255 : c->gain / 2;
		v->pan = slot == SLOT_CONTACT || slot == SLOT_CONTACT + 1 || slot >= SLOT_ANNOUNCER
				 ? 128
				 : side ? 160 : 96;
		au->block[slot] = -1;
	}
}

/* The row of a cue: the arena's stage, else the system, else any. */
static int music_row(const struct audio *au, unsigned cue, uint32_t system, int arena)
{
	int sys = -1, any = -1;
	for (unsigned i = 0; i < au->music; i++) {
		const struct music_row *r = &au->row[i];
		if (r->cue != cue)
			continue;
		if (r->kind == 4 && (int)r->scope == arena)
			return (int)i;
		if (r->kind == 1 && r->scope == system && sys < 0)
			sys = (int)i;
		if (r->kind == 0 && any < 0)
			any = (int)i;
	}
	return sys >= 0 ? sys : any;
}

/* Start a cue's track (pick-th of its list); a track already playing
 * unfaded keeps going. No row: silence. */
static void music_start(struct audio *au, unsigned cue, uint32_t system, int arena,
			uint32_t pick)
{
	struct audio_state *st = &au->s;
	int r = music_row(au, cue, system, arena);
	st->pending = st->pending_system = 0;
	if (r < 0) {
		st->track = 0;
		st->fade = st->fade_len = 0;
		return;
	}
	uint32_t t = au->row[r].track[pick % au->row[r].count] + 1u;
	if (st->track == t && !st->fade)
		return;
	st->track = t;
	st->pos = st->fade = st->fade_len = st->loops = 0;
}

static void music_fade(struct audio *au, uint32_t frames, unsigned cue, uint32_t system)
{
	struct audio_state *st = &au->s;
	if (!st->track) {
		music_start(au, cue, system, NO_ARENA, 0);
		return;
	}
	if (!st->fade)
		st->fade = st->fade_len = frames;
	st->pending = cue + 1;
	st->pending_system = system;
}

void audio_tick(struct audio *au, const struct fight_state *s, const struct assets *a)
{
	if (!au->bank)
		return;
	uint32_t p1 = s->chr[0] < a->characters ? a->chr[s->chr[0]].system : 0,
		 p2 = s->chr[1] < a->characters ? a->chr[s->chr[1]].system : 0,
		 stage = s->stage ? s->stage - 1 : 0;
	int arena = s->arena < a->stages ? (int)s->arena : NO_ARENA;
	if (s->mode + 1 != au->s.mode) {
		au->s.mode = s->mode + 1;
		if (s->mode == MODE_SELECT)
			music_start(au, MUSIC_SELECT, ANY_SYSTEM, NO_ARENA, 0);
		else if (s->mode == MODE_FIGHT)
			music_start(au, MUSIC_FIGHT, p2, arena, stage);
		else if (s->mode == MODE_RESULT)
			music_start(au, MUSIC_CONTINUE, p1, NO_ARENA, 0);
		else if (s->mode == MODE_CLEAR)
			music_start(au, MUSIC_CLEAR, p1, NO_ARENA, 0);
	}
	for (unsigned i = 0; i < s->sounds && i < MAX_SOUND_EVENTS; i++) {
		const struct sound_event *ev = &s->sound[i];
		if (ev->id >= SOUNDS)
			continue;
		event(au, s, a, ev);
		if (ev->id == SOUND_ROUND_START)
			music_start(au, MUSIC_FIGHT, p2, arena, stage);
		else if (ev->id == SOUND_KO) {
			unsigned winner = s->chr[!ev->side];
			music_fade(au, KO_FADE, MUSIC_WIN,
				   winner < a->characters ? a->chr[winner].system : 0);
		}
	}
}

/* ---- mixing ---- */

static void decode_block(struct audio *au, int slot, const struct clip *c, uint32_t b)
{
	int16_t *out = au->decoded[slot];
	uint32_t blocks = (c->samples + BLOCK - 1) / BLOCK;
	audio_adpcm_block(c->data + (size_t)b * BLOCK_BYTES, out, BLOCK);
	out[BLOCK] = 0;
	if (b + 1 < blocks)
		audio_adpcm_block(c->data + (size_t)(b + 1) * BLOCK_BYTES, out + BLOCK, 1);
	au->block[slot] = (int32_t)b;
}

static void mix_voice(struct audio *au, int slot, int32_t *acc, unsigned frames)
{
	struct voice *v = &au->s.voice[slot];
	const struct clip *c = &au->clip[v->clip - 1];
	/* Center plays at full level on both sides; off center the far side
	 * drops (pan 96: left 256, right 192). */
	int32_t pan = (int32_t)v->pan, gain = (int32_t)v->gain,
		gl = (pan <= 128 ? 256 : 256 - (pan - 128) * 2) * gain >> 7,
		gr = (pan >= 128 ? 256 : 256 - (128 - pan) * 2) * gain >> 7;
	/* Locals: the stores to acc would otherwise reload the voice. */
	uint32_t pos = v->pos, frac = v->frac, step = c->step, i = 0;
	while (i < frames && pos < c->samples) {
		uint32_t b = pos / BLOCK;
		if (au->block[slot] != (int32_t)b)
			decode_block(au, slot, c, b);
		const int16_t *d = au->decoded[slot];
		/* Stay inside this block (the cache holds one sample past it). */
		while (i < frames && pos / BLOCK == b && pos < c->samples) {
			uint32_t k = pos % BLOCK;
			int32_t x = d[k] + (int32_t)(((int64_t)(d[k + 1] - d[k]) * frac) >> 16);
			acc[2 * i] += x * gl >> 8;
			acc[2 * i + 1] += x * gr >> 8;
			frac += step;
			pos += frac >> 16;
			frac &= 0xffff;
			i++;
		}
	}
	if (pos >= c->samples) {
		memset(v, 0, sizeof(*v));
		return;
	}
	v->pos = pos;
	v->frac = frac;
	v->age += i;
}

/* The decoded frame f of the current track in au->pcm; 0 if it fails. */
static int music_frame(struct audio *au, const struct track *t, uint32_t f)
{
	if (au->mp2_track != au->s.track) {
		mp2_open(au->mp2, t->data, t->frames, t->frame_bytes,
			 t->loops ? t->loop_start / MP2_SAMPLES : t->frames);
		au->mp2_track = au->s.track;
		au->frame = UINT32_MAX;
	}
	if (au->frame == f)
		return 1;
	uint64_t t0 = now_us();
	int n = mp2_frame(au->mp2, f, au->pcm);
	uint64_t us = now_us() - t0;
	if (n < 0)
		return 0;
	au->frame = f;
	au->stats.frames += (unsigned)n;
	au->stats.seeks += n == 2;
	au->stats.restores += n == 0;
	au->stats.decode_us += us;
	if (us > au->stats.decode_max_us)
		au->stats.decode_max_us = us;
	return 1;
}

static void mix_music(struct audio *au, int32_t *acc, unsigned frames, int32_t gain)
{
	struct audio_state *st = &au->s;
	for (unsigned i = 0; i < frames && st->track; i++) {
		const struct track *t = &au->track[st->track - 1];
		if (t->loops && st->pos >= t->loop_end) {
			st->pos = t->loop_start + (st->pos - t->loop_end);
			st->loops++;
			au->stats.loops++;
		} else if (!t->loops && st->pos >= t->samples) {
			st->track = 0;
			break;
		}
		if (!music_frame(au, t, st->pos / MP2_SAMPLES)) {
			st->track = 0;
			break;
		}
		const int16_t *x = au->pcm + 2 * (st->pos % MP2_SAMPLES);
		int32_t g = gain * (int32_t)t->level / 1000;
		if (st->fade)
			g = (int32_t)((int64_t)g * st->fade / st->fade_len);
		acc[2 * i] += x[0] * g >> 8;
		acc[2 * i + 1] += x[1] * g >> 8;
		st->pos++;
		if (st->fade && !--st->fade) {
			uint32_t cue = st->pending, system = st->pending_system;
			st->track = 0;
			st->fade_len = 0;
			if (cue)
				music_start(au, cue - 1, system, NO_ARENA, 0);
		}
	}
}

unsigned audio_busy(const struct audio *au)
{
	unsigned n = 0;
	for (int i = 0; i < AUDIO_VOICES; i++)
		n += au->s.voice[i].clip != 0;
	return n;
}

size_t audio_resident(const struct audio *au)
{
	return au->size + (au->mp2 ? 40u << 10 : 0) + sizeof(*au);
}

void audio_mix(struct audio *au, int16_t *out, unsigned frames, int advance)
{
	static int32_t sfx[CHUNK * 2], mus[CHUNK * 2];
	if (!advance || !au->bank) {
		memset(out, 0, (size_t)frames * 4);
		return;
	}
	unsigned busy = audio_busy(au);
	if (busy > au->stats.peak)
		au->stats.peak = busy;
	int32_t master = (int32_t)(au->master > 100 ? 100 : au->master),
		sfx_gain = (int32_t)(au->sfx_level > 100 ? 100 : au->sfx_level) * master * 256 / 10000,
		music_gain = (int32_t)(au->music_level > 100 ? 100 : au->music_level) * master *
			     256 / 10000;
	while (frames) {
		unsigned n = frames < CHUNK ? frames : CHUNK;
		memset(sfx, 0, n * 8);
		memset(mus, 0, n * 8);
		for (int v = 0; v < AUDIO_VOICES; v++)
			if (au->s.voice[v].clip)
				mix_voice(au, v, sfx, n);
		mix_music(au, mus, n, music_gain);
		for (unsigned i = 0; i < 2 * n; i++) {
			int32_t x = (sfx[i] * sfx_gain >> 8) + mus[i];
			out[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
		}
		out += 2 * n;
		frames -= n;
	}
}

/* ---- state block ---- */

void audio_encode(const struct audio *au, uint8_t out[AUDIO_STATE_BYTES])
{
	uint32_t w[AUDIO_STATE_WORDS] = {0};
	const struct audio_state *st = &au->s;
	w[0] = au->bank ? au->crc : 0;
	for (int i = 0; i < AUDIO_VOICES; i++) {
		const struct voice *v = &st->voice[i];
		uint32_t *p = w + 1 + 5 * i;
		p[0] = v->clip | v->gain << 16;
		p[1] = v->pos;
		p[2] = v->frac;
		p[3] = v->pan;
		p[4] = v->age;
	}
	uint32_t *m = w + 1 + 5 * AUDIO_VOICES;
	m[0] = st->track;
	m[1] = st->pos;
	m[2] = st->fade;
	m[3] = st->fade_len;
	m[4] = st->pending;
	m[5] = st->pending_system;
	m[6] = st->mode;
	m[7] = st->loops;
	memcpy(out, "FGAUDIO1", 8);
	for (int i = 0; i < AUDIO_STATE_WORDS; i++)
		put32(out + 8 + 4 * i, w[i]);
}

int audio_decode(struct audio *au, const uint8_t in[AUDIO_STATE_BYTES])
{
	uint32_t w[AUDIO_STATE_WORDS];
	struct audio_state st;
	if (memcmp(in, "FGAUDIO1", 8))
		return 0;
	for (int i = 0; i < AUDIO_STATE_WORDS; i++)
		w[i] = get32(in + 8 + 4 * i);
	const int used = 1 + 5 * AUDIO_VOICES + 8;
	for (int i = used; i < AUDIO_STATE_WORDS; i++)
		if (w[i])
			return 0;
	if (!au->bank || w[0] != au->crc) {
		/* Saved with another bank or none: the game state still loads. */
		audio_reset(au);
		return 1;
	}
	memset(&st, 0, sizeof(st));
	for (int i = 0; i < AUDIO_VOICES; i++) {
		struct voice *v = &st.voice[i];
		const uint32_t *p = w + 1 + 5 * i;
		v->clip = p[0] & 0xffff;
		v->gain = p[0] >> 16;
		v->pos = p[1];
		v->frac = p[2];
		v->pan = p[3];
		v->age = p[4];
		if (v->clip > au->clips || v->gain > 255 || v->frac > 0xffff || v->pan > 256 ||
		    (v->clip ? v->pos >= au->clip[v->clip - 1].samples
			     : v->gain || v->pos || v->frac || v->pan || v->age))
			return 0;
	}
	const uint32_t *m = w + 1 + 5 * AUDIO_VOICES;
	st.track = m[0];
	st.pos = m[1];
	st.fade = m[2];
	st.fade_len = m[3];
	st.pending = m[4];
	st.pending_system = m[5];
	st.mode = m[6];
	st.loops = m[7];
	if (st.track > au->tracks || st.fade > st.fade_len || st.fade_len > AUDIO_RATE * 10 ||
	    st.pending > MUSIC_CUES || (st.pending_system >= MAX_SYSTEMS &&
					st.pending_system != ANY_SYSTEM) ||
	    st.mode > MODES)
		return 0;
	if (st.track) {
		const struct track *t = &au->track[st.track - 1];
		if (st.pos > (t->loops ? t->loop_end : t->samples))
			return 0;
	}
	au->s = st;
	for (int i = 0; i < AUDIO_VOICES; i++)
		au->block[i] = -1;
	au->frame = UINT32_MAX;
	return 1;
}
