/* MP2 (MPEG-1 Layer II) frame decoder for the fighter's music: the core's
 * own copy of pl_mpeg's audio decoder (mp2.c), over one track in memory
 * whose frames all have the same size. Frames decode to interleaved stereo
 * int16. Seeking decodes the frame before the target first: Layer II has no
 * bit reservoir and the synthesis filter's history is fully rewritten by
 * one frame, so with the filter's ring position restored as well the
 * target then decodes bit for bit as in a continuous decode. One frame
 * (the loop start) can be kept: the decoder state after it and its
 * samples are saved the first time it is decoded and restored when
 * playback jumps back to it, so a loop costs no extra decoding. */
#ifndef FIGHTER_MP2_H
#define FIGHTER_MP2_H
#include <stdint.h>
#define MP2_SAMPLES 1152
struct mp2;
struct mp2 *mp2_create(void);
void mp2_destroy(struct mp2 *m);
/* Use frames x frame_bytes bytes at data (kept by the caller); keep is the
 * frame to snapshot (or frames for none). */
void mp2_open(struct mp2 *m, const uint8_t *data, unsigned frames,
	      unsigned frame_bytes, unsigned keep);
/* Frame f into pcm. Returns the frames decoded for it (0 when restored or
 * cached, 1 continuing or from the start, 2 after a seek), -1 if f is out
 * of range or does not decode. */
int mp2_frame(struct mp2 *m, unsigned f, int16_t pcm[MP2_SAMPLES * 2]);
#endif
