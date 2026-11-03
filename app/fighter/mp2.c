/* The fighter core's translation unit of pl_mpeg (external/pl_mpeg, MIT),
 * used only for its MP2 audio decoder (see mp2.h). pl_mpeg.h comes in
 * through -isystem (mk/fighter.mk), so its own code is held to its own
 * standard while the core keeps -Werror. The frontend links its own copy
 * (app/frontend/plm.c); mk/core-archive.sh makes every symbol of the core
 * other than retro_* local, so the two never meet. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#include "mp2.h"

struct mp2 {
	plm_buffer_t *buffer;
	plm_audio_t *audio;
	unsigned frames, frame_bytes, next, keep, kept;
	plm_audio_t snapshot;
	size_t snapshot_bits;
	int16_t snapshot_pcm[MP2_SAMPLES * 2];
};

struct mp2 *mp2_create(void)
{
	struct mp2 *m = calloc(1, sizeof(*m));
	static uint8_t empty[4];
	if (!m)
		return NULL;
	m->buffer = plm_buffer_create_with_memory(empty, 0, 0);
	m->audio = m->buffer ? plm_audio_create_with_buffer(m->buffer, 0) : NULL;
	if (!m->audio) {
		mp2_destroy(m);
		return NULL;
	}
	return m;
}

void mp2_destroy(struct mp2 *m)
{
	if (!m)
		return;
	if (m->audio)
		plm_audio_destroy(m->audio);
	if (m->buffer)
		plm_buffer_destroy(m->buffer);
	free(m);
}

/* What plm_audio_create_with_buffer does, in place, at frame f. */
static void restart(struct mp2 *m, unsigned f)
{
	plm_audio_t *a = m->audio;
	plm_buffer_seek(m->buffer, (size_t)f * m->frame_bytes);
	memset(a, 0, sizeof(*a));
	a->samples.count = PLM_AUDIO_SAMPLES_PER_FRAME;
	a->buffer = m->buffer;
	a->samplerate_index = 3;
	memcpy(a->D, PLM_AUDIO_SYNTHESIS_WINDOW, 512 * sizeof(float));
	memcpy(a->D + 512, PLM_AUDIO_SYNTHESIS_WINDOW, 512 * sizeof(float));
	/* The synthesis sums in an order set by v_pos, which moves 36 x 64
	 * per frame: give frame f the position a decode from frame 0 has, so
	 * the floats round the same way. */
	a->v_pos = (int)((1024u - 256u * (f & 3)) & 1023);
	a->next_frame_data_size = plm_audio_decode_header(a);
	m->next = f;
}

void mp2_open(struct mp2 *m, const uint8_t *data, unsigned frames,
	      unsigned frame_bytes, unsigned keep)
{
	plm_buffer_t *b = m->buffer;
	/* A fixed memory buffer over the track; pl_mpeg never writes to it. */
	b->bytes = (uint8_t *)(uintptr_t)data;
	b->length = b->capacity = b->total_size = (size_t)frames * frame_bytes;
	m->frames = frames;
	m->frame_bytes = frame_bytes;
	m->keep = keep;
	m->kept = 0;
	restart(m, 0);
}

static int decode(struct mp2 *m, int16_t *pcm)
{
	plm_samples_t *s = plm_audio_decode(m->audio);
	if (!s)
		return -1;
	m->next++;
	if (!pcm)
		return 1;
	for (int i = 0; i < MP2_SAMPLES * 2; i++) {
		float v = s->interleaved[i] * 32767.0f;
		v = v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v;
		pcm[i] = (int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
	}
	return 1;
}

int mp2_frame(struct mp2 *m, unsigned f, int16_t pcm[MP2_SAMPLES * 2])
{
	int n = 0;
	if (f >= m->frames)
		return -1;
	if (f != m->next) {
		if (f == m->keep && m->kept) {
			*m->audio = m->snapshot;
			m->buffer->bit_index = m->snapshot_bits;
			m->buffer->has_ended = 0;
			m->next = f + 1;
			memcpy(pcm, m->snapshot_pcm, sizeof(m->snapshot_pcm));
			return 0;
		}
		restart(m, f ? f - 1 : 0);
		if (f && (n = decode(m, NULL)) < 0)
			return -1;
	}
	if (decode(m, pcm) < 0)
		return -1;
	if (f == m->keep && !m->kept) {
		m->snapshot = *m->audio;
		m->snapshot_bits = m->buffer->bit_index;
		memcpy(m->snapshot_pcm, pcm, sizeof(m->snapshot_pcm));
		m->kept = 1;
	}
	return n + 1;
}
