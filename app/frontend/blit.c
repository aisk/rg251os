/*
 * Scale and rotate in one pass. The scanout buffer is write-combined:
 * writes are fast only when sequential, and reads are very slow. So each
 * native row is assembled in a small cached line buffer and then copied
 * out with one memcpy. With rotation, a native row is a logical column, so
 * the source is read down a column (strided, but the whole GBA frame is
 * 75KiB and the lines touched per column stay in L1), and each line
 * buffer is reused for the `scale` identical rows it produces.
 */
#include <string.h>

#include "blit.h"

#define MAX_LINE 2048

static void build_column(uint16_t *line, int len, int oy, int rev,
			 const uint16_t *src, int spitch, int sh, int scale)
{
	uint16_t *p;

	memset(line, 0, (size_t)len * 2);
	/* Fill in logical y order; reversed afterwards for rotation 90. */
	p = line + oy;
	if (scale == 2) {
		for (int sy = 0; sy < sh; sy++) {
			uint32_t v = src[(long)sy * spitch];

			v |= v << 16;
			memcpy(p + 2 * sy, &v, 4);
		}
	} else {
		for (int sy = 0; sy < sh; sy++) {
			uint16_t v = src[(long)sy * spitch];

			for (int k = 0; k < scale; k++)
				*p++ = v;
		}
	}
	if (rev) {
		for (int i = 0, j = len - 1; i < j; i++, j--) {
			uint16_t t = line[i];

			line[i] = line[j];
			line[j] = t;
		}
	}
}

void blit_rotate(uint16_t *dst, int dpitch, int nw, int nh, int rot,
		 const uint16_t *src, int spitch, int sw, int sh, int scale)
{
	uint16_t line[MAX_LINE];
	int lw, lh, iw = sw * scale, ih = sh * scale, ox, oy;

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
		blit_reference(dst, dpitch, nw, nh, rot, src, spitch, sw, sh, scale);
		return;
	}

	if (rot == 0) {
		int built = -1;

		for (int ny = 0; ny < nh; ny++) {
			uint16_t *row = dst + (long)ny * dpitch;
			int sy;

			if (ny < oy || ny >= oy + ih) {
				memset(row, 0, (size_t)nw * 2);
				continue;
			}
			sy = (ny - oy) / scale;
			if (sy != built) {
				const uint16_t *s = src + (long)sy * spitch;
				uint16_t *p = line + ox;

				memset(line, 0, (size_t)nw * 2);
				for (int sx = 0; sx < sw; sx++)
					for (int k = 0; k < scale; k++)
						*p++ = s[sx];
				built = sy;
			}
			memcpy(row, line, (size_t)nw * 2);
		}
		return;
	}

	int built = -1;

	for (int ny = 0; ny < nh; ny++) {
		uint16_t *row = dst + (long)ny * dpitch;
		int lx = rot == 270 ? lw - 1 - ny : ny, sx;

		if (lx < ox || lx >= ox + iw) {
			memset(row, 0, (size_t)nw * 2);
			continue;
		}
		sx = (lx - ox) / scale;
		if (sx != built) {
			build_column(line, nw, oy, rot == 90, src + sx, spitch,
				     sh, scale);
			built = sx;
		}
		memcpy(row, line, (size_t)nw * 2);
	}
}

void blit_reference(uint16_t *dst, int dpitch, int nw, int nh, int rot,
		    const uint16_t *src, int spitch, int sw, int sh, int scale)
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
			}
			dst[(long)ny * dpitch + nx] = v;
		}
}
