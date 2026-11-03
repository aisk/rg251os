/*
 * Scale and rotate in one pass. The scanout buffer is write-combined:
 * writes are fast only when sequential, and reads are very slow. So each
 * native row is assembled in a small cached line buffer and then copied
 * out with one memcpy, in native row order.
 *
 * With rotation a native row is a logical column, so the source has to
 * be transposed. Reading it down one column at a time touches a new
 * cache line per pixel and, for a 480x320 frame (300 KiB, more than the
 * L2), waits on DRAM for most of them. Instead the source is taken in
 * bands of BAND columns: each band is read row by row (one cache line
 * per row), transposed in 8x8 blocks (NEON on arm64, a plain loop
 * elsewhere) into one cached buffer per column, with the rows a little
 * ahead (then the next band's first rows) prefetched meanwhile. At 1x
 * without effects the columns are transposed straight into the line
 * buffers, one per native row; otherwise each column is then expanded
 * (scale, effects) into a line buffer that is reused for the `scale`
 * native rows it produces. The borders of the line buffers
 * are cleared once per call, since they are the same for every row.
 *
 * Effects (blit_fx) are table lookups done while expanding a column,
 * once per source pixel. The LCD grid needs two line buffers per source
 * line: one for the native row through the lit pixels of the cells and
 * one for the remaining rows, which are all dark. For rotation 90 a
 * native row is a logical column bottom to top: the columns are
 * transposed reversed, and the borders and the lit pixel inside a cell
 * mirrored to match.
 *
 * blit_rows copies a band of logical rows at 1x over a buffer already
 * filled (the video OSD's strips, osd.c): with rotation a logical row
 * is a native column, so each native row gets one span of the band's
 * height, transposed the same way and written with one memcpy.
 *
 * The buffers are static (136 KiB, too much for a thread's stack): the
 * blits run on the main thread only.
 */
#include <string.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "blit.h"

#define MAX_LINE 2048
/* Source columns transposed per pass: 64 bytes, a cache line, of each
 * source row. 32 lines of a 320 pixel native row are 20 KiB, which stays
 * in the A35's 32 KiB L1 together with the source rows in flight. */
#define BAND 32

static _Alignas(64) uint16_t band_buf[BAND * MAX_LINE];
static _Alignas(64) uint16_t line_buf[MAX_LINE], dline_buf[MAX_LINE];

#ifdef __ARM_NEON
/*
 * One 8x8 block: out[c * os + j] = s[j * sp + c], or with rev the rows
 * taken bottom up (out[c * os + j] = s[(7 - j) * sp + c]).
 */
static inline void transpose8(uint16_t *out, long os, const uint16_t *s,
			      long sp, int rev)
{
	uint16x8_t r[8];
	uint32x4_t b[8];

	for (int k = 0; k < 8; k++)
		r[k] = vld1q_u16(s + (rev ? 7 - k : k) * sp);
	/* 16 bit pairs, then 32 bit pairs, then 64 bit halves. */
	for (int k = 0; k < 8; k += 2) {
		uint16x8_t lo = vtrn1q_u16(r[k], r[k + 1]);
		uint16x8_t hi = vtrn2q_u16(r[k], r[k + 1]);

		r[k] = lo;
		r[k + 1] = hi;
	}
	for (int k = 0; k < 8; k += 4)
		for (int j = 0; j < 2; j++) {
			uint32x4_t x = vreinterpretq_u32_u16(r[k + j]);
			uint32x4_t y = vreinterpretq_u32_u16(r[k + j + 2]);

			b[k + j] = vtrn1q_u32(x, y);
			b[k + j + 2] = vtrn2q_u32(x, y);
		}
	/* b[0..3]: columns 0-3 (low half) and 4-7 (high half) of rows 0-3,
	 * b[4..7] the same for rows 4-7. */
	for (int c = 0; c < 4; c++) {
		uint64x2_t x = vreinterpretq_u64_u32(b[c]);
		uint64x2_t y = vreinterpretq_u64_u32(b[c + 4]);

		vst1q_u16(out + c * os, vreinterpretq_u16_u64(vtrn1q_u64(x, y)));
		vst1q_u16(out + (c + 4) * os,
			  vreinterpretq_u16_u64(vtrn2q_u64(x, y)));
	}
}
#endif

/* Source rows prefetched ahead of the one being transposed: far enough
 * to hide a DRAM access, near enough to stay in L1. */
#define PREFETCH_ROWS 24

/* A band of source columns: the first one and how many. */
struct band {
	const uint16_t *p;
	int cols;
};

static inline void prefetch_row(struct band cur, struct band next, long sp,
				int n, int r)
{
	const uint16_t *p;

	if (r < n)
		p = cur.p + r * sp;
	else if (next.p && r < 2 * n)
		p = (cur = next).p + (r - n) * sp;
	else
		return;
	/* The band's run of a row may straddle two cache lines. */
	__builtin_prefetch(p);
	__builtin_prefetch(p + cur.cols - 1);
}

/*
 * Transpose a band of source columns, n pixels each: out[c * os + i] =
 * src[i * sp + c], or with rev out[c * os + n - 1 - i]. The rows ahead,
 * then those of the next band (next.p NULL if none), are prefetched.
 */
static void transpose(uint16_t *out, long os, struct band cur, long sp,
		      int n, int rev, struct band next)
{
	const uint16_t *src = cur.p;
	int ncols = cur.cols, i = 0;

#ifdef __ARM_NEON
	int c8 = ncols & ~7;

	for (; i + 8 <= n; i += 8) {
		const uint16_t *s = src + i * sp;
		int oi = rev ? n - 8 - i : i;

		for (int k = 0; k < 8; k++)
			prefetch_row(cur, next, sp, n, i + PREFETCH_ROWS + k);
		for (int c = 0; c < c8; c += 8)
			transpose8(out + c * os + oi, os, s + c, sp, rev);
		for (int c = c8; c < ncols; c++)
			for (int k = 0; k < 8; k++)
				out[c * os + oi + (rev ? 7 - k : k)] = s[k * sp + c];
	}
#endif
	for (; i < n; i++) {
		const uint16_t *s = src + i * sp;
		uint16_t *o = out + (rev ? n - 1 - i : i);

		prefetch_row(cur, next, sp, n, i + PREFETCH_ROWS);
		for (int c = 0; c < ncols; c++)
			o[c * os] = s[c];
	}
}

/*
 * Band b (counting from the first in native row order) of a source w
 * columns wide: native order is ascending columns with rev (rotation
 * 90), descending otherwise. p NULL past the last band.
 */
static struct band band_at(const uint16_t *src, int w, int b, int rev)
{
	struct band r = { NULL, 0 };
	int x = b * BAND;

	if (x < w) {
		r.cols = w - x < BAND ? w - x : BAND;
		r.p = src + (rev ? x : w - x - r.cols);
	}
	return r;
}

/*
 * n source pixels (contiguous, in native order) scaled into a line buffer
 * at offset `off`. With the grid, `lit` gets the line through the lit
 * pixels of the cells (the lit one at `along` inside each cell, dark ones
 * around it) and `dark` the other lines of the cells. Only the picture's
 * part of the lines is written.
 */
static void expand(uint16_t *lit, uint16_t *dark, int off, const uint16_t *cb,
		   int n, int scale, int along, const struct blit_fx *fx)
{
	uint16_t *p = lit + off;

	if (!fx) {
		if (scale == 1) {
			memcpy(p, cb, (size_t)n * 2);
		} else if (scale == 2) {
			int i = 0;

#ifdef __ARM_NEON
			for (; i + 8 <= n; i += 8) {
				uint16x8_t v = vld1q_u16(cb + i);

				vst1q_u16(p + 2 * i, vzip1q_u16(v, v));
				vst1q_u16(p + 2 * i + 8, vzip2q_u16(v, v));
			}
#endif
			for (; i < n; i++) {
				uint32_t v = cb[i];

				v |= v << 16;
				memcpy(p + 2 * i, &v, 4);
			}
		} else {
			for (int i = 0; i < n; i++) {
				uint16_t v = cb[i];

				for (int k = 0; k < scale; k++)
					*p++ = v;
			}
		}
	} else if (!fx->dark) {
		const uint16_t *lut = fx->lit;

		if (scale == 2) {
			for (int i = 0; i < n; i++) {
				uint32_t v = lut[FX_INDEX(cb[i])];

				v |= v << 16;
				memcpy(p + 2 * i, &v, 4);
			}
		} else {
			for (int i = 0; i < n; i++) {
				uint16_t v = lut[FX_INDEX(cb[i])];

				for (int k = 0; k < scale; k++)
					*p++ = v;
			}
		}
	} else if (scale == 2) {
		const uint16_t *ll = fx->lit, *dl = fx->dark;
		uint16_t *q = dark + off;
		/* Little endian: the first pixel of the pair is the low half. */
		int sl = along ? 16 : 0, sd = 16 - sl;

		for (int i = 0; i < n; i++) {
			unsigned x = FX_INDEX(cb[i]);
			uint32_t d = dl[x];
			uint32_t a = (uint32_t)ll[x] << sl | d << sd, b = d | d << 16;

			memcpy(p + 2 * i, &a, 4);
			memcpy(q + 2 * i, &b, 4);
		}
	} else {
		uint16_t *q = dark + off;

		for (int i = 0; i < n; i++) {
			unsigned x = FX_INDEX(cb[i]);
			uint16_t d = fx->dark[x];

			for (int k = 0; k < scale; k++) {
				*p++ = k == along ? fx->lit[x] : d;
				*q++ = d;
			}
		}
	}
}

static void clear_rows(uint16_t *dst, int dpitch, int nw, int y0, int y1)
{
	for (int ny = y0; ny < y1; ny++)
		memset(dst + (long)ny * dpitch, 0, (size_t)nw * 2);
}

/*
 * Rotation 90 or 270: source column s fills the native rows of logical
 * columns ox + s * scale .. + scale - 1, which for 270 run bottom up
 * (native row lw - 1 - lx). Bands are taken in native row order.
 */
static void blit_columns(uint16_t *dst, int dpitch, int nw, int nh, int rot,
			 const uint16_t *src, int spitch, int sw, int sh,
			 int scale, const struct blit_fx *fx, int ox, int oy,
			 int along, int across)
{
	int rev = rot == 90, iw = sw * scale, ih = sh * scale;
	/* Where the picture starts in a native row, and the lit pixel of a
	 * cell along it: mirrored for 90, which fills a row bottom up. */
	int off = rev ? nw - oy - ih : oy;
	int grid = fx && fx->dark, direct = scale == 1 && !fx;
	int first = rev ? ox : nh - ox - iw;  /* first native row of the picture */
	uint16_t *lines = direct ? band_buf : line_buf;
	int nlines = direct ? BAND : 1;

	if (rev)
		along = scale - 1 - along;
	/* The borders of the line buffers, the same for every row. */
	memset(lines, 0, (size_t)nlines * nw * 2);
	if (grid)
		memset(dline_buf, 0, (size_t)nw * 2);
	clear_rows(dst, dpitch, nw, 0, first);

	for (int b = 0; b * BAND < sw; b++) {
		struct band cur = band_at(src, sw, b, rev);
		struct band next = band_at(src, sw, b + 1, rev);
		int nb = cur.cols, first_col = b * BAND;

		if (direct) {
			transpose(band_buf + off, nw, cur, spitch, sh, rev, next);
			for (int k = 0; k < nb; k++) {
				int t = rev ? k : nb - 1 - k;
				int ny = first + first_col + k;

				memcpy(dst + (long)ny * dpitch, band_buf + t * nw,
				       (size_t)nw * 2);
			}
			continue;
		}
		transpose(band_buf, sh, cur, spitch, sh, rev, next);
		for (int k = 0; k < nb; k++) {
			int t = rev ? k : nb - 1 - k;
			int ny = first + (first_col + k) * scale;

			expand(line_buf, dline_buf, off, band_buf + t * sh, sh,
			       scale, along, fx);
			/* o: the row inside the cell, logical top to bottom
			 * is native order for 90 and reverse for 270. */
			for (int j = 0; j < scale; j++, ny++) {
				int o = rev ? j : scale - 1 - j;

				memcpy(dst + (long)ny * dpitch,
				       grid && o != across ? dline_buf : line_buf,
				       (size_t)nw * 2);
			}
		}
	}
	clear_rows(dst, dpitch, nw, first + iw, nh);
}

void blit_rotate(uint16_t *dst, int dpitch, int nw, int nh, int rot,
		 const uint16_t *src, int spitch, int sw, int sh, int scale,
		 const struct blit_fx *fx)
{
	int lw, lh, iw = sw * scale, ih = sh * scale, ox, oy;
	int grid, built = -1, along, across;

	if (fx && !fx->lit)
		fx = NULL;
	grid = fx && fx->dark;
	/* Lit pixel of a cell: along the native row, and which row. */
	along = across = 0;
	if (grid && scale > 0) {
		along = (rot == 0 ? fx->lit_x : fx->lit_y) % scale;
		across = (rot == 0 ? fx->lit_y : fx->lit_x) % scale;
	}
	if (rot == 90 || rot == 270) {
		lw = nh;
		lh = nw;
	} else {
		lw = nw;
		lh = nh;
	}
	ox = (lw - iw) / 2;
	oy = (lh - ih) / 2;
	/* Anything that does not fit (ox, oy round toward 0) goes through the
	 * per pixel version, which clips. */
	if (scale < 1 || iw > lw || ih > lh || nw > MAX_LINE ||
	    (rot != 0 && rot != 90 && rot != 270)) {
		blit_reference(dst, dpitch, nw, nh, rot, src, spitch, sw, sh,
			       scale, fx);
		return;
	}
	if (rot != 0) {
		blit_columns(dst, dpitch, nw, nh, rot, src, spitch, sw, sh,
			     scale, fx, ox, oy, along, across);
		return;
	}

	/* Rotation 0: each native row is a logical row; find which source
	 * row it shows and which line of the cell it is. */
	memset(line_buf, 0, (size_t)nw * 2);
	if (grid)
		memset(dline_buf, 0, (size_t)nw * 2);
	for (int ny = 0; ny < nh; ny++) {
		uint16_t *row = dst + (long)ny * dpitch;
		int l = ny - oy, s;

		if (l < 0 || l >= ih) {
			memset(row, 0, (size_t)nw * 2);
			continue;
		}
		s = l / scale;
		if (s != built) {
			expand(line_buf, dline_buf, ox, src + (long)s * spitch, sw,
			       scale, along, fx);
			built = s;
		}
		memcpy(row, grid && l - s * scale != across ? dline_buf :
		       line_buf, (size_t)nw * 2);
	}
}

void blit_rows(uint16_t *dst, int dpitch, int nw, int nh, int rot,
	       const uint16_t *src, int spitch, int y0, int rows)
{
	int lw = rot == 0 ? nw : nh, lh = rot == 0 ? nh : nw;
	int rev = rot == 90, off = rev ? lh - y0 - rows : y0;

	if (y0 < 0 || rows <= 0 || y0 + rows > lh || rows > MAX_LINE)
		return;
	if (rot == 0) {
		for (int i = 0; i < rows; i++)
			memcpy(dst + (long)(y0 + i) * dpitch,
			       src + (long)i * spitch, (size_t)nw * 2);
		return;
	}
	/* 270: native (nx, ny) shows logical (lw-1-ny, nx); 90: (ny,
	 * lh-1-nx), the band's rows in reverse. Native rows in order, so
	 * logical columns descending for 270. */
	for (int b = 0; b * BAND < lw; b++) {
		struct band cur = band_at(src, lw, b, rev);

		transpose(band_buf, rows, cur, spitch, rows, rev,
			  band_at(src, lw, b + 1, rev));
		for (int k = 0; k < cur.cols; k++) {
			int t = rev ? k : cur.cols - 1 - k;

			memcpy(dst + (long)(b * BAND + k) * dpitch + off,
			       band_buf + t * rows, (size_t)rows * 2);
		}
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
					int is_lit = lx % scale == fx->lit_x % scale &&
						     ly % scale == fx->lit_y % scale;

					v = fx->dark && !is_lit ? fx->dark[x] :
					    fx->lit[x];
				}
			}
			dst[(long)ny * dpitch + nx] = v;
		}
}
