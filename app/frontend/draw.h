#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>

#define RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

#define COL_BG     RGB(0x10, 0x18, 0x28)
#define COL_FG     RGB(0xff, 0xff, 0xff)
#define COL_DIM    RGB(0x90, 0x98, 0xa8)
#define COL_HI     RGB(0xff, 0xd0, 0x40)
#define COL_SEL    RGB(0x30, 0x50, 0x90)
#define COL_ERR    RGB(0xff, 0x50, 0x50)
#define COL_BOX    RGB(0x18, 0x20, 0x38)
#define COL_OK     RGB(0x60, 0xe0, 0x60)

/* The text cell of draw_text: font8x8.h at 2x. */
#define FONT_W 16
#define FONT_H 16

/* The menu canvas, the logical (landscape) screen. */
#define CANVAS_W 480
#define CANVAS_H 320

/* A logical (landscape) RGB565 canvas for menus. */
struct canvas {
	uint16_t *px;
	int w, h;  /* pitch == w */
};

void draw_fill(struct canvas *c, int x, int y, int w, int h, uint16_t col);
void draw_frame(struct canvas *c, int x, int y, int w, int h, uint16_t col);
/* Text in FONT_W x FONT_H cells from (x, y), its top left; returns the x
 * after the text. */
int draw_text(struct canvas *c, int x, int y, uint16_t col, const char *s);
/* The same, drawing only the pixels in columns [x0, x1). */
int draw_text_clip(struct canvas *c, int x, int y, uint16_t col,
		   const char *s, int x0, int x1);
int draw_textf(struct canvas *c, int x, int y, uint16_t col,
	       const char *fmt, ...) __attribute__((format(printf, 5, 6)));
/* The font's pixels scaled by `scale` (font8x8: 8 x scale px a cell). */
int draw_text_scaled(struct canvas *c, int x, int y, int scale, uint16_t col,
		     const char *s);
/* The width of s drawn with draw_text. */
int draw_text_width(const char *s);
/* Scale an image 2x into the canvas, centered and darkened. */
void draw_image_dim(struct canvas *c, const uint16_t *src, int spitch,
		    int sw, int sh);

struct blit_fx;
/* Scale an image 2x into the canvas at (x0, y0) with effects (or NULL),
 * the same pixels blit_rotate() would produce. Clipped. */
void draw_image_fx(struct canvas *c, int x0, int y0, const uint16_t *src,
		   int spitch, int sw, int sh, const struct blit_fx *fx);

#endif
