/* Host checks of the fighter's sound (audio.c, mp2.c) against known values
 * and the shipped bank: IMA ADPCM decode vectors and clamping, the mixer's
 * resampling, interpolation, gains and pan on a bank built here, voice
 * slots and stealing, the cue precedence (character + move over system
 * over any, the opponent's voice on hits, silent variants), the music cues
 * (select, fight by system and stage, KO fade into the winner's jingle),
 * MP2 seeking (every sampled frame decoded after a seek equals the
 * continuous decode), loops (the run across a loop end equals the same run
 * restored from a state saved before it), the audio state block (round
 * trip, rejection, another bank = silence), rejection of malformed banks
 * and the host cost of a busy run. Usage: test-audio BANK.fightsnd */
#include "audio.h"
#include "mp2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct assets pack;
static struct audio au, other;

static uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
	}
	return c ^ 0xffffffffu;
}
static void put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v & 0xffff);
	put16(p + 2, v >> 16);
}
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
static void seal(uint8_t *d, size_t n) { put32(d + 16, crc32(d + 20, n - 20)); }
static uint8_t *copy(const uint8_t *d, size_t n)
{
	uint8_t *p = malloc(n);
	assert(p);
	memcpy(p, d, n);
	return p;
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
	assert(p && fread(p, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	*n = (size_t)len;
	return p;
}

static void adpcm_vectors(void)
{
	uint8_t b[132] = {0};
	int16_t out[256];
	/* Codes 7, 7, 15, 0 from silence: by the IMA tables by hand. */
	b[4] = 0x77;
	b[5] = 0x0f;
	audio_adpcm_block(b, out, 4);
	assert(out[0] == 11 && out[1] == 41 && out[2] == -22 && out[3] == -13);
	/* Clamping at both ends and at step index 88. */
	put16(b, 32760);
	b[2] = 88;
	b[4] = 0xf7;
	audio_adpcm_block(b, out, 2);
	assert(out[0] == 32767 && out[1] == 32767 - 61436);
	put16(b, (uint16_t)-32760);
	b[4] = 0x0f;
	audio_adpcm_block(b, out, 1);
	assert(out[0] == -32768);
}

/* A bank of one 24 kHz clip whose 300 samples are code 0 from predictor v
 * (so 0x7fff: 32767 everywhere at step 7 / 8 = 0), one cue per event kind
 * and no music; the clip is 2 blocks. */
static uint8_t *tiny_bank(size_t *n, int16_t level)
{
	const unsigned clips = 1, cues = 4, data = 32 + 32 * clips + 32 * cues;
	size_t size = data + 2 * 132;
	uint8_t *d = calloc(1, size);
	assert(d);
	memcpy(d, "FIGHTSND", 8);
	put32(d + 8, 1);
	put32(d + 12, (uint32_t)size);
	put16(d + 20, clips);
	put16(d + 22, 0);
	put16(d + 24, cues);
	put16(d + 26, 0);
	put32(d + 28, data);
	uint8_t *p = d + 32;
	memcpy(p, "flat", 4);
	put32(p + 16, data);
	put32(p + 20, 300);
	put32(p + 24, 24000);
	p += 32;
	/* any WHOOSH_LIGHT sfx; TERRY WHOOSH_LIGHT sfx (more specific);
	 * HIT_LIGHT voice for the opponent with a silent second variant;
	 * KO announcer at half gain. */
	static const uint8_t ev[4] = {SOUND_WHOOSH_LIGHT, SOUND_WHOOSH_LIGHT, SOUND_HIT_LIGHT,
				      SOUND_KO};
	static const uint8_t fl[4] = {0, 0, 1 | 4, 2};
	for (unsigned i = 0; i < cues; i++, p += 32) {
		if (i == 1)
			memcpy(p, "TERRY", 5);
		p[16] = ev[i];
		p[17] = fl[i];
		p[19] = i == 2 ? 2 : 1;
		for (int k = 0; k < 4; k++)
			put16(p + 20 + 2 * k, 0xffff);
		put16(p + 20, 0);
		put16(p + 28, i == 3 ? 128 : 256);
	}
	for (int b = 0; b < 2; b++)
		put16(d + data + 132 * b, (uint16_t)level);
	seal(d, size);
	*n = size;
	return d;
}

static void setup_pack(void)
{
	pack.systems = 2;
	strcpy(pack.sys[0].name, "kof2002");
	strcpy(pack.sys[1].name, "samsho5sp");
	pack.characters = 2;
	strcpy(pack.chr[0].name, "TERRY");
	pack.chr[0].system = 0;
	strcpy(pack.chr[1].name, "HAOHMARU");
	pack.chr[1].system = 1;
}

static void tick_event(struct fight_state *s, unsigned id, unsigned side, unsigned move)
{
	s->sounds = 1;
	s->sound[0] = (struct sound_event){(uint8_t)id, (uint8_t)side,
					   (uint8_t)s->chr[side], 0, (uint8_t)move};
	audio_tick(&au, s, &pack);
	s->sounds = 0;
}

static void mixer(void)
{
	size_t n;
	int16_t out[1600];
	char why[128];
	uint8_t *d = tiny_bank(&n, 8000);
	assert(audio_bank(&au, d, n, &pack, why, sizeof(why)));
	assert(au.clips == 1 && au.cues == 4 && !au.unmatched);
	au.master = au.sfx_level = au.music_level = 100;
	struct fight_state s;
	memset(&s, 0, sizeof(s));
	s.mode = MODE_FIGHT;
	s.chr[0] = 1; /* HAOHMARU */
	s.chr[1] = 0; /* TERRY */
	/* Haohmaru matches only the any-scope cue; on the effects slot of P1,
	 * panned left (left 256, right 192). 300 samples at 24 kHz = 600
	 * output frames of the flat 8000. */
	tick_event(&s, SOUND_WHOOSH_LIGHT, 0, 1);
	assert(au.s.voice[SLOT_SFX].clip == 1 && au.s.voice[SLOT_SFX].pan == 96);
	audio_mix(&au, out, 800, 1);
	assert(out[0] == 8000 && out[1] == 6000 && out[2 * 598] == 8000 &&
	       out[2 * 600] == 0 && out[2 * 799 + 1] == 0);
	assert(!au.s.voice[SLOT_SFX].clip);
	/* Paused: silence and nothing moves. */
	tick_event(&s, SOUND_WHOOSH_LIGHT, 1, 1);
	struct audio_state before = au.s;
	audio_mix(&au, out, 800, 0);
	assert(!out[0] && !memcmp(&before, &au.s, sizeof(before)));
	/* P2 is Terry: the character cue wins, panned right. */
	assert(au.s.voice[SLOT_SFX + 1].clip == 1 && au.s.voice[SLOT_SFX + 1].pan == 160);
	audio_mix(&au, out, 1, 1);
	assert(out[0] == 6000 && out[1] == 8000);
	/* Levels: master 50 and effects 50 give a quarter. */
	au.master = au.sfx_level = 50;
	audio_mix(&au, out, 1, 1);
	assert(out[1] == 2000);
	au.master = au.sfx_level = 100;
	/* A hit by Terry (side 1) voices Haohmaru (side 0) or stays silent by
	 * the tick's variant; both variants happen. */
	unsigned voiced = 0, silent = 0;
	audio_stats_reset(&au);
	for (s.tick = 0; s.tick < 16; s.tick++) {
		au.s.voice[SLOT_VOICE].clip = 0;
		tick_event(&s, SOUND_HIT_LIGHT, 1, 1);
		voiced += au.s.voice[SLOT_VOICE].clip == 1;
		silent += !au.s.voice[SLOT_VOICE].clip;
		assert(!au.s.voice[SLOT_VOICE + 1].clip);
	}
	assert(voiced && silent && au.stats.silent == silent);
	/* The announcer at half gain; no other kind has a KO cue. */
	tick_event(&s, SOUND_KO, 0, 0);
	assert(au.s.voice[SLOT_ANNOUNCER].clip == 1 && au.s.voice[SLOT_ANNOUNCER].gain == 64);
	audio_close(&au);
	/* Interpolation and resampling: a ramp clip at 12 kHz plays each
	 * source step over 4 output frames, interpolated. */
	d = tiny_bank(&n, 0);
	put32(d + 32 + 24, 12000);
	/* Code 7 throughout from 0: 11, 41, 104, ... */
	uint8_t *blk = d + 32 + 32 + 32 * 4;
	memset(blk + 4, 0x77, 128);
	seal(d, n);
	assert(audio_bank(&au, d, n, &pack, why, sizeof(why)));
	au.master = au.sfx_level = au.music_level = 100;
	s.chr[0] = 1;
	tick_event(&s, SOUND_WHOOSH_LIGHT, 0, 1);
	audio_mix(&au, out, 8, 1);
	/* Quarter steps between 11, 41 and 104, rounded down. */
	static const int16_t want[8] = {11, 18, 26, 33, 41, 56, 72, 88};
	for (int i = 0; i < 8; i++)
		assert(out[2 * i] == want[i]);
	audio_close(&au);
}

static void malformed(const uint8_t *bank, size_t n)
{
	char why[128];
	/* Each mutation, CRC resealed unless the CRC is the point. */
	struct {
		size_t at;
		uint8_t xor;
		int reseal;
	} cases[] = {
	    {0, 1, 1},  /* magic */
	    {8, 2, 1},  /* version */
	    {12, 1, 1}, /* size */
	    {100, 1, 0},
	    {n - 1, 1, 0},
	    {20, 0xff, 1},          /* clip count */
	    {32 + 26, 0xff, 1},     /* clip 0 rate */
	    {32 + 28, 1, 1},        /* clip 0 reserved */
	    {32 + 16 + 3, 0x40, 1}, /* clip 0 offset */
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint8_t *d = copy(bank, n);
		d[cases[i].at] ^= cases[i].xor;
		*why = 0;
		if (cases[i].reseal)
			seal(d, n);
		int r = audio_bank(&au, d, n, &pack, why, sizeof(why));
		if (r || !*why || au.bank)
			fprintf(stderr, "case %zu: %d %s\n", i, r, why);
		assert(!r && *why && !au.bank);
	}
	unsigned clips = bank[20] | bank[21] << 8, tracks = bank[22] | bank[23] << 8,
		 cues = bank[24] | bank[25] << 8;
	assert(tracks >= 2 && cues);
	size_t track0 = 32 + 32 * (size_t)clips, cue0 = track0 + 48 * (size_t)tracks,
	       music0 = cue0 + 32 * (size_t)cues;
	/* A frame header deep in track 1, a loop end past the track, the ADPCM
	 * step index of clip 0, a cue event, a music track index. */
	uint8_t *d = copy(bank, n);
	uint32_t off = get32(d + track0 + 48 + 16), fb = get32(d + track0 + 48 + 24);
	d[off + 7 * fb + 2] ^= 0x0c;
	seal(d, n);
	assert(!audio_bank(&au, d, n, &pack, why, sizeof(why)));
	d = copy(bank, n);
	put32(d + track0 + 32, get32(d + track0 + 20) * 1152 + 1);
	put32(d + track0 + 36, 1);
	seal(d, n);
	assert(!audio_bank(&au, d, n, &pack, why, sizeof(why)));
	d = copy(bank, n);
	d[get32(d + 32 + 16) + 2] = 89;
	seal(d, n);
	assert(!audio_bank(&au, d, n, &pack, why, sizeof(why)));
	d = copy(bank, n);
	d[cue0 + 16] = SOUNDS;
	seal(d, n);
	assert(!audio_bank(&au, d, n, &pack, why, sizeof(why)));
	d = copy(bank, n);
	put16(d + music0 + 18, (unsigned)tracks);
	seal(d, n);
	assert(!audio_bank(&au, d, n, &pack, why, sizeof(why)));
	for (size_t len = 0; len < 64; len += 7)
		assert(!audio_bank(&au, copy(bank, n), len, &pack, why, sizeof(why)));
	char path[64];
	assert(!audio_open(&au, NULL, &pack, why, sizeof(why), path, sizeof(path)));
	assert(!audio_open(&au, "/nonexistent/x.fight", &pack, why, sizeof(why), path,
			   sizeof(path)) &&
	       !strcmp(path, "/nonexistent/x.fightsnd"));
	/* Silent without a bank, whatever the state asks. */
	int16_t out[4] = {1, 1, 1, 1};
	audio_mix(&au, out, 2, 1);
	assert(!out[0] && !out[3]);
}

static int track_named(const struct audio *a, const char *name)
{
	for (unsigned i = 0; i < a->tracks; i++)
		if (!strcmp(a->track[i].name, name))
			return (int)i;
	return -1;
}

static void seeking(void)
{
	/* Every 37th frame of a looping track, decoded after a seek, equals
	 * the continuous decode; so does the kept loop-start frame. */
	const struct track *t = &au.track[0];
	unsigned keep = t->loops ? t->loop_start / MP2_SAMPLES : t->frames;
	struct mp2 *a = mp2_create(), *b = mp2_create();
	assert(a && b);
	mp2_open(a, t->data, t->frames, t->frame_bytes, t->frames);
	mp2_open(b, t->data, t->frames, t->frame_bytes, keep);
	static int16_t pcm[MP2_SAMPLES * 2], seek[MP2_SAMPLES * 2], kept[MP2_SAMPLES * 2];
	unsigned checked = 0;
	for (unsigned f = 0; f < t->frames; f++) {
		assert(mp2_frame(a, f, pcm) == 1);
		if (f == keep)
			memcpy(kept, pcm, sizeof(kept));
		if (f % 37 == 5 || f == keep) {
			int r = mp2_frame(b, f, seek);
			assert(r == (f ? 2 : 1));
			assert(!memcmp(pcm, seek, sizeof(pcm)));
			checked++;
		}
	}
	assert(mp2_frame(a, t->frames, pcm) == -1);
	if (keep < t->frames) {
		/* Jump back to the loop start: restored, no decoding. */
		assert(mp2_frame(b, keep + 5, seek) == 2);
		assert(mp2_frame(b, keep, seek) == 0 && !memcmp(seek, kept, sizeof(kept)));
		assert(mp2_frame(b, keep + 1, seek) == 1);
	}
	mp2_destroy(a);
	mp2_destroy(b);
	printf("test-audio: %u frames of %s decoded after a seek match\n", checked, t->name);
}

/* Run a and a copy restored from a's state for runs; both outputs agree. */
static void same_after_restore(struct audio *a, struct audio *b, unsigned runs)
{
	uint8_t block[AUDIO_STATE_BYTES];
	static int16_t x[1600], y[1600];
	audio_encode(a, block);
	assert(audio_decode(b, block));
	for (unsigned r = 0; r < runs; r++) {
		audio_mix(a, x, 800, 1);
		audio_mix(b, y, 800, 1);
		assert(!memcmp(x, y, sizeof(x)));
	}
	uint8_t c[AUDIO_STATE_BYTES];
	audio_encode(a, block);
	audio_encode(b, c);
	assert(!memcmp(block, c, sizeof(c)));
}

static void music_and_state(const uint8_t *bank, size_t n)
{
	char why[128];
	assert(audio_bank(&au, copy(bank, n), n, &pack, why, sizeof(why)));
	assert(audio_bank(&other, copy(bank, n), n, &pack, why, sizeof(why)));
	au.master = other.master = 100;
	au.music_level = other.music_level = 60;
	au.sfx_level = other.sfx_level = 100;
	seeking();
	struct fight_state s;
	memset(&s, 0, sizeof(s));
	s.mode = MODE_SELECT;
	s.chr[0] = 0;
	s.chr[1] = 1;
	audio_tick(&au, &s, &pack);
	assert(au.s.track == (uint32_t)track_named(&au, "kof-select") + 1);
	/* Fight music by the opponent's system and the stage. */
	s.mode = MODE_FIGHT;
	s.stage = 2;
	audio_tick(&au, &s, &pack);
	assert(au.s.track == (uint32_t)track_named(&au, "lb-theme2") + 1);
	s.chr[1] = 0;
	s.stage = 1;
	tick_event(&s, SOUND_ROUND_START, 0, 0);
	assert(au.s.track == (uint32_t)track_named(&au, "kof-japan") + 1);
	assert(au.s.voice[SLOT_ANNOUNCER].clip);
	/* A special: Terry's voice for Power Wave (move 9) and the effect. */
	tick_event(&s, SOUND_WHOOSH_SPECIAL, 0, MOVES + 1);
	assert(!strcmp(au.clip[au.s.voice[SLOT_VOICE].clip - 1].name, "terry-sp1"));
	assert(!strcmp(au.clip[au.s.voice[SLOT_SFX].clip - 1].name, "k-whoosh-sp"));
	/* Haohmaru's whoosh comes from the samsho5sp set. */
	s.chr[1] = 1;
	tick_event(&s, SOUND_WHOOSH_LIGHT, 1, 1);
	assert(!strcmp(au.clip[au.s.voice[SLOT_SFX + 1].clip - 1].name, "s-whoosh-l"));
	/* Three contact effects fill the pool; a fourth steals the oldest. */
	unsigned steals = au.stats.steals;
	for (int i = 0; i < 3; i++) {
		tick_event(&s, SOUND_GUARD, 0, 1);
		static int16_t tmp[200];
		audio_mix(&au, tmp, 100, 1);
	}
	assert(au.s.voice[SLOT_CONTACT].clip && au.s.voice[SLOT_CONTACT + 1].clip &&
	       au.s.voice[SLOT_SPARE].clip);
	tick_event(&s, SOUND_GUARD, 0, 1);
	assert(au.stats.steals == steals + 1 && au.s.voice[SLOT_CONTACT].age == 0);
	/* Restore mid-fight: identical output and state for 300 runs. */
	same_after_restore(&au, &other, 300);
	/* Across the loop end of kof-japan, continuous vs restored. */
	const struct track *t = &au.track[au.s.track - 1];
	assert(t->loops);
	au.s.pos = t->loop_end - 3000;
	au.frame = UINT32_MAX;
	unsigned loops = au.s.loops;
	same_after_restore(&au, &other, 10);
	assert(au.s.loops == loops + 1 && au.s.pos < t->loop_start + 10 * 800);
	/* A KO by P1 (P2 down) fades into the winner's (Terry's) jingle. */
	s.chr[1] = 1;
	tick_event(&s, SOUND_KO, 1, 0);
	assert(au.s.fade && au.s.pending == MUSIC_WIN + 1);
	same_after_restore(&au, &other, 50);
	assert(!au.s.fade && au.s.track == (uint32_t)track_named(&au, "kof-winner") + 1);
	/* The jingle plays once, then silence. */
	static int16_t out[1600];
	for (int i = 0; i < 60 * 6; i++)
		audio_mix(&au, out, 800, 1);
	assert(!au.s.track);
	s.mode = MODE_RESULT;
	audio_tick(&au, &s, &pack);
	assert(au.s.track == (uint32_t)track_named(&au, "kof-continue") + 1);
	/* State block rejection; another bank's block restores as silence. */
	uint8_t block[AUDIO_STATE_BYTES], bad[AUDIO_STATE_BYTES];
	audio_encode(&au, block);
	memcpy(bad, block, sizeof(bad));
	bad[0] ^= 1;
	assert(!audio_decode(&other, bad));
	memcpy(bad, block, sizeof(bad));
	bad[AUDIO_STATE_BYTES - 1] = 1;
	assert(!audio_decode(&other, bad));
	memcpy(bad, block, sizeof(bad));
	put32(bad + 8 + 4 * (1 + 5 * AUDIO_VOICES), 999); /* track */
	assert(!audio_decode(&other, bad));
	memcpy(bad, block, sizeof(bad));
	put32(bad + 8 + 4 * (1 + 5 * SLOT_CONTACT), 0xffff); /* clip */
	assert(!audio_decode(&other, bad));
	memcpy(bad, block, sizeof(bad));
	put32(bad + 8, au.crc ^ 1);
	assert(audio_decode(&other, bad) && !other.s.track && !audio_busy(&other));
	assert(audio_decode(&other, block) && other.s.track == au.s.track);
	/* Host cost of a busy run: all 8 voices and music. */
	for (int v = 0; v < AUDIO_VOICES; v++)
		au.s.voice[v] = (struct voice){1 + (unsigned)v % au.clips, 0, 0, 128, 128, 0};
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	audio_stats_reset(&au);
	for (int i = 0; i < 600; i++) {
		for (int v = 0; v < AUDIO_VOICES; v++)
			if (!au.s.voice[v].clip)
				au.s.voice[v] = (struct voice){1, 0, 0, 128, 128, 0};
		audio_mix(&au, out, 800, 1);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double us = ((double)(t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / 600;
	printf("test-audio: host %.1f us per run with 8 voices and music (%u MP2 frames, "
	       "%llu us avg per frame; this build)\n",
	       us, au.stats.frames,
	       (unsigned long long)(au.stats.frames ? au.stats.decode_us / au.stats.frames : 0));
	audio_close(&au);
	audio_close(&other);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	setup_pack();
	adpcm_vectors();
	mixer();
	size_t n;
	uint8_t *bank = read_all(argv[1], &n);
	char why[128];
	assert(audio_bank(&au, copy(bank, n), n, &pack, why, sizeof(why)));
	printf("test-audio: bank %zu KiB, %u clips, %u tracks, %u cues, %u music cues, %u "
	       "scopes not in the test pack\n",
	       n / 1024, au.clips, au.tracks, au.cues, au.music, au.unmatched);
	malformed(bank, n);
	music_and_state(bank, n);
	free(bank);
	puts("test-audio: ADPCM vectors, mixer resampling/gains/pan/pause, cue precedence, "
	     "slots and stealing, music cues and KO fade, MP2 seek and loop exactness, "
	     "state block round trip and rejection, malformed banks passed");
	return 0;
}
