/*
 * Video effects for the game picture: color correction and the LCD grid,
 * both as two 32768 entry tables (RGB555 index to RGB565, 64 KiB each)
 * built when a setting changes and looked up once per source pixel by the
 * blit. No float per pixel and per frame.
 *
 * Color correction. GBA games were authored for the dark, unlit
 * reflective screen of the original GBA, so on a backlit IPS panel they
 * look washed out and oversaturated. The models are Pokefan531's color
 * matrices from the libretro slang-shaders (handheld/shaders/color/
 * gba-color.slang and sp101-color.slang, sRGB profile, public domain):
 *
 *   linear = in ^ 2.2
 *   out    = clamp(M * linear * lum)
 *   result = out ^ (1/2.2 + darken / 2.2^2)
 *
 *   GBA        M = | 0.905  0.195 -0.100 |   lum 0.91   darken 1.0
 *                  | 0.100  0.650  0.250 |
 *                  | 0.1575 0.1425 0.700 |
 *   GBA light  same M and lum, darken 0 (the slang shader's default)
 *   SP101      M = | 0.960  0.110 -0.070 |   lum 0.935  darken 0
 *                  | 0.0325 0.890  0.0775|
 *                  | 0.001 -0.030  1.029 |
 *
 * (rows are output R, G, B; columns input R, G, B.) darken 1.0 gives an
 * overall exponent of 1.4545, the same as the older glsl gba-color shader
 * (in ^ (2.2 + 1.0), out ^ (1/2.2)), the classic "GBA" look.
 *
 * LCD grid. Each GBA pixel is 2x2 panel pixels; with the grid only the
 * top-left one (as the player sees it) keeps the color, optionally
 * brightened by `boost` percent, and the other three are darkened by
 * `dim` percent, like the visible gaps between the GBA LCD's pixels.
 * Both are plain scale factors on the gamma encoded value, with clamping.
 */
#include <math.h>
#include <string.h>

#include "fe.h"
#include "fx.h"

const int grid_dim_levels[] = { 25, 50, 75 };
const int grid_boost_levels[] = { 0, 15, 30, 50 };
const int grid_dim_count = ARRAY_SIZE(grid_dim_levels);
const int grid_boost_count = ARRAY_SIZE(grid_boost_levels);

struct color_model {
	float m[3][3];
	float lum, darken;
};

static const struct color_model models[COLOR_COUNT] = {
	[COLOR_GBA] = {
		{ { 0.905f, 0.195f, -0.100f },
		  { 0.100f, 0.650f, 0.250f },
		  { 0.1575f, 0.1425f, 0.700f } }, 0.91f, 1.0f,
	},
	[COLOR_GBA_LIGHT] = {
		{ { 0.905f, 0.195f, -0.100f },
		  { 0.100f, 0.650f, 0.250f },
		  { 0.1575f, 0.1425f, 0.700f } }, 0.91f, 0.0f,
	},
	[COLOR_SP101] = {
		{ { 0.960f, 0.110f, -0.070f },
		  { 0.0325f, 0.890f, 0.0775f },
		  { 0.001f, -0.030f, 1.029f } }, 0.935f, 0.0f,
	},
};

static inline int quant(float v, int max)
{
	int q = (int)(v * max + 0.5f);

	return q < 0 ? 0 : q > max ? max : q;
}

static inline uint16_t pack(const float c[3])
{
	return (uint16_t)(quant(c[0], 31) << 11 | quant(c[1], 63) << 5 |
			  quant(c[2], 31));
}

void fx_build(uint16_t *lit, uint16_t *dark, int color, int dim, int boost)
{
	const struct color_model *cm = color > COLOR_OFF && color < COLOR_COUNT ?
				       &models[color] : NULL;
	float klit = 1 + boost / 100.0f, kdark = 1 - dim / 100.0f;
	float lin[32], enc = 0;

	if (cm) {
		for (int i = 0; i < 32; i++)
			lin[i] = powf(i / 31.0f, 2.2f);
		enc = 1 / 2.2f + cm->darken / (2.2f * 2.2f);
	}
	for (int x = 0; x < FX_LUT_SIZE; x++) {
		int r = x >> 10, g = (x >> 5) & 31, b = x & 31;
		float c[3], l[3];

		if (cm) {
			for (int k = 0; k < 3; k++) {
				float v = (cm->m[k][0] * lin[r] + cm->m[k][1] * lin[g] +
					   cm->m[k][2] * lin[b]) * cm->lum;

				v = v < 0 ? 0 : v > 1 ? 1 : v;
				c[k] = powf(v, enc);
			}
		} else {
			/* As the core's own RGB565: green is g << 1. */
			c[0] = r / 31.0f;
			c[1] = (g << 1) / 63.0f;
			c[2] = b / 31.0f;
		}
		for (int k = 0; k < 3; k++)
			l[k] = c[k] * klit;
		lit[x] = pack(l);
		if (dark) {
			for (int k = 0; k < 3; k++)
				l[k] = c[k] * kdark;
			dark[x] = pack(l);
		}
	}
}

static struct {
	uint16_t lit[FX_LUT_SIZE], dark[FX_LUT_SIZE];
	struct blit_fx fx;
	int active;
	int color, grid, dim, boost;  /* what the tables hold */
	int built;
} f;

void fx_update(void)
{
	int color = rom_settings.color, grid = settings.grid;
	int dim = grid ? settings.grid_dim : 0;
	int boost = grid ? settings.grid_boost : 0;
	int64_t t0;

	f.active = color != COLOR_OFF || grid;
	if (!f.active)
		return;
	if (f.built && f.color == color && f.grid == grid && f.dim == dim &&
	    f.boost == boost)
		return;
	t0 = now_ns();
	fx_build(f.lit, grid ? f.dark : NULL, color, dim, boost);
	f.color = color;
	f.grid = grid;
	f.dim = dim;
	f.boost = boost;
	f.built = 1;
	f.fx.lit = f.lit;
	f.fx.dark = grid ? f.dark : NULL;
	log_msg("fx: color %s, grid %s (dim %d%%, boost %d%%), tables built "
		"in %.1f ms", color_mode_names[color], grid ? "on" : "off", dim,
		boost, (now_ns() - t0) / 1e6);
}

const struct blit_fx *fx_current(void)
{
	return f.active ? &f.fx : NULL;
}
