/*
 * pl_mpeg (external/pl_mpeg, implemented in plm.c) for the video player,
 * plus what plm.c adds to its API.
 */
#ifndef PLM_H
#define PLM_H

/* pl_mpeg.h uses these without including them. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "pl_mpeg.h"

/* Channels in the selected audio stream's header: 1 (mono, decoded into
 * both channels), 2, or 0 without audio. */
int plm_audio_channels(plm_t *plm);

#endif
