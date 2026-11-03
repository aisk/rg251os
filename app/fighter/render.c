/* Bounded sprite-package decoder and clipped software draw-command backend.
 * All destination reads stay in a cached RGB565 canvas; the host presents it. */
#include "fighter.h"
#include <string.h>
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
int fight_assets(struct assets *a, const void *data, size_t size, int *automatic)
{
	const uint8_t *p = data;
	if (!p || size != 32 + PIXELS * 3 || memcmp(p, "FIGHTP01", 8) ||
	    le32(p + 8) != SW || le32(p + 12) != SH || le32(p + 16) != SPRITES ||
	    le32(p + 20) > 1)
		return 0;
	uint32_t hash = 2166136261u;
	for (size_t i = 32; i < size; i++)
		hash = (hash ^ p[i]) * 16777619u;
	if (hash != le32(p + 24) || le32(p + 28))
		return 0;
	a->hash = hash;
	*automatic = (int)le32(p + 20);
	for (int i = 0; i < PIXELS; i++) {
		a->color[i] = (uint16_t)(p[32 + i * 3] | p[33 + i * 3] << 8);
		a->alpha[i] = p[34 + i * 3];
	}
	return 1;
}
static uint16_t mix(uint16_t dst, uint16_t src, unsigned a, int mode)
{
	unsigned r = src >> 11, g = (src >> 5) & 63, b = src & 31;
	unsigned dr = dst >> 11, dg = (dst >> 5) & 63, db = dst & 31;
	if (mode == ADD) {
		r = dr + r * a / 255;
		g = dg + g * a / 255;
		b = db + b * a / 255;
		if (r > 31)
			r = 31;
		if (g > 63)
			g = 63;
		if (b > 31)
			b = 31;
	} else {
		r = (r * a + dr * (255 - a) + 127) / 255;
		g = (g * a + dg * (255 - a) + 127) / 255;
		b = (b * a + db * (255 - a) + 127) / 255;
	}
	return (uint16_t)(r << 11 | g << 5 | b);
}
void fight_render(const struct assets *a, const struct scene *s, uint16_t *fb)
{
	for (unsigned k = 0; k < s->count && k < COMMANDS; k++) {
		const struct command *c = &s->cmd[k];
		int x0 = c->x < 0 ? 0 : c->x, y0 = c->y < 0 ? 0 : c->y;
		int x1 = c->x + c->w > FW ? FW : c->x + c->w,
		    y1 = c->y + c->h > FH ? FH : c->y + c->h;
		if (c->op == SPRITE &&
		    (c->sprite < 0 || c->sprite >= SPRITES || c->w != SW || c->h != SH))
			continue;
		for (int y = y0; y < y1; y++)
			for (int x = x0; x < x1; x++) {
				uint16_t color = c->color;
				unsigned alpha = c->alpha;
				if (c->op == SPRITE) {
					int sx = x - c->x;
					if (c->flip)
						sx = SW - 1 - sx;
					int at =
					    c->sprite * SW * SH + (y - c->y) * SW + sx;
					color = a->color[at];
					alpha = alpha * a->alpha[at] / 255;
				}
				if (!alpha)
					continue;
				uint16_t *d = &fb[y * FW + x];
				*d = (alpha == 255 && c->blend != ADD)
					 ? color
					 : mix(*d, color, alpha, c->blend);
			}
	}
}
