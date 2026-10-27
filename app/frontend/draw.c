/*
 * Menu drawing on a logical RGB565 canvas; text in the anti-aliased ASCII
 * font of font.h, blended over the canvas, at its own size or scaled up
 * (nearest neighbor) for the clock page.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "blit.h"
#include "draw.h"

void draw_fill(struct canvas *c, int x, int y, int w, int h, uint16_t col)
{
	int x1 = x + w, y1 = y + h;

	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x1 > c->w)
		x1 = c->w;
	if (y1 > c->h)
		y1 = c->h;
	for (int j = y; j < y1; j++) {
		uint16_t *p = c->px + (long)j * c->w;

		for (int i = x; i < x1; i++)
			p[i] = col;
	}
}

void draw_frame(struct canvas *c, int x, int y, int w, int h, uint16_t col)
{
	draw_fill(c, x, y, w, 2, col);
	draw_fill(c, x, y + h - 2, w, 2, col);
	draw_fill(c, x, y, 2, h, col);
	draw_fill(c, x + w - 2, y, 2, h, col);
}

/* col over the pixel at alpha a (0..15), per RGB565 channel. */
static uint16_t blend(uint16_t bg, uint16_t col, int a)
{
	int r = bg >> 11, g = (bg >> 5) & 63, b = bg & 31;

	r += ((col >> 11) - r) * a / 15;
	g += (((col >> 5) & 63) - g) * a / 15;
	b += ((col & 31) - b) * a / 15;
	return (uint16_t)(r << 11 | g << 5 | b);
}

/* One glyph, only its columns in [x0, x1) (and on the canvas). */
static void draw_char(struct canvas *c, int x, int y, int scale, uint16_t col,
		      unsigned char ch, int x0, int x1)
{
	const uint8_t (*g)[FONT_W / 2];

	if (ch < FONT_FIRST || ch > FONT_LAST)
		ch = '?';
	g = font_glyphs[ch - FONT_FIRST];
	if (x0 < 0)
		x0 = 0;
	if (x1 > c->w)
		x1 = c->w;
	for (int r = 0; r < FONT_H * scale; r++) {
		const uint8_t *row = g[r / scale];
		int py = y + r;
		uint16_t *p;

		if (py < 0 || py >= c->h)
			continue;
		p = c->px + (long)py * c->w;
		for (int i = 0; i < FONT_W * scale; i++) {
			int px = x + i, k = i / scale;
			int a = (row[k / 2] >> (k & 1 ? 0 : 4)) & 15;

			if (!a || px < x0 || px >= x1)
				continue;
			p[px] = a == 15 ? col : blend(p[px], col, a);
		}
	}
}

/* The font at scale, only the columns in [x0, x1). */
static int text_at(struct canvas *c, int x, int y, int scale, uint16_t col,
		   const char *s, int x0, int x1)
{
	for (; *s; s++, x += FONT_W * scale)
		if (x + FONT_W * scale > x0 && x < x1)
			draw_char(c, x, y, scale, col, (unsigned char)*s, x0, x1);
	return x;
}

int draw_text(struct canvas *c, int x, int y, uint16_t col, const char *s)
{
	return text_at(c, x, y, 1, col, s, 0, c->w);
}

int draw_text_clip(struct canvas *c, int x, int y, uint16_t col,
		   const char *s, int x0, int x1)
{
	return text_at(c, x, y, 1, col, s, x0, x1);
}

int draw_textf(struct canvas *c, int x, int y, uint16_t col,
	       const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return draw_text(c, x, y, col, buf);
}

int draw_text_scaled(struct canvas *c, int x, int y, int scale, uint16_t col,
		     const char *s)
{
	return text_at(c, x, y, scale, col, s, 0, c->w);
}

int draw_text_width(const char *s)
{
	return FONT_W * (int)strlen(s);
}

void draw_image_dim(struct canvas *c, const uint16_t *src, int spitch,
		    int sw, int sh)
{
	int ox = (c->w - 2 * sw) / 2, oy = (c->h - 2 * sh) / 2;

	draw_fill(c, 0, 0, c->w, c->h, 0);
	if (!src || ox < 0 || oy < 0)
		return;
	for (int y = 0; y < sh; y++) {
		uint16_t *d0 = c->px + (long)(oy + 2 * y) * c->w + ox;
		uint16_t *d1 = d0 + c->w;
		const uint16_t *s = src + (long)y * spitch;

		for (int x = 0; x < sw; x++) {
			/* Quarter brightness: shift each channel by 2. */
			uint16_t v = (s[x] >> 2) & 0x39e7;

			d0[2 * x] = d0[2 * x + 1] = v;
			d1[2 * x] = d1[2 * x + 1] = v;
		}
	}
}

void draw_image_fx(struct canvas *c, int x0, int y0, const uint16_t *src,
		   int spitch, int sw, int sh, const struct blit_fx *fx)
{
	int lx = 0, ly = 0;

	if (fx && !fx->lit)
		fx = NULL;
	if (fx && fx->dark) {
		lx = fx->lit_x & 1;
		ly = fx->lit_y & 1;
	}
	for (int y = 0; y < sh; y++) {
		const uint16_t *s = src + (long)y * spitch;
		int dy = y0 + 2 * y;
		uint16_t *d0, *d1;

		if (dy < 0 || dy + 1 >= c->h)
			continue;
		d0 = c->px + (long)dy * c->w;
		d1 = d0 + c->w;
		for (int x = 0; x < sw; x++) {
			int dx = x0 + 2 * x;
			uint16_t a = s[x], b;

			if (dx < 0 || dx + 1 >= c->w)
				continue;
			if (fx) {
				unsigned i = FX_INDEX(a);

				a = fx->lit[i];
				b = fx->dark ? fx->dark[i] : a;
			} else {
				b = a;
			}
			/* Lit pixel at (lit_x, lit_y), as in blit_rotate(). */
			d0[dx] = d0[dx + 1] = d1[dx] = d1[dx + 1] = b;
			(ly ? d1 : d0)[dx + lx] = a;
		}
	}
}
