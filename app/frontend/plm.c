/*
 * The one translation unit with pl_mpeg's implementation (MPEG-PS demuxer,
 * MPEG-1 video and MP2 audio decoders, external/pl_mpeg, MIT) for the
 * video player in video_core.c. pl_mpeg.h comes in through -isystem
 * (mk/frontend.mk), so its own code is held to its own standard and the
 * frontend's warning flags stay as they are. Also the one thing the
 * public API does not tell: whether the MP2 stream is mono (pl_mpeg
 * decodes it into both channels either way), for the log.
 */
#define PL_MPEG_IMPLEMENTATION
#include "plm.h"

int plm_audio_channels(plm_t *plm)
{
	if (!plm_has_headers(plm) || !plm->audio_decoder ||
	    !plm_audio_has_header(plm->audio_decoder))
		return 0;
	return plm->audio_decoder->mode == PLM_AUDIO_MODE_MONO ? 1 : 2;
}
