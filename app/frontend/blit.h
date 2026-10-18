#ifndef BLIT_H
#define BLIT_H

#include <stdint.h>

/*
 * Per pixel effects, as lookup tables indexed by RGB555 (see FX_INDEX):
 * lit gives the output color (color correction), dark, when not NULL,
 * turns on the LCD grid: in each scale x scale cell only the pixel at
 * (lit_x, lit_y) inside the cell, in logical (game) orientation, uses lit
 * and the others dark. 0, 0 is the top-left one; offsets are taken
 * modulo the scale.
 */
struct blit_fx {
	const uint16_t *lit;
	const uint16_t *dark;
	int lit_x, lit_y;
};

/*
 * The core's RGB565 to a 15 bit table index (r << 10 | g << 5 | b). mGBA
 * expands 5 bit green to 6 bits by a shift, so the low green bit is always
 * 0 and dropping it loses nothing.
 */
#define FX_INDEX(v) ((((v) >> 1) & 0x7fe0) | ((v) & 0x1f))

/*
 * Copy an RGB565 image into a native scanout buffer of nw x nh pixels,
 * scaled by an integer factor, centered in the logical (rotated) screen,
 * with black borders. rot is 0, 90 or 270 degrees counter clockwise, as in
 * hwtest: for 270, native pixel (nx, ny) shows logical (lw-1-ny, nx).
 * Pitches are in pixels. fx may be NULL (plain copy).
 */
void blit_rotate(uint16_t *dst, int dpitch, int nw, int nh, int rot,
		 const uint16_t *src, int spitch, int sw, int sh, int scale,
		 const struct blit_fx *fx);

/* Straightforward per-pixel version with the same result, for tests. */
void blit_reference(uint16_t *dst, int dpitch, int nw, int nh, int rot,
		    const uint16_t *src, int spitch, int sw, int sh, int scale,
		    const struct blit_fx *fx);

#endif
