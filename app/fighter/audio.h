/* The fighter's sound: the FIGHTSND bank that sits next to a content pack
 * (built by tools/fighter/sound.py, which documents the format), an
 * 8-voice mixer of IMA ADPCM effects resampled to 48 kHz, MP2 music
 * (mp2.h) with sample-exact loops and fades, and the mapping from the
 * rules' sound events (enum sound_id) and game modes to clips and tracks.
 * Everything that decides what is heard is in struct audio_state, which
 * the core saves as its own block, so a loaded state sounds exactly like
 * the run it was saved from. A missing or bad bank only means silence. */
#ifndef FIGHTER_AUDIO_H
#define FIGHTER_AUDIO_H
#include "fighter.h"
#include <stddef.h>
#include <stdint.h>
#define AUDIO_RATE 48000
#define AUDIO_VOICES 8
/* Bank limits, mirrored in sound.py. */
#define AUDIO_MAX_CLIPS 1024
#define AUDIO_MAX_TRACKS 64
#define AUDIO_MAX_CUES 2048
#define AUDIO_MAX_MUSIC 64
#define AUDIO_MAX_CLIP_SAMPLES (48000 * 10)
#define AUDIO_MAX_BANK (64u << 20)
/* Saved block: "FGAUDIO1" and AUDIO_STATE_WORDS little-endian words: the
 * bank CRC, 5 per voice, 8 for the music, the rest zero. */
#define AUDIO_STATE_WORDS 64
#define AUDIO_STATE_BYTES (8 + AUDIO_STATE_WORDS * 4)
enum music_cue { MUSIC_SELECT, MUSIC_FIGHT, MUSIC_WIN, MUSIC_CONTINUE, MUSIC_CLEAR, MUSIC_CUES };
/* Voice slots: each side's voice and own effects, two contact effects
 * (hits, guards, clashes, KO), the announcer and one spare for contact
 * effects. */
enum voice_slot { SLOT_VOICE, SLOT_SFX = 2, SLOT_CONTACT = 4, SLOT_ANNOUNCER = 6, SLOT_SPARE };
/* clip is the clip index + 1 (0 = idle); pos/frac the source position in
 * 16.16; gain /128; pan 0 (left) .. 256 (right); age counts output frames
 * since the start, for stealing the oldest. */
struct voice {
	uint32_t clip, pos, frac, gain, pan, age;
};
/* track is the track index + 1 (0 = none); pos the next decoded sample;
 * fade counts down output frames of a fade-out of fade_len frames (0 = no
 * fade); pending is a music cue + 1 to start when the fade ends, with
 * pending_system its system; mode is the last game mode seen + 1. */
struct audio_state {
	struct voice voice[AUDIO_VOICES];
	uint32_t track, pos, fade, fade_len, pending, pending_system, mode, loops;
};
struct clip {
	char name[16];
	const uint8_t *data;
	uint32_t samples, rate, step;
};
struct track {
	char name[16];
	const uint8_t *data;
	uint32_t frames, frame_bytes, samples, loop_start, loop_end, loops, level;
};
/* A cue with its scope resolved against the pack: kind 0 any, 1 system,
 * 2 character, 3 matches nothing, 4 stage (music rows only). */
struct cue {
	uint8_t event, flags, move, count, kind, scope;
	uint16_t clip[4], gain;
};
struct music_row {
	uint8_t cue, count, kind, scope;
	uint16_t track[6];
};
/* Counters for the periodic log, cleared by audio_stats_reset. */
struct audio_stats {
	unsigned starts, steals, silent, peak, frames, seeks, restores, loops;
	uint64_t decode_us, decode_max_us;
};
struct audio {
	uint8_t *bank;
	size_t size;
	uint32_t crc;
	unsigned clips, tracks, cues, music, unmatched;
	struct clip clip[AUDIO_MAX_CLIPS];
	struct track track[AUDIO_MAX_TRACKS];
	struct cue cue[AUDIO_MAX_CUES];
	struct music_row row[AUDIO_MAX_MUSIC];
	/* Levels in percent, from the core options. */
	unsigned master, music_level, sfx_level;
	struct audio_state s;
	struct audio_stats stats;
	/* Caches, rebuilt from the state on demand. */
	int32_t block[AUDIO_VOICES];
	int16_t decoded[AUDIO_VOICES][257];
	struct mp2 *mp2;
	uint32_t mp2_track, frame;
	int16_t pcm[1152 * 2];
};
/* Parse a bank held in memory (taking ownership of the malloc'd data) and
 * resolve its scopes against the pack. Returns 0 and a reason on error,
 * leaving the audio without a bank. */
int audio_bank(struct audio *au, uint8_t *data, size_t size, const struct assets *a,
	       char *why, size_t why_size);
/* The bank next to content_path (its extension replaced by .fightsnd). */
int audio_open(struct audio *au, const char *content_path, const struct assets *a,
	       char *why, size_t why_size, char *path, size_t path_size);
void audio_close(struct audio *au);
/* Back to silence: no voices, no music, nothing pending. */
void audio_reset(struct audio *au);
/* React to one unpaused tick: start the effects of s->sound[] and follow
 * the game mode with the music. */
void audio_tick(struct audio *au, const struct fight_state *s, const struct assets *a);
/* Mix frames stereo frames; advance 0 (paused) gives silence and keeps
 * the state. */
void audio_mix(struct audio *au, int16_t *out, unsigned frames, int advance);
void audio_encode(const struct audio *au, uint8_t out[AUDIO_STATE_BYTES]);
/* 0 if the block is malformed; a block for another bank (or none) is
 * accepted as silence. */
int audio_decode(struct audio *au, const uint8_t in[AUDIO_STATE_BYTES]);
/* Voices busy now, and the bytes of decoded data kept besides the bank. */
unsigned audio_busy(const struct audio *au);
size_t audio_resident(const struct audio *au);
void audio_stats_reset(struct audio *au);
/* IMA ADPCM: decode count samples of one block of the bank format. */
void audio_adpcm_block(const uint8_t *block, int16_t *out, unsigned count);
#endif
