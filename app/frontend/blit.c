/*
 * Scale and rotate in one pass. The scanout buffer is write-combined:
 * writes are fast only when sequential, and reads are very slow. So each
 * native row is assembled in a small cached line buffer and then copied
 * out with one memcpy. With rotation, a native row is a logical column, so
 * the source is read down a column (strided, but the whole GBA frame is
 * 75KiB and the lines touched per column stay in L1), and each line
 * buffer is reused for the `scale` identical rows it produces.
 *
 * Effects (blit_fx) are table lookups done while building a line, once
 * per source pixel. The LCD grid needs two line buffers per source line:
 * one for the native row through the lit pixels of the cells and one for
 * the remaining rows, which are all dark.
 */
#include <string.h>

#include "blit.h"

#define MAX_LINE 2048

static void reverse(uint16_t *line, int len)
{
	for (int i = 0, j = len - 1; i < j; i++, j--) {
		uint16_t t = line[i];

		line[i] = line[j];
		line[j] = t;
	}
}

/*
 * One logical column (rotated) or row (rot 0) of source pixels, scaled,
 * into a line buffer, at offset `off`, with stride `step` in the source.
 * With the grid, `lit` gets the line through the top of the cells (lit
 * pixel then dark ones) and `dark` the other lines of the cells.
 */
static void build_line(uint16_t *lit, uint16_t *dark, int len, int off,
		       int rev, const uint16_t *src, long step, int n,
		       int scale, const struct blit_fx *fx)
{
	uint16_t *p = lit + off;

	memset(lit, 0, (size_t)len * 2);
	if (!fx) {
		if (scale == 2) {
			for (int i = 0; i < n; i++) {
				uint32_t v = src[i * step];

				v |= v << 16;
				memcpy(p + 2 * i, &v, 4);
			}
		} else {
			for (int i = 0; i < n; i++) {
				uint16_t v = src[i * step];

				for (int k = 0; k < scale; k++)
					*p++ = v;
			}
		}
	} else if (!fx->dark) {
		const uint16_t *lut = fx->lit;

		for (int i = 0; i < n; i++) {
			uint16_t v = lut[FX_INDEX(src[i * step])];

			for (int k = 0; k < scale; k++)
				*p++ = v;
		}
	} else if (scale == 2) {
		const uint16_t *ll = fx->lit, *dl = fx->dark;
		uint16_t *q = dark + off;

		memset(dark, 0, (size_t)len * 2);
		for (int i = 0; i < n; i++) {
			unsigned x = FX_INDEX(src[i * step]);
			uint32_t d = dl[x];
			uint32_t a = ll[x] | d << 16, b = d | d << 16;

			memcpy(p + 2 * i, &a, 4);
			memcpy(q + 2 * i, &b, 4);
		}
	} else {
		uint16_t *q = dark + off;

		memset(dark, 0, (size_t)len * 2);
		for (int i = 0; i < n; i++) {
			unsigned x = FX_INDEX(src[i * step]);
			uint16_t d = fx->dark[x];

			*p++ = fx->lit[x];
			*q++ = d;
			for (int k = 1; k < scale; k++) {
				*p++ = d;
				*q++ = d;
			}
		}
	}
	/* Filled in logical order; reversed afterwards for rotation 90. */
	if (rev) {
		reverse(lit, len);
		if (fx && fx->dark)
			reverse(dark, len);
	}
}

void blit_rotate(uint16_t *dst, int dpitch, int nw, int nh, int rot,
		 const uint16_t *src, int spitch, int sw, int sh, int scale,
		 const struct blit_fx *fx)
{
	uint16_t line[MAX_LINE], dline[MAX_LINE];
	int lw, lh, iw = sw * scale, ih = sh * scale, ox, oy;
	int grid, built = -1;

	if (fx && !fx->lit)
		fx = NULL;
	grid = fx && fx->dark;
	if (rot == 90 || rot == 270) {
		lw = nh;
		lh = nw;
	} else {
		lw = nw;
		lh = nh;
	}
	ox = (lw - iw) / 2;
	oy = (lh - ih) / 2;
	if (scale < 1 || ox < 0 || oy < 0 || nw > MAX_LINE ||
	    (rot != 0 && rot != 90 && rot != 270)) {
		blit_reference(dst, dpitch, nw, nh, rot, src, spitch, sw, sh,
			       scale, fx);
		return;
	}

	/*
	 * Each native row is a logical row (rot 0) or column (90, 270): find
	 * which source line it shows and which line of the cell it is.
	 */
	for (int ny = 0; ny < nh; ny++) {
		uint16_t *row = dst + (long)ny * dpitch;
		int l, o, size, s;

		if (rot == 0) {
			l = ny - oy;
			size = ih;
		} else {
			l = (rot == 270 ? lw - 1 - ny : ny) - ox;
			size = iw;
		}
		if (l < 0 || l >= size) {
			memset(row, 0, (size_t)nw * 2);
			continue;
		}
		s = l / scale;
		if (s != built) {
			if (rot == 0)
				build_line(line, dline, nw, ox, 0,
					   src + (long)s * spitch, 1, sw, scale, fx);
			else
				build_line(line, dline, nw, oy, rot == 90, src + s,
					   spitch, sh, scale, fx);
			built = s;
		}
		o = l - s * scale;
		memcpy(row, grid && o ? dline : line, (size_t)nw * 2);
	}
}

void blit_reference(uint16_t *dst, int dpitch, int nw, int nh, int rot,
		    const uint16_t *src, int spitch, int sw, int sh, int scale,
		    const struct blit_fx *fx)
{
	int lw = rot == 0 ? nw : nh, lh = rot == 0 ? nh : nw;
	int ox = (lw - sw * scale) / 2, oy = (lh - sh * scale) / 2;

	for (int ny = 0; ny < nh; ny++)
		for (int nx = 0; nx < nw; nx++) {
			int lx, ly, sx, sy;
			uint16_t v = 0;

			switch (rot) {
			case 270:
				lx = lw - 1 - ny;
				ly = nx;
				break;
			case 90:
				lx = ny;
				ly = lh - 1 - nx;
				break;
			default:
				lx = nx;
				ly = ny;
				break;
			}
			lx -= ox;
			ly -= oy;
			if (lx >= 0 && ly >= 0) {
				sx = lx / scale;
				sy = ly / scale;
				if (sx < sw && sy < sh)
					v = src[(long)sy * spitch + sx];
				if (sx < sw && sy < sh && fx && fx->lit) {
					unsigned x = FX_INDEX(v);
					int top_left = lx % scale == 0 &&
						       ly % scale == 0;

					v = fx->dark && !top_left ? fx->dark[x] :
					    fx->lit[x];
				}
			}
			dst[(long)ny * dpitch + nx] = v;
		}
}
