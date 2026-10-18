#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

/*
 * KMS output through dumb buffers: two RGB565 buffers in the panel's
 * native (portrait) orientation, page flipped on vblank. Everything is
 * drawn in logical landscape coordinates and rotated while copying.
 */
struct display_info {
	int native_w, native_h;  /* scanout size, 320x480 on the RG351P */
	int logical_w, logical_h;/* after rotation, 480x320 */
	int rotation;            /* 0, 90 or 270 */
	double refresh_hz;       /* from the mode timings */
};

int display_init(void);
int display_ready(void);
int display_fd(void);
const struct display_info *display_info(void);
/* True when a back buffer is free to draw into. */
int display_can_draw(void);
uint16_t *display_backbuf(int *pitch_px);
void display_present(void);
void display_handle_events(void);
/* Call regularly; recovers from a lost flip event. Returns poll timeout. */
int display_poll_timeout(int64_t now);
/* Vblanks that passed without a new frame since the last call. */
uint32_t display_take_missed(void);
uint64_t display_frames(void);

#endif
