/*
 * The video player's on-screen display (osd.c): a strip at the top with
 * the video's name and a note (the volume, the first or last video), one
 * at the bottom with play/pause, the time and a progress bar, both over
 * a darkened copy of the picture, put over the frame main.c blitted.
 */
#ifndef OSD_H
#define OSD_H

#include <stddef.h>
#include <stdint.h>

struct canvas;
struct video_status;

#define OSD_STRIP_H 32                  /* each strip, logical px */
#define OSD_SHOW_NS 3000000000LL        /* how long a control shows it */

/* A video started (its name as shown): the OSD shows for OSD_SHOW_NS,
 * its statistics start over. */
void osd_start(const char *title, int64_t now);
/* A note right in the top strip for OSD_SHOW_NS, which shows the OSD. */
void osd_note(const char *note, int64_t now);
/* The held buttons once per frame in game: B toggles the OSD. */
void osd_input(uint32_t held, int64_t now);
/* Once per frame in game after the picture went into the native buffer
 * (as blit_rotate put it there, the frame fr at the largest integer
 * scale that fits, centered): the strips over it while the OSD shows,
 * nothing otherwise. */
void osd_frame(uint16_t *dst, int dpitch, int nw, int nh, int rot,
	       const uint16_t *fr, int fp, int fw, int fh, int64_t now);
/* Whether the OSD shows now (a control since the last look counts). */
int osd_shown(int64_t now);
/* The strips into a logical canvas (only their rows), over the frame as
 * osd_frame has it, for status st at now. */
void osd_draw(struct canvas *c, const struct video_status *st,
	      const uint16_t *fr, int fp, int fw, int fh, int64_t now);
/* "m:ss", or "h:mm:ss" with hours; t in seconds, cut to whole ones. */
void osd_format_time(double t, int hours, char *buf, size_t size);
/* "video: OSD (why): ..." with the frames drawn and their cost. */
void osd_log_stats(const char *why);

#endif
