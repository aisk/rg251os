#ifndef BLIT_H
#define BLIT_H

#include <stdint.h>

/*
 * Copy an RGB565 image into a native scanout buffer of nw x nh pixels,
 * scaled by an integer factor, centered in the logical (rotated) screen,
 * with black borders. rot is 0, 90 or 270 degrees counter clockwise, as in
 * hwtest: for 270, native pixel (nx, ny) shows logical (lw-1-ny, nx).
 * Pitches are in pixels.
 */
void blit_rotate(uint16_t *dst, int dpitch, int nw, int nh, int rot,
		 const uint16_t *src, int spitch, int sw, int sh, int scale);

/* Straightforward per-pixel version with the same result, for tests. */
void blit_reference(uint16_t *dst, int dpitch, int nw, int nh, int rot,
		    const uint16_t *src, int spitch, int sw, int sh, int scale);

#endif
