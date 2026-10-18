/*
 * Menu drawing on a logical RGB565 canvas, with the 8x8 font from hwtest.
 */
#include <stdarg.h>
#include <stdio.h>

#include "draw.h"
#include "font8x8.h"

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

static void draw_char(struct canvas *c, int x, int y, int scale, uint16_t col,
		      unsigned char ch)
{
	const uint8_t *g;

	if (ch < FONT_FIRST || ch > FONT_LAST)
		ch = '?';
	g = font8x8[ch - FONT_FIRST];
	for (int r = 0; r < 8; r++)
		for (int b = 0; b < 8; b++)
			if (g[r] & (1u << b))
				draw_fill(c, x + b * scale, y + r * scale, scale,
					  scale, col);
}

int draw_text(struct canvas *c, int x, int y, int scale, uint16_t col,
	      const char *s)
{
	for (; *s; s++, x += 8 * scale)
		draw_char(c, x, y, scale, col, (unsigned char)*s);
	return x;
}

int draw_textf(struct canvas *c, int x, int y, int scale, uint16_t col,
	       const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return draw_text(c, x, y, scale, col, buf);
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
