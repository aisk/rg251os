/*
 * Unit tests for the parts that do not need the device; run with
 * `frontend --selftest DIR`, e.g. under qemu-aarch64.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "battery.h"
#include "blit.h"
#include "clock.h"
#include "core.h"
#include "core_api.h"
#include "cpuctl.h"
#include "draw.h"
#include "menu.h"
#include "fe.h"
#include "fx.h"
#include "input.h"
#include "resample.h"
#include "resume.h"
#include "romdb.h"
#include "romlist.h"
#include "sdgrow.h"
#include "settings.h"
#include "sleep.h"
#include "sysinfo.h"
#include "ui.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
	} \
} while (0)

/* The pages called directly, as ui_frame does: the action of a key press
 * (the page opened by UI_PUSH in opened, the ROM to start in path), and
 * a page drawn over a 240x160 game picture or none. Each update is a
 * vblank (PAGE_FRAME_NS, rounded up from 1/60 s) after the last. */
#define PAGE_FRAME_NS 16666667LL

static const struct page *opened;
static int64_t page_t;

static int page_key(const struct page *p, uint32_t pressed)
{
	struct ui_result r = p->update(pressed, page_t += PAGE_FRAME_NS);

	opened = r.page;
	return r.act;
}

/* path: 512 bytes */
static int rl_key(uint32_t pressed, char *path)
{
	int act = page_key(&romlist_page, pressed);

	snprintf(path, 512, "%s", romlist_menu_rom());
	return act;
}

static int ig_key(uint32_t pressed)
{
	return page_key(&ingame_page, pressed);
}

/* The global and the in-game settings share their update. */
static int st_key(uint32_t pressed)
{
	return page_key(&settings_page, pressed);
}

static int dt_key(uint32_t pressed)
{
	return page_key(&datetime_page, pressed);
}

static void ig_open(const char *name)
{
	ingame_menu_set_name(name);
	ingame_page.enter(NULL, page_t);
}

static void draw_page(const struct page *p, struct canvas *c,
		      const uint16_t *game)
{
	struct game_view g = { game, 240, 240, 160 };

	p->draw(c, game ? &g : &(struct game_view){ 0 });
}

static uint32_t rnd_state = 12345;

static uint16_t rnd16(void)
{
	rnd_state = rnd_state * 1103515245 + 12345;
	return (uint16_t)(rnd_state >> 8);
}

static void test_blit(void)
{
	static const struct { int nw, nh, rot, sw, sh, spitch, scale; } cases[] = {
		{ 320, 480, 270, 240, 160, 256, 2 },  /* GBA on the RG351P */
		{ 320, 480, 90, 240, 160, 256, 2 },
		{ 480, 320, 0, 240, 160, 256, 2 },
		{ 320, 480, 270, 480, 320, 480, 1 },  /* menu canvas */
		{ 320, 480, 90, 480, 320, 480, 1 },
		{ 320, 480, 270, 160, 144, 160, 2 },  /* smaller, centered */
		{ 320, 480, 270, 99, 77, 101, 3 },
	};
	const int dpitch = 336;  /* wider than the mode, like a real pitch */
	uint16_t *src = malloc(512 * 320 * 2);
	uint16_t *a = malloc(dpitch * 480 * 2), *b = malloc(dpitch * 480 * 2);
	int64_t t0;

	for (int i = 0; i < 512 * 320; i++)
		src[i] = rnd16();
	for (size_t k = 0; k < ARRAY_SIZE(cases); k++) {
		int nw = cases[k].nw, nh = cases[k].nh, diff = 0;

		memset(a, 0x55, dpitch * 480 * 2);
		memset(b, 0x55, dpitch * 480 * 2);
		blit_rotate(a, dpitch, nw, nh, cases[k].rot, src, cases[k].spitch,
			    cases[k].sw, cases[k].sh, cases[k].scale, NULL);
		blit_reference(b, dpitch, nw, nh, cases[k].rot, src,
			       cases[k].spitch, cases[k].sw, cases[k].sh,
			       cases[k].scale, NULL);
		for (int y = 0; y < nh; y++)
			diff += memcmp(a + y * dpitch, b + y * dpitch, nw * 2) != 0;
		CHECK(!diff, "blit case %zu: %d rows differ", k, diff);
	}

	/* Same with effects: random tables, color only and with the grid. */
	{
		static uint16_t lit[FX_LUT_SIZE], dark[FX_LUT_SIZE];
		const struct blit_fx fxs[2] = { { lit, NULL, 0, 0 }, { lit, dark, 0, 0 } };

		for (int i = 0; i < FX_LUT_SIZE; i++) {
			lit[i] = rnd16();
			dark[i] = rnd16();
		}
		for (int f = 0; f < 2; f++)
			for (size_t k = 0; k < ARRAY_SIZE(cases); k++) {
				int nw = cases[k].nw, nh = cases[k].nh, diff = 0;

				blit_rotate(a, dpitch, nw, nh, cases[k].rot, src,
					    cases[k].spitch, cases[k].sw, cases[k].sh,
					    cases[k].scale, &fxs[f]);
				blit_reference(b, dpitch, nw, nh, cases[k].rot, src,
					       cases[k].spitch, cases[k].sw,
					       cases[k].sh, cases[k].scale, &fxs[f]);
				for (int y = 0; y < nh; y++)
					diff += memcmp(a + y * dpitch, b + y * dpitch,
						       nw * 2) != 0;
				CHECK(!diff, "fx %d blit case %zu: %d rows differ", f,
				      k, diff);
			}
	}

	/* Orientation, as verified on the device: rotation 270 puts logical
	 * (0,0), the user's top-left, at native column 0 of the last row. */
	blit_rotate(a, dpitch, 320, 480, 270, src, 256, 240, 160, 2, NULL);
	CHECK(a[479 * dpitch + 0] == src[0], "top-left pixel not at native (0,479)");
	CHECK(a[478 * dpitch + 1] == src[0], "2x scaling of top-left pixel");
	CHECK(a[0 * dpitch + 319] == src[159 * 256 + 239],
	      "bottom-right pixel not at native (319,0)");
	CHECK(a[479 * dpitch + 2] == src[256], "logical y runs along native x");

	t0 = now_ns();
	for (int i = 0; i < 50; i++)
		blit_rotate(a, dpitch, 320, 480, 270, src, 256, 240, 160, 2, NULL);
	log_msg("selftest: GBA blit %.3f ms per frame (host or emulator time)",
		(now_ns() - t0) / 50 / 1e6);
	free(src);
	free(a);
	free(b);
}

static void test_resampler(void)
{
	struct resampler r;
	int16_t in[600 * 2], out[1200 * 2];
	double phase = 0, expect = 0;
	size_t total = 0;
	int maxjump = 0, prev = 0, have_prev = 0;

	rs_init(&r, 32768.0 / 48000.0);
	for (int chunk = 0; chunk < 200; chunk++) {
		int n = chunk & 1 ? 549 : 548;
		size_t got;

		for (int i = 0; i < n; i++) {
			int16_t v = (int16_t)(20000 * sin(phase));

			phase += 2 * M_PI * 1000 / 32768;
			in[2 * i] = v;
			in[2 * i + 1] = (int16_t)-v;
		}
		got = rs_process(&r, in, n, out, 1200);
		for (size_t i = 0; i < got; i++) {
			int d;

			CHECK(out[2 * i] == -out[2 * i + 1] ||
			      out[2 * i] == -out[2 * i + 1] - 1 ||
			      out[2 * i] == -out[2 * i + 1] + 1,
			      "channels mixed up");
			if (have_prev) {
				d = abs(out[2 * i] - prev);
				if (d > maxjump)
					maxjump = d;
			}
			prev = out[2 * i];
			have_prev = 1;
		}
		total += got;
		expect += n * 48000.0 / 32768.0;
	}
	/* A 1 kHz sine at 48 kHz moves at most 20000*2*pi/48 ~ 2618 per frame. */
	CHECK(maxjump < 2700, "discontinuity in resampled sine: %d", maxjump);
	CHECK(fabs(total - expect) < 2, "resampler produced %zu frames, expected %.1f",
	      total, expect);

	/* Ratio changes keep the phase: no jump when switching. */
	rs_init(&r, 0.5);
	for (int i = 0; i < 100; i++)
		in[2 * i] = in[2 * i + 1] = (int16_t)(i * 100);
	total = rs_process(&r, in, 50, out, 1200);
	rs_set_ratio(&r, 0.7);
	total += rs_process(&r, in + 100, 50, out + 2 * total, 1200 - total);
	for (size_t i = 1; i < total; i++)
		CHECK(out[2 * i] >= out[2 * i - 2], "ramp not monotonic at %zu", i);
}

static void test_drc(double drift)
{
	static int16_t silence[1200 * 2];
	struct audio_stats st;
	double acc = 0;
	unsigned xruns = 0;
	int fmin = 1 << 30, fmax = 0;

	settings.audio = AUDIO_ADAPTIVE;
	audio_init_null(drift);
	audio_set_source(32768, 59.7275, 60.0186);
	for (int f = 0; f < 60 * 120; f++) {
		int n;

		acc += 32768 / 59.7275;
		n = (int)acc;
		acc -= n;
		audio_push(silence, n);
		audio_flush();
		if (f % 600 == 599) {
			audio_take_stats(&st);
			if (f > 1800) {
				xruns += st.xruns;
				if (st.fill_min < fmin)
					fmin = st.fill_min;
				if (st.fill_max > fmax)
					fmax = st.fill_max;
			}
		}
	}
	log_msg("selftest: drc drift %+.3f%%: device %u Hz, buffer %u, adj %.4f, "
		"fill %d..%d of target %u, underruns %u", drift * 100,
		audio_rate(), audio_buffer(), st.adj, fmin, fmax, audio_target(),
		xruns);
	CHECK(xruns == 0, "underruns with drift %f", drift);
	CHECK(fmin > (int)audio_target() / 4, "fill too low with drift %f", drift);
	CHECK(fabs(st.adj - 1) < 0.005, "drc saturated with drift %f", drift);
	CHECK(fmax + audio_rate() / 60.0186 <= audio_buffer() + 1,
	      "buffer overflow with drift %f", drift);
	CHECK(st.overflows == 0, "overflow with drift %f", drift);
	CHECK(audio_rate() == 48000 && audio_buffer() == 4096,
	      "config %u Hz buffer %u", audio_rate(), audio_buffer());
	CHECK(fabs(audio_session_rate() / (48000 * (1 + drift)) - 1) < 0.003,
	      "measured rate %f with drift %f", audio_session_rate(), drift);
}

/* Color tables: identity when off, grid scaling, sane color models. */
static void test_fx_tables(void)
{
	static uint16_t lit[FX_LUT_SIZE], dark[FX_LUT_SIZE];
	int64_t t0;
	int bad = 0;

	fx_build(lit, NULL, COLOR_OFF, 0, 0);
	for (int v = 0; v < 65536; v++)
		if (!(v & 0x20) && lit[FX_INDEX(v)] != v)
			bad++;
	CHECK(!bad, "color off is not the identity for %d colors", bad);

	fx_build(lit, dark, COLOR_OFF, 50, 0);
	CHECK(lit[FX_INDEX(0xffdf)] == 0xffdf, "grid lit white %04x",
	      lit[FX_INDEX(0xffdf)]);
	CHECK(dark[FX_INDEX(0xffdf)] == (16 << 11 | 31 << 5 | 16),
	      "grid dark white %04x", dark[FX_INDEX(0xffdf)]);
	CHECK(dark[0] == 0 && lit[0] == 0, "grid black");
	fx_build(lit, dark, COLOR_OFF, 75, 30);
	CHECK(lit[FX_INDEX(0xffdf)] == 0xffff, "boost clamps to white: %04x",
	      lit[FX_INDEX(0xffdf)]);
	CHECK(lit[FX_INDEX(10 << 11)] == 13 << 11, "boost 30%% of red 10: %04x",
	      lit[FX_INDEX(10 << 11)]);
	CHECK(dark[FX_INDEX(0xffdf)] == (8 << 11 | 16 << 5 | 8),
	      "dim 75%% of white: %04x", dark[FX_INDEX(0xffdf)]);

	for (int mode = COLOR_GBA; mode < COLOR_MODES; mode++) {
		int prev = -1, mono = 1;

		bad = 0;
		t0 = now_ns();
		fx_build(lit, NULL, mode, 0, 0);
		log_msg("selftest: color %s table %.1f ms, white %04x, red %04x, "
			"mid gray %04x", color_mode_names[mode],
			(now_ns() - t0) / 1e6, lit[0x7fff], lit[0x7c00], lit[0x3def]);
		CHECK(lit[0] == 0, "%s: black is %04x", color_mode_names[mode],
		      lit[0]);
		/* Every model maps gray to gray (rows sum to 1). */
		for (int g = 0; g < 32; g++) {
			uint16_t v = lit[g << 10 | g << 5 | g];
			int r = v >> 11, gg = (v >> 5) & 63, b = v & 31;

			if (abs(r - b) > 1 || abs(gg - 2 * r) > 2)
				bad++;
			if (r < prev)
				mono = 0;
			prev = r;
		}
		CHECK(!bad, "%s: grays are tinted", color_mode_names[mode]);
		CHECK(mono, "%s: gray ramp not monotonic", color_mode_names[mode]);
		/* Saturated red loses saturation. */
		CHECK((lit[0x7c00] & 0x7e0) && (lit[0x7c00] & 0x1f),
		      "%s: pure red stays pure", color_mode_names[mode]);
	}
	fx_build(lit, NULL, COLOR_GBA, 0, 0);
	{
		uint16_t gba = lit[0x3def];

		fx_build(lit, NULL, COLOR_GBA_LIGHT, 0, 0);
		CHECK((gba >> 11) < (lit[0x3def] >> 11),
		      "GBA is not darker than GBA light");
	}
}

/*
 * The grid as the player sees it: a white frame with the grid on must
 * give, in logical (rotated) coordinates, a lit pixel at the top-left of
 * every 2x2 cell and dark ones elsewhere.
 */
static void test_grid_pattern(void)
{
	static uint16_t lit[FX_LUT_SIZE], dark[FX_LUT_SIZE];
	static uint16_t frame[256 * 160], native[320 * 480];
	const struct blit_fx fx = { lit, dark, 0, 0 };
	uint16_t w, d;
	int bad = 0;
	int64_t t0;

	for (int i = 0; i < 256 * 160; i++)
		frame[i] = 0xffdf;
	fx_build(lit, dark, COLOR_OFF, 50, 0);
	w = lit[FX_INDEX(0xffdf)];
	d = dark[FX_INDEX(0xffdf)];
	blit_rotate(native, 320, 320, 480, 270, frame, 256, 240, 160, 2, &fx);
	/* Rotation 270: logical (lx, ly) is native (ly, 479 - lx). */
	for (int lx = 0; lx < 480; lx++)
		for (int ly = 0; ly < 320; ly++) {
			uint16_t want = lx % 2 == 0 && ly % 2 == 0 ? w : d;

			if (native[(479 - lx) * 320 + ly] != want)
				bad++;
		}
	CHECK(!bad, "grid pattern: %d pixels wrong", bad);
	CHECK(native[479 * 320 + 0] == w, "grid: user's top-left is not lit");
	CHECK(native[478 * 320 + 0] == d && native[479 * 320 + 1] == d &&
	      native[478 * 320 + 1] == d, "grid: rest of the first cell");

	/* The canvas version (in-game settings preview) matches. */
	{
		static uint16_t px[480 * 320];
		struct canvas cv = { px, 480, 320 };

		bad = 0;
		draw_image_fx(&cv, 0, 0, frame, 256, 240, 160, &fx);
		for (int ly = 0; ly < 320; ly++)
			for (int lx = 0; lx < 480; lx++)
				if (px[ly * 480 + lx] != native[(479 - lx) * 320 + ly])
					bad++;
		CHECK(!bad, "draw_image_fx differs from blit in %d pixels", bad);
	}

	for (int i = 0; i < 256 * 160; i++)
		frame[i] = rnd16() & ~0x20;
	fx_build(lit, dark, COLOR_GBA, 50, 15);
	t0 = now_ns();
	for (int i = 0; i < 50; i++)
		blit_rotate(native, 320, 320, 480, 270, frame, 256, 240, 160, 2, &fx);
	log_msg("selftest: GBA blit with color + grid %.3f ms per frame "
		"(host or emulator time, random colors)",
		(now_ns() - t0) / 50 / 1e6);
}

static void write_text(const char *path, const char *text)
{
	CHECK(write_file_atomic(path, text, strlen(text)) == 0, "write %s", path);
}

static void test_settings(const char *base)
{
	struct settings s = settings_defaults, t = settings_defaults;
	struct rom_settings r;
	char buf[512], path[400];
	struct settings keep = settings;

	/* Round trip, every field changed. */
	s.volume = 100;
	s.brightness = 70;
	s.grid = 1;
	s.grid_dim = 75;
	s.grid_boost = 30;
	s.cpu = 1008;
	s.audio = AUDIO_ADAPTIVE;
	s.debug = 1;
	s.resume = 0;
	s.video_scale = VIDEO_CROP;
	CHECK(settings_format(&s, buf, sizeof(buf)) > 0, "format");
	CHECK(settings_parse(&t, buf) == 10, "parse back: %s", buf);
	CHECK(strstr(buf, "video_scale=crop\n") != NULL, "video_scale: %s", buf);
	CHECK(settings_defaults.video_scale == VIDEO_FIT, "videos fit by default");
	CHECK(strstr(buf, "resume=off\n") != NULL, "resume: %s", buf);
	CHECK(settings_defaults.resume == 1, "resume on boot on by default");
	CHECK(strstr(buf, "debug=on\n") != NULL, "debug: %s", buf);
	CHECK(settings_defaults.debug == 0, "debug log off by default");
	CHECK(settings_defaults.audio == AUDIO_ADAPTIVE, "audio adaptive by default");
	{
		struct settings u = settings_defaults;

		CHECK(settings_parse(&u, "audio=simple\n") == 1 &&
		      u.audio == AUDIO_SIMPLE, "audio=simple still selectable");
	}
	CHECK(strstr(buf, "audio=adaptive\n") != NULL, "audio mode: %s", buf);
	CHECK(!memcmp(&s, &t, sizeof(s)), "round trip: %s", buf);
	CHECK(strstr(buf, "grid=on\n") != NULL, "grid written as on/off");
	CHECK(strstr(buf, "cpu=1008\n") != NULL, "cpu written as MHz: %s", buf);
	s.cpu = CPU_MODE_MAX;
	settings_format(&s, buf, sizeof(buf));
	CHECK(strstr(buf, "cpu=max\n") != NULL, "cpu written as max: %s", buf);
	t = settings_defaults;
	CHECK(t.cpu == CPU_MODE_ADAPTIVE, "cpu default adaptive");
	CHECK(settings_parse(&t, "cpu=MAX\n") == 1 && t.cpu == CPU_MODE_MAX,
	      "cpu=max");
	CHECK(settings_parse(&t, "cpu=816\n") == 1 && t.cpu == 816, "cpu=816");
	CHECK(settings_parse(&t, "cpu=adaptive\n") == 1 &&
	      t.cpu == CPU_MODE_ADAPTIVE, "cpu=adaptive");
	CHECK(settings_parse(&t, "cpu=1\ncpu=fast\ncpu=99999\n") == 0 &&
	      t.cpu == CPU_MODE_ADAPTIVE, "cpu range");

	/* Old format, CRLF, spaces, comments, junk: good values only. */
	t = settings_defaults;
	CHECK(settings_parse(&t, "volume=200\r\nbrightness=70\nbogus=1\n") == 2,
	      "settings parse");
	CHECK(t.volume == 200 && t.brightness == 70, "settings values");
	settings_parse(&t, "volume=999\nbrightness=1\ngrid=maybe\n"
		       "grid_dim=50x\n#grid_boost=30\nnoequals\n=5\n"
		       "  grid = ON \r\ngrid_boost=\n");
	CHECK(t.volume == 200 && t.brightness == 70 && t.grid == 1 &&
	      t.grid_dim == settings_defaults.grid_dim &&
	      t.grid_boost == settings_defaults.grid_boost,
	      "settings range check");
	t = settings_defaults;
	CHECK(settings_parse(&t, "\x01\xff\n\n===\nvolume") == 0 &&
	      !memcmp(&t, &settings_defaults, sizeof(t)), "garbage");

	rom_settings_defaults("x.gba", &r);
	CHECK(r.color == COLOR_AUTO, "rom default auto");
	CHECK(rom_settings_parse(&r, "color=Auto\n") == 1 && r.color == COLOR_AUTO,
	      "color=auto parse");
	CHECK(rom_settings_parse(&r, "color=GBA-light\r\n") == 1 &&
	      r.color == COLOR_GBA_LIGHT, "rom parse");
	CHECK(rom_settings_parse(&r, "color=7\ncolor=sepia\n") == 0 &&
	      r.color == COLOR_GBA_LIGHT, "rom parse range");
	CHECK(r.core == CORE_GPSP, "core default gpsp");
	CHECK(rom_settings_parse(&r, "core=gpSP\n") == 1 && r.core == CORE_GPSP,
	      "core parse");
	CHECK(rom_settings_parse(&r, "core=vba\ncore=2\n") == 0 &&
	      r.core == CORE_GPSP, "core parse range");
	CHECK(r.blend == 0, "blend default off");
	CHECK(rom_settings_parse(&r, "blend = On\r\n") == 1 && r.blend == 1,
	      "blend parse");
	CHECK(rom_settings_parse(&r, "blend=2\nblend=half\n") == 0 &&
	      r.blend == 1, "blend parse range");
	CHECK(rom_settings_parse(&r, "blend=no\n") == 1 && r.blend == 0,
	      "blend parse no");
	CHECK(r.frameskip == FRAMESKIP_OFF, "frameskip default off");
	CHECK(rom_settings_parse(&r, "frameskip = Auto\r\n") == 1 &&
	      r.frameskip == FRAMESKIP_AUTO, "frameskip parse auto");
	CHECK(rom_settings_parse(&r, "frameskip=1\n") == 1 &&
	      r.frameskip == FRAMESKIP_1, "frameskip parse 1");
	CHECK(rom_settings_parse(&r, "frameskip=3\n") == 1 &&
	      r.frameskip == FRAMESKIP_3, "frameskip parse 3");
	CHECK(rom_settings_parse(&r, "frameskip=4\nframeskip=0\n"
				 "frameskip=fast\n") == 0 &&
	      r.frameskip == FRAMESKIP_3, "frameskip parse range");
	CHECK(rom_settings_parse(&r, "frameskip=off\n") == 1 &&
	      r.frameskip == FRAMESKIP_OFF, "frameskip parse off");

	/* Files: missing, config/, per ROM. */
	sys_make_dirs();
	settings_load();
	CHECK(!memcmp(&settings, &settings_defaults, sizeof(settings)),
	      "no file gives defaults");
	settings.volume = 40;
	settings.grid = 1;
	settings.grid_dim = 25;
	settings_save();
	settings = settings_defaults;
	settings_load();
	CHECK(settings.volume == 40 && settings.grid == 1 &&
	      settings.grid_dim == 25, "config/settings.txt round trip");
	snprintf(path, sizeof(path), "%s/config/settings.txt", base);
	write_text(path, "\xde\xad\nvolume=12");
	settings_load();
	CHECK(settings.volume == 12 && settings.brightness == 50,
	      "corrupt config file: defaults plus what parses");
	write_text(path, "brightness=10\n");
	settings_load();
	CHECK(settings.brightness == BRIGHTNESS_MIN,
	      "a saved brightness below the minimum is raised");

	rom_settings_load("Some Game (USA).gba");
	CHECK(rom_settings.color == COLOR_AUTO, "rom without file");
	CHECK(rom_settings_color() == COLOR_OFF, "auto before resolving: off");
	rom_settings_set_auto(COLOR_GBA, "test");
	CHECK(rom_settings_color() == COLOR_GBA, "auto resolved");
	rom_color_label(buf, sizeof(buf));
	CHECK(!strcmp(buf, "Auto (GBA)"), "auto label: %s", buf);
	rom_settings.color = COLOR_SP101;
	rom_settings_save();
	snprintf(path, sizeof(path), "%s/config/roms/Some Game (USA).txt", base);
	CHECK(access(path, F_OK) == 0, "per ROM file name");
	rom_settings_load(NULL);
	CHECK(rom_settings.color == COLOR_AUTO && rom_settings_color() == COLOR_OFF,
	      "no game");
	rom_settings_load("Some Game (USA).gba");
	CHECK(rom_settings.color == COLOR_SP101, "per ROM round trip");
	rom_settings_set_auto(COLOR_GBA, "test");
	CHECK(rom_settings_color() == COLOR_SP101, "explicit mode beats auto");
	rom_color_label(buf, sizeof(buf));
	CHECK(!strcmp(buf, "SP101"), "explicit label: %s", buf);
	rom_settings_load("Other.gba");
	CHECK(rom_settings.color == COLOR_AUTO, "other ROM unaffected");
	/* A learned cpu_start alone does not pin the color default. */
	rom_settings.cpu_start = 816;
	rom_settings_save();
	snprintf(path, sizeof(path), "%s/config/roms/Other.txt", base);
	CHECK(read_file(path, buf, sizeof(buf) - 1) > 0 &&
	      !strncmp(buf, "cpu_start=816\n", 14) && !strstr(buf, "color"),
	      "learned cpu_start only: %s", buf);
	rom_settings_load("Other.gba");
	CHECK(rom_settings.cpu_start == 816, "cpu_start round trip");
	rom_settings.color = COLOR_GBA;
	rom_settings_save();
	rom_settings_load("Other.gba");
	CHECK(rom_settings.cpu_start == 816 && rom_settings.color == COLOR_GBA,
	      "cpu_start and color");
	rom_settings.color = COLOR_OFF;
	rom_settings_save();
	memset(buf, 0, sizeof(buf));
	read_file(path, buf, sizeof(buf) - 1);
	CHECK(strstr(buf, "color=off") != NULL, "color in the file stays: %s",
	      buf);
	/* An explicit off from before auto existed stays off. */
	rom_settings_load("Other.gba");
	rom_settings_set_auto(COLOR_GBA, "test");
	CHECK(rom_settings.color == COLOR_OFF && rom_settings_color() == COLOR_OFF,
	      "old color=off kept");
	/* Back to auto: written as auto since the key is in the file. */
	rom_settings.color = COLOR_AUTO;
	rom_settings_save();
	memset(buf, 0, sizeof(buf));
	read_file(path, buf, sizeof(buf) - 1);
	CHECK(strstr(buf, "color=auto\n") != NULL, "color=auto written: %s", buf);
	rom_settings.color = COLOR_OFF;
	rom_settings_save();
	CHECK(!strstr(buf, "core="), "default core not written: %s", buf);
	rom_settings.core = CORE_MGBA;
	rom_settings_save();
	rom_settings_load("Other.gba");
	CHECK(rom_settings.core == CORE_MGBA, "core round trip");
	memset(buf, 0, sizeof(buf));
	read_file(path, buf, sizeof(buf) - 1);
	CHECK(strstr(buf, "core=mgba\n") != NULL, "core in the file: %s", buf);
	/* A file from before (learned cpu_start, no core) gets the new
	 * default; an explicit choice in the file stays. */
	write_text(path, "cpu_start=816\n");
	rom_settings_load("Other.gba");
	CHECK(rom_settings.core == CORE_GPSP, "old file without core: gpsp");
	rom_settings.cpu_start = 1008;
	rom_settings_save();
	memset(buf, 0, sizeof(buf));
	read_file(path, buf, sizeof(buf) - 1);
	CHECK(!strstr(buf, "core="), "default core still not written: %s", buf);
	write_text(path, "core=gpsp\n");
	rom_settings_load("Other.gba");
	rom_settings.core = CORE_MGBA;
	rom_settings_save();
	rom_settings_load("Other.gba");
	CHECK(rom_settings.core == CORE_MGBA, "explicit mgba after gpsp");
	memset(buf, 0, sizeof(buf));
	read_file(path, buf, sizeof(buf) - 1);
	CHECK(!strstr(buf, "blend="), "default blend not written: %s", buf);
	/* Blend: written once on, kept in the file when back off. */
	rom_settings.blend = 1;
	rom_settings_save();
	rom_settings_load("Other.gba");
	CHECK(rom_settings.blend == 1 && rom_settings.core == CORE_MGBA,
	      "blend round trip");
	rom_settings.blend = 0;
	rom_settings_save();
	memset(buf, 0, sizeof(buf));
	read_file(path, buf, sizeof(buf) - 1);
	CHECK(strstr(buf, "blend=off\n") != NULL, "blend in the file stays: %s",
	      buf);
	CHECK(!strstr(buf, "frameskip="), "default frameskip not written: %s",
	      buf);
	for (int v = FRAMESKIP_AUTO; v < FRAMESKIP_COUNT; v++) {
		char want[32];

		rom_settings.frameskip = v;
		rom_settings_save();
		rom_settings_load("Other.gba");
		memset(buf, 0, sizeof(buf));
		read_file(path, buf, sizeof(buf) - 1);
		snprintf(want, sizeof(want), "frameskip=%s\n", frameskip_names[v]);
		CHECK(rom_settings.frameskip == v && strstr(buf, want),
		      "frameskip %s round trip: %s", frameskip_names[v], buf);
	}
	rom_settings.frameskip = FRAMESKIP_OFF;
	rom_settings_save();
	rom_settings_load(NULL);
	CHECK(rom_settings.core == CORE_GPSP, "no game: default core");
	CHECK(rom_settings.blend == 0, "no game: blend off");
	CHECK(rom_settings.frameskip == FRAMESKIP_OFF, "no game: frameskip off");
	settings = keep;
	log_set_debug(1);
}

static void put(const char *dir, const char *name, const char *val)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	write_text(path, val);
}

/* A fake regmap debugfs registers file, "rr: vv" for 0..0xfe without
 * the registers from skip_from to skip_to (as regmap leaves out the
 * unreadable ones), RK817 charger values in 0xe4..0xf0. */
static void fake_regs(const char *dir, int skip_from, int skip_to, int sts,
		      int sys)
{
	char buf[256 * 8], path[400];
	size_t n = 0;
	FILE *f;

	mkdir(dir, 0755);
	put(dir, "name", "rk8xx-i2c\n");
	for (int r = 0; r <= 0xfe; r++) {
		int v = r == 0xe4 ? 0x22 : r == 0xe5 ? 0xdb : r == 0xe6 ? 0x44 :
			r == 0xeb ? sts : r == 0xf0 ? sys : r & 0x0f;

		if (r >= skip_from && r <= skip_to)
			continue;
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%02x: %02x\n", r,
				      v);
	}
	/* In place: battery.c keeps the file open, as on debugfs. */
	snprintf(path, sizeof(path), "%s/registers", dir);
	f = fopen(path, "w");
	CHECK(f && fwrite(buf, 1, n, f) == n && !fclose(f), "write %s", path);
}

static void fake_irqs(const char *path, int in, int out)
{
	char buf[512];

	snprintf(buf, sizeof(buf),
		 "           CPU0       CPU1       CPU2       CPU3\n"
		 " 11:      12345      23456      34567      45678     GICv2  30 "
		 "Level     arch_timer\n"
		 " 76:          %d          0          %d          0     rk817   8 "
		 "Edge      rk817_plug_in\n"
		 " 77:          0          %d          0          0     rk817   9 "
		 "Edge      rk817_plug_out\n"
		 " 78:          9          0          0          0     rk817  10 "
		 "Edge      rk817_plug_in_extra\n"
		 "IPI0:       100        200        300        400  Rescheduling "
		 "interrupts\n", in - in / 2, in / 2, out);
	write_text(path, buf);
}

static char fake_irq_path[300], fake_regmap[300];

/* /proc/interrupts and the regmap debugfs for battery.c, with a decoy
 * regmap in front. */
static void charger_fakes(const char *base)
{
	char dir[400];

	snprintf(fake_irq_path, sizeof(fake_irq_path), "%s/interrupts", base);
	snprintf(fake_regmap, sizeof(fake_regmap), "%s/regmap", base);
	mkdir(fake_regmap, 0755);
	snprintf(dir, sizeof(dir), "%s/dummy0", fake_regmap);
	mkdir(dir, 0755);
	put(dir, "name", "nodev\n");
	snprintf(dir, sizeof(dir), "%s/0-0020", fake_regmap);
	fake_regs(dir, -1, -1, 0x80, 0x00);
	fake_irqs(fake_irq_path, 0, 1);
	battery_diag_paths(fake_irq_path, fake_regmap);
}

/*
 * The charger diagnostics: a plug-in the driver misses (online stays 0,
 * the PMIC sees the cable, CHRG_OFF), the 30 one-second samples after
 * it, a change in the middle of them, the registers file with lines
 * missing, and the full lines at sleep.
 */
static void test_charger_diag(const char *base, const char *ps)
{
	static char log[65536];
	char logp[300], bat[350], chg[350], dir[400];
	int64_t t = 6000000000000000LL, next;
	int samples = 0, n;
	struct stat sb;
	long start;

	snprintf(bat, sizeof(bat), "%s/rk817-battery", ps);
	snprintf(chg, sizeof(chg), "%s/rk817-charger", ps);
	snprintf(logp, sizeof(logp), "%s/battery-log.txt", base);
	log_open_file(logp);
	start = stat(logp, &sb) ? 0 : (long)sb.st_size;
	snprintf(dir, sizeof(dir), "%s/0-0020", fake_regmap);
	put(bat, "capacity", "65\n");
	put(bat, "status", "Discharging\n");
	put(bat, "charge_type", "N/A\n");
	put(bat, "current_avg", "-300000\n");
	put(bat, "charge_now", "2229980\n");
	put(bat, "charge_full", "3450000\n");
	put(bat, "constant_charge_current_max", "2000000\n");
	put(bat, "constant_charge_voltage_max", "4200000\n");
	put(chg, "online", "0\n");
	battery_diag_paths(fake_irq_path, fake_regmap);
	battery_init(ps);

	CHECK(battery_poll(t) == t + 5000000000LL, "charger: steady: 5 s");
	t += 5000000000LL;
	/* Plugged in: the PMIC sees it, the driver does not. */
	fake_irqs(fake_irq_path, 3, 1);
	fake_regs(dir, -1, -1, 0x80, 0x40);
	put(bat, "status", "Not charging\n");
	put(bat, "current_avg", "148000\n");
	put(chg, "voltage_avg", "4950000\n");
	next = battery_poll(t);
	CHECK(next == t + 1000000000LL, "charger: change: next in %lld ms",
	      (long long)((next - t) / 1000000));
	while (next - t < 5000000000LL && samples < 40) {
		t = next;
		next = battery_poll(t);
		samples++;
	}
	CHECK(samples == 30 && next == t + 5000000000LL,
	      "charger: %d samples, then %lld ms", samples,
	      (long long)((next - t) / 1000000));
	CHECK(battery_poll(next) == next + 5000000000LL, "charger: steady again");
	t = next;

	/* A change in the middle of the samples starts them over. */
	put(chg, "online", "1\n");
	next = battery_poll(t += 5000000000LL);
	for (int i = 0; i < 10; i++)
		next = battery_poll(t = next);
	fake_regs(dir, -1, -1, 0xb0, 0x40);
	put(bat, "status", "Charging\n");
	put(bat, "charge_type", "Standard\n");
	next = battery_poll(t = next);
	samples = 0;
	while (next - t < 5000000000LL && samples < 40) {
		next = battery_poll(t = next);
		samples++;
	}
	CHECK(samples == 30, "charger: restarted, %d samples", samples);

	battery_charger_log("sleep #9 before");

	/* Registers 0x10..0x1f not listed: e4 is found by reading. */
	snprintf(dir, sizeof(dir), "%s/regmap2", base);
	mkdir(dir, 0755);
	snprintf(logp, sizeof(logp), "%s/regmap2/1-0020", base);
	fake_regs(logp, 0x10, 0x1f, 0x90, 0x40);
	battery_diag_paths(fake_irq_path, dir);
	battery_charger_log("located");
	/* None at all. */
	snprintf(dir, sizeof(dir), "%s/no-regmap", base);
	battery_diag_paths(fake_irq_path, dir);
	battery_charger_log("no regmap");
	battery_diag_paths(fake_irq_path, fake_regmap);

	snprintf(logp, sizeof(logp), "%s/battery-log.txt", base);
	n = read_file(logp, log, sizeof(log) - 1);
	log[n > 0 ? n : 0] = 0;
	CHECK(n > start, "charger: log %d bytes", n);
	memmove(log, log + (n > start ? start : 0),
		(size_t)(n > start ? n - start + 1 : 1));
	CHECK(strstr(log, "/0-0020/registers, 7 bytes per line, 0xe4 at offset "
		     "1596") != NULL, "charger: regmap found");
	CHECK(strstr(log, "charger: start: online 0, Discharging, charge_type "
		     "N/A, 65% ") && strstr(log, "IRQs plug_in 0 plug_out 1\n"),
	      "charger: start line");
	CHECK(strstr(log, "charger: start: regs e4=0x22 e5=0xdb e6=0x44 eb=0x80 "
		     "f0=0x00 (PLUG_IN_STS 0, CHRG_STS CHRG_OFF, BAT_EXS 1, "
		     "CHRG_EN 1, in: VLIM on 4.5 V ILIM on 1.50 A, out: 4.20 V "
		     "2.00 A), read in ") != NULL, "charger: start regs");
	CHECK(strstr(log, "charger: change (status Discharging->Not charging, "
		     "IRQs plug_in 0->3 plug_out 1->1, PLUG_IN_STS 0->1): "
		     "online 0, Not charging, charge_type N/A, 65% 3.912 V "
		     "+0.148 A, input 4.950 V, charge 2229 of 3450 mAh, max "
		     "2.000 A 4.200 V; IRQs plug_in 3 plug_out 1\n") != NULL,
	      "charger: change line");
	CHECK(strstr(log, "charger: +1 s: online 0, Not charging, N/A, 65% "
		     "3.912 V +0.148 A, input 4.950 V; IRQs 3/1; e5=0xdb "
		     "eb=0x80 f0=0x40 (PLUG_IN_STS 1, CHRG_OFF); read in ") &&
	      strstr(log, "charger: +30 s: ") && !strstr(log, "charger: +31 s"),
	      "charger: sample lines");
	CHECK(strstr(log, "charger: samples done, 30 in 30 s, read time avg ") !=
	      NULL, "charger: samples done");
	CHECK(strstr(log, "charger: change (online 0->1): ") &&
	      strstr(log, "charger: change (status Not charging->Charging, "
		     "charge_type N/A->Standard, CHRG_STS "
		     "CHRG_OFF->CC_OR_CV_CHRG): online 1, Charging, "),
	      "charger: change in the samples");
	CHECK(strstr(log, "charger: sleep #9 before: online 1, Charging, ") &&
	      strstr(log, "charger: sleep #9 before: regs e4=0x22 "),
	      "charger: sleep lines");
	CHECK(strstr(log, "/1-0020/registers, 7 bytes per line, 0xe4 at offset "
		     "1484") &&
	      strstr(log, "charger: located: regs e4=0x22 e5=0xdb e6=0x44 "
		     "eb=0x90 f0=0x40 (PLUG_IN_STS 1, CHRG_STS DEAD_CHRG"),
	      "charger: registers located by reading");
	CHECK(strstr(log, "/no-regmap (No such file or directory), PMIC "
		     "registers not logged") &&
	      strstr(log, "charger: no regmap: online 1, ") &&
	      !strstr(log, "charger: no regmap: regs"),
	      "charger: no regmap");
	put(bat, "capacity", "64\n");
	put(bat, "status", "Discharging\n");
	put(bat, "current_avg", "-412000\n");
	put(chg, "online", "0\n");
}

/* A fake /sys/class/power_supply as the RK817 driver lays it out. */
static void test_battery(const char *base)
{
	char ps[300], bat[350], chg[350], label[16], line[128];
	struct battery b;

	snprintf(ps, sizeof(ps), "%s/power_supply", base);
	snprintf(bat, sizeof(bat), "%s/rk817-battery", ps);
	snprintf(chg, sizeof(chg), "%s/rk817-charger", ps);
	mkdir(ps, 0755);
	mkdir(bat, 0755);
	mkdir(chg, 0755);
	put(bat, "type", "Battery\n");
	put(bat, "capacity", "87\n");
	put(bat, "status", "Discharging\n");
	put(bat, "voltage_avg", "3912000\n");
	put(bat, "current_avg", "-412000\n");
	put(bat, "uevent", "POWER_SUPPLY_NAME=rk817-battery\n"
	    "POWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_CAPACITY=87\n");
	put(chg, "type", "USB\n");
	put(chg, "online", "0\n");
	put(chg, "voltage_avg", "0\n");
	charger_fakes(base);

	battery_init(ps);
	CHECK(battery_read(&b) == 0, "battery read");
	CHECK(b.capacity == 87 && b.status == BAT_DISCHARGING &&
	      b.uv == 3912000 && b.ua == -412000 && !b.online,
	      "battery values %d %d %ld %ld %d", b.capacity, b.status, b.uv,
	      b.ua, b.online);
	battery_label(&b, label, sizeof(label));
	CHECK(!strcmp(label, "87%"), "label %s", label);
	battery_format(&b, line, sizeof(line));
	log_msg("selftest: battery %s", line);
	CHECK(!strcmp(line, "87% Discharging 3.912 V -0.412 A 1612 mW, charger "
		      "offline"), "battery line %s", line);

	put(bat, "capacity", "5\n");
	battery_poll(1000000000000000LL);
	CHECK(battery_low(), "5%% discharging is low");
	put(bat, "status", "Charging\n");
	put(chg, "online", "1\n");
	battery_poll(2000000000000000LL);
	CHECK(!battery_low(), "charging is not low");
	battery_label(battery_get(), label, sizeof(label));
	CHECK(!strcmp(label, "+5%"), "charging label %s", label);
	put(bat, "capacity", "100\n");
	put(bat, "status", "Full\n");
	battery_poll(3000000000000000LL);
	CHECK(battery_get()->status == BAT_FULL, "full");
	put(bat, "capacity", "64\n");
	put(bat, "status", "Discharging\n");
	put(chg, "online", "0\n");
	battery_poll(4000000000000000LL);

	test_charger_diag(base, ps);

	battery_init("/nonexistent/power_supply");
	CHECK(battery_read(&b) != 0 && !b.valid, "no battery");
	battery_init(ps);  /* back, for the UI renders */
	battery_poll(7000000000000000LL);
}

/* The simple path: fixed ratio, blocking writes, half a buffer. */
static void test_audio_simple(double drift, int seconds)
{
	static int16_t silence[1200 * 2];
	struct audio_stats st;
	double acc = 0;
	unsigned xruns = 0;
	char csv[200];

	settings.audio = AUDIO_SIMPLE;
	audio_init_null(drift);
	audio_set_source(32768, 59.7275, 60.0186);
	for (int f = 0; f < 60 * seconds; f++) {
		int n;

		acc += 32768 / 59.7275;
		n = (int)acc;
		acc -= n;
		audio_push(silence, n);
		audio_flush();
		if (f % 600 == 599) {
			audio_take_stats(&st);
			xruns += st.xruns;
		}
	}
	audio_summary("selftest");
	audio_csv_fields(csv, sizeof(csv));
	log_msg("selftest: simple drift %+.4f%% %d s: underruns %u, measured %.0f "
		"Hz, csv %s", drift * 100, seconds, xruns, audio_session_rate(), csv);
	CHECK(xruns == 0, "simple mode underruns with drift %f", drift);
	CHECK(audio_rate() == 48000 && audio_buffer() == 4096,
	      "simple mode config");
	CHECK(fabs(audio_session_rate() / (48000 * (1 + drift)) - 1) < 0.003,
	      "simple mode measured rate %f", audio_session_rate());
	{
		int commas = 0;

		for (const char *p = csv; *p; p++)
			commas += *p == ',';
		CHECK(!strncmp(csv, "simple,48000 Hz S16 512x8,", 26) &&
		      commas == 4, "csv fields %s", csv);
	}
	settings.audio = settings_defaults.audio;
}

static void touch(const char *dir, const char *name)
{
	char path[400];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "w");
	if (f)
		fclose(f);
}

static void test_romlist(const char *base)
{
	static const char *const files[] = {
		"b.gba", "A.GBA", "._a.gba", "c.txt", ".hidden.gba", "z.Gba", "gba",
	};
	/* Folders, nested; hidden ones skipped, empty ones listed. */
	static const char *const dirs[] = {
		"Zelda", "action", "action/inner", ".hidden", "empty",
	};
	char dir[300], sub[340];
	struct romlist rl = { 0 };

	snprintf(dir, sizeof(dir), "%s/roms", base);
	mkdir(dir, 0755);
	for (size_t i = 0; i < ARRAY_SIZE(files); i++)
		touch(dir, files[i]);
	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		snprintf(sub, sizeof(sub), "%s/%s", dir, dirs[i]);
		mkdir(sub, 0755);
	}
	snprintf(sub, sizeof(sub), "%s/action", dir);
	touch(sub, "x.gba");
	snprintf(sub, sizeof(sub), "%s/action/inner", dir);
	touch(sub, "deep.gba");
	CHECK(romlist_scan(&rl, dir) == 6 && rl.dirs == 3, "romlist found %d "
	      "(%d folders)", rl.count, rl.dirs);
	if (rl.count == 6) {
		CHECK(!strcmp(rl.roms[0].file, "action") && rl.roms[0].dir &&
		      !strcmp(rl.roms[0].id.title, "action/"), "order 0: %s",
		      rl.roms[0].file);
		CHECK(!strcmp(rl.roms[1].file, "empty"), "order 1: %s", rl.roms[1].file);
		CHECK(!strcmp(rl.roms[2].file, "Zelda"), "order 2: %s", rl.roms[2].file);
		CHECK(!strcmp(rl.roms[3].file, "A.GBA") && !rl.roms[3].dir,
		      "order 3: %s", rl.roms[3].file);
		CHECK(!strcmp(rl.roms[4].file, "b.gba"), "order 4: %s", rl.roms[4].file);
		CHECK(!strcmp(rl.roms[5].file, "z.Gba"), "order 5: %s", rl.roms[5].file);
		CHECK(!strcmp(rl.roms[3].id.title, "A") &&
		      rl.roms[3].id.how == ROMDB_UNKNOWN, "empty file: %s",
		      rl.roms[3].id.title);
	}
	snprintf(sub, sizeof(sub), "%s/action", dir);
	CHECK(romlist_scan(&rl, sub) == 2 && rl.dirs == 1 &&
	      !strcmp(rl.roms[0].file, "inner") && !strcmp(rl.roms[1].file, "x.gba"),
	      "romlist in a folder: %d", rl.count);
	snprintf(sub, sizeof(sub), "%s/action/inner", dir);
	CHECK(romlist_scan(&rl, sub) == 1 && !rl.dirs &&
	      !strcmp(rl.roms[0].file, "deep.gba"), "romlist two deep: %d",
	      rl.count);
	snprintf(sub, sizeof(sub), "%s/empty", dir);
	CHECK(romlist_scan(&rl, sub) == 0, "empty folder");
	romlist_free(&rl);
	snprintf(dir, sizeof(dir), "%s/empty-roms", base);
	mkdir(dir, 0755);
	CHECK(romlist_scan(&rl, dir) == 0, "empty dir");
	CHECK(romlist_scan(&rl, "/nonexistent/roms") < 0, "missing dir");
}

/* ------------------------------------------------------------- romdb */

static uint32_t crc_tab[256];

/* Four bytes at p (the end of a buffer of len bytes, CRC so far crc0)
 * that make the CRC of the whole target: the reflected CRC's steps
 * backwards (the top byte of each table entry is unique). */
static void crc_force(uint8_t *p, uint32_t crc0, uint32_t target)
{
	uint32_t s = ~target, st = ~crc0;
	int k[4];

	if (!crc_tab[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;

			for (int j = 0; j < 8; j++)
				c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			crc_tab[i] = c;
		}
	for (int i = 3; i >= 0; i--) {
		for (k[i] = 0; k[i] < 256; k[i]++)
			if (crc_tab[k[i]] >> 24 == s >> 24)
				break;
		s = (s ^ crc_tab[k[i]]) << 8;
	}
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(k[i] ^ (st & 0xff));
		st = crc_tab[k[i]] ^ (st >> 8);
	}
}

static void make_header(uint8_t *rom, const char *title, const char *code,
			int version)
{
	memset(rom, 0, 0xc0);
	memcpy(rom + 0xa0, title, strlen(title) < 12 ? strlen(title) : 12);
	if (code)
		memcpy(rom + 0xac, code, 4);
	memcpy(rom + 0xb0, "01", 2);
	rom[0xb2] = 0x96;
	rom[0xbc] = (uint8_t)version;
}

static const struct romdb_entry *entry_named(const char *name)
{
	for (int i = 0; i < romdb_count; i++)
		if (!strcmp(romdb_name(&romdb_entries[i]), name))
			return &romdb_entries[i];
	return NULL;
}

/* Entries of a code with this version. */
static int code_count(const char *code, int version)
{
	int n = 0;

	for (int i = 0; i < romdb_count; i++)
		n += !strcmp(romdb_entries[i].code, code) &&
		     romdb_entries[i].version == version;
	return n;
}

/* A dummy ROM: header, then bytes so that the file's CRC is crc (if not
 * 0) or not any entry's. */
static void write_rom(const char *path, const char *code, int version,
		      uint32_t crc, uint32_t salt)
{
	static uint8_t rom[4096];

	make_header(rom, "DUMMY", code, version);
	memcpy(rom + 0x100, &salt, 4);
	memset(rom + 0x104, 0, sizeof(rom) - 0x104);
	if (crc)
		crc_force(rom + sizeof(rom) - 4, romdb_crc32(0, rom, sizeof(rom) - 4),
			  crc);
	CHECK(write_file_atomic(path, rom, sizeof(rom)) == 0, "write %s", path);
}

static void test_romdb(const char *base)
{
	static const struct color_exception ex[] = {
		{ "BJB", COLOR_GBA_LIGHT, "test" },
	};
	const struct romdb_entry *e, *vc, *rev;
	struct gba_header h;
	struct rom_id id;
	uint8_t hdr[0xc0];
	uint32_t crc, bad = 0x12345678;
	char why[96], dir[300], path[400];
	struct romlist rl = { 0 };
	int64_t t0, t_scan, t_crc, t_cached;
	int n_code = 0, n_crc = 0, n_mod = 0, n_unk = 0, steps = 0, sorted = 1;

	/* Header */
	make_header(hdr, "POKEMON RUBY", "AXVE", 1);
	CHECK(romdb_parse_header(hdr, sizeof(hdr), &h) == 0, "header");
	CHECK(!strcmp(h.title, "POKEMON RUBY") && !strcmp(h.code, "AXVE") &&
	      !strcmp(h.maker, "01") && h.version == 1, "header fields: %s %s %s %d",
	      h.title, h.code, h.maker, h.version);
	make_header(hdr, "SHORT", "AB c", 0);
	CHECK(romdb_parse_header(hdr, sizeof(hdr), &h) == 0 &&
	      !strcmp(h.title, "SHORT") && !h.code[0], "bad code: %s", h.code);
	hdr[0xb2] = 0;
	CHECK(romdb_parse_header(hdr, sizeof(hdr), &h) < 0 && !h.code[0],
	      "no fixed byte");
	CHECK(romdb_parse_header(hdr, 0x80, &h) < 0, "short header");

	/* CRC32 */
	CHECK(romdb_crc32(0, "123456789", 9) == 0xcbf43926, "crc32 check value");
	crc = romdb_crc32(romdb_crc32(0, "12345", 5), "6789", 4);
	CHECK(crc == 0xcbf43926, "crc32 incremental: %08x", crc);
	CHECK(romdb_crc32(0, "x", 0) == 0, "crc32 empty");

	/* The table: sorted for code_range, names ASCII. */
	CHECK(romdb_count > 3000, "database entries %d", romdb_count);
	for (int i = 1; i < romdb_count; i++)
		if (strcmp(romdb_entries[i - 1].code, romdb_entries[i].code) > 0)
			sorted = 0;
	CHECK(sorted, "database not sorted by code");

	/* By code: a single entry. */
	e = entry_named("007 - Everything or Nothing (Japan)");
	CHECK(e && !strcmp(e->code, "BJBJ") && romdb_year(e) == 2004,
	      "BJBJ entry");
	strcpy(h.code, "BJBJ");
	h.version = 0;
	romdb_match("x.gba", &h, 8 << 20, NULL, &id);
	CHECK(id.how == ROMDB_CODE && !strcmp(id.title,
	      "007 - Everything or Nothing (Japan)"), "by code: %s", id.title);
	CHECK(romdb_auto_color(&id, why, sizeof(why)) == COLOR_GBA,
	      "auto 2004: gba (%s)", why);
	CHECK(strstr(why, "2004") != NULL, "auto reason: %s", why);
	CHECK(romdb_auto_color_with(&id, ex, 1, why, sizeof(why)) ==
	      COLOR_GBA_LIGHT && !strcmp(why, "exception: test"), "exception: %s",
	      why);

	h.version = 2;
	romdb_match("x.gba", &h, 8 << 20, NULL, &id);
	CHECK(id.how == ROMDB_PENDING, "single entry, other version: pending");
	romdb_match("x.gba", &h, 8 << 20, &bad, &id);
	CHECK(id.how == ROMDB_MODIFIED && !strcmp(id.title,
	      "007 - Everything or Nothing (Japan) *"), "other version: %s",
	      id.title);

	/* Revisions: the version picks Rev 1; the original and its Virtual
	 * Console build share code and version and need the CRC. */
	e = entry_named("Advance Wars (USA)");
	rev = entry_named("Advance Wars (USA) (Rev 1)");
	vc = entry_named("Advance Wars (USA) (Virtual Console)");
	CHECK(e && rev && vc && rev->version == 1 && code_count("AWRE", 0) == 2,
	      "AWRE entries");
	if (e && rev && vc) {
		strcpy(h.code, "AWRE");
		h.version = 1;
		romdb_match("aw.gba", &h, rev->size, NULL, &id);
		CHECK(id.how == ROMDB_CODE && id.e == rev, "rev 1 by version: %s",
		      id.title);
		h.version = 0;
		romdb_match("aw.gba", &h, e->size, NULL, &id);
		CHECK(id.how == ROMDB_PENDING && !strcmp(id.title,
		      "Advance Wars (USA)"), "pending: %s", id.title);
		romdb_match("aw.gba", &h, e->size, &vc->crc, &id);
		CHECK(id.how == ROMDB_CRC && id.e == vc, "by crc: %s", id.title);
		romdb_match("aw.gba", &h, e->size, &bad, &id);
		CHECK(id.how == ROMDB_MODIFIED && !strcmp(id.title,
		      "Advance Wars (USA) *"), "modified: %s", id.title);
		/* A version No-Intro does not list needs the CRC. */
		h.version = 7;
		romdb_match("aw.gba", &h, e->size, NULL, &id);
		CHECK(id.how == ROMDB_PENDING, "unlisted version: pending");
		romdb_match("aw.gba", &h, e->size, &bad, &id);
		CHECK(id.how == ROMDB_MODIFIED, "unlisted version: modified");
		h.version = 0;
		/* Rev 1's CRC with a version 0 header is still that ROM. */
		romdb_match("aw.gba", &h, e->size, &rev->crc, &id);
		CHECK(id.how == ROMDB_CRC && id.e == rev, "crc over version");
		CHECK(romdb_auto_color(&id, why, sizeof(why)) == COLOR_GBA,
		      "Advance Wars 2001: %s", why);
	}
	/* Regions sharing the start of a name: the common words. */
	strcpy(h.code, "AAMJ");
	h.version = 0;
	romdb_match("cotm.gba", &h, 8 << 20, NULL, &id);
	CHECK(id.how == ROMDB_PENDING && !strcmp(id.title,
	      "Akumajou Dracula - Circle of the Moon"), "common title: %s",
	      id.title);

	/* Unknown code, and one only the CRC can find. */
	strcpy(h.code, "ZZZZ");
	romdb_match("My Hack.gba", &h, 1234, NULL, &id);
	CHECK(id.how == ROMDB_UNKNOWN && !strcmp(id.title, "My Hack") && !id.e,
	      "unknown: %s", id.title);
	CHECK(romdb_auto_color(&id, why, sizeof(why)) == COLOR_OFF &&
	      strstr(why, "unknown"), "auto unknown: %s", why);
	e = entry_named("Apotris (World) (v4.1.0)");
	CHECK(e && !e->code[0], "Apotris entry");
	if (e) {
		romdb_match("apotris.gba", &h, e->size, NULL, &id);
		CHECK(id.how == ROMDB_PENDING, "codeless by size: pending");
		romdb_match("apotris.gba", &h, e->size, &e->crc, &id);
		CHECK(id.how == ROMDB_CRC && id.e == e, "codeless by crc");
		romdb_match("apotris.gba", &h, e->size, &bad, &id);
		CHECK(id.how == ROMDB_UNKNOWN && !strcmp(id.title, "apotris"),
		      "codeless, other crc: %s", id.title);
	}
	/* A 2005+ title gets SP101. */
	for (int i = 0; i < romdb_count; i++)
		if (romdb_year(&romdb_entries[i]) >= 2006 &&
		    romdb_entries[i].code[0] &&
		    code_count(romdb_entries[i].code,
			       romdb_entries[i].version) == 1) {
			strcpy(h.code, romdb_entries[i].code);
			h.version = romdb_entries[i].version;
			break;
		}
	romdb_match("late.gba", &h, 1, NULL, &id);
	CHECK(id.how == ROMDB_CODE &&
	      romdb_auto_color(&id, why, sizeof(why)) == COLOR_SP101,
	      "auto 2006: sp101 (%s)", why);

	/* Cache: keyed by name, size and time; pruned after a full scan. */
	snprintf(path, sizeof(path), "%s/config", base);
	mkdir(path, 0755);
	romdb_cache_load();
	romdb_cache_put("a.gba", 100, 5, 0xdeadbeef);
	romdb_cache_put("b c.gba", 200, 6, 0x00000001);
	romdb_cache_save(NULL);
	romdb_cache_reset();
	CHECK(romdb_cache_get("a.gba", 100, 5, &crc) == 0 && crc == 0xdeadbeef,
	      "cache round trip");
	CHECK(romdb_cache_get("b c.gba", 200, 6, &crc) == 0 && crc == 1,
	      "cache name with a space");
	CHECK(romdb_cache_get("a.gba", 100, 7, &crc) < 0, "cache: new mtime");
	CHECK(romdb_cache_get("a.gba", 101, 5, &crc) < 0, "cache: new size");
	CHECK(romdb_cache_get("c.gba", 100, 5, &crc) < 0, "cache: other file");
	romdb_cache_load();
	romdb_cache_get("b c.gba", 200, 6, &crc);
	romdb_cache_save("");
	romdb_cache_reset();
	CHECK(romdb_cache_get("a.gba", 100, 5, &crc) < 0 &&
	      romdb_cache_get("b c.gba", 200, 6, &crc) == 0, "cache pruned");

	/* 200 dummy ROMs: 120 by code, 30 needing the CRC (20 of them a
	 * real entry's, 10 modified), 50 unknown. */
	snprintf(dir, sizeof(dir), "%s/romdb-roms", base);
	mkdir(dir, 0755);
	for (int i = 0, made = 0, amb = 0; i < romdb_count && made + amb < 150;
	     i++) {
		const struct romdb_entry *x = &romdb_entries[i];
		int n;

		if (!x->code[0] || (i && !strcmp(romdb_entries[i - 1].code, x->code)))
			continue;
		n = code_count(x->code, x->version);
		snprintf(path, sizeof(path), "%s/%s-%d.gba", dir, x->code, i);
		if (n == 1 && made < 120) {
			write_rom(path, x->code, x->version, 0, i);
			made++;
		} else if (n > 1 && amb < 30) {
			write_rom(path, x->code, x->version, amb < 20 ?
				  x[amb & 1].crc : 0, i);
			amb++;
		}
	}
	for (int i = 0; i < 50; i++) {
		char code[8];

		snprintf(code, sizeof(code), "Z%03d", i);
		snprintf(path, sizeof(path), "%s/homebrew %02d.gba", dir, i);
		write_rom(path, code, 0, 0, i);
	}
	snprintf(path, sizeof(path), "%s/config/romdb-cache.txt", base);
	unlink(path);
	romdb_cache_reset();
	t0 = now_ns();
	CHECK(romlist_scan(&rl, dir) == 200, "200 ROMs: %d", rl.count);
	t_scan = now_ns() - t0;
	CHECK(rl.pending == 30, "pending %d", rl.pending);
	t0 = now_ns();
	while (rl.pending && steps < 10000)
		steps += romlist_step(&rl, 8000000) ? 1 : 0;
	t_crc = now_ns() - t0;
	CHECK(steps == 30, "steps %d", steps);
	for (int i = 0; i < rl.count; i++) {
		n_code += rl.roms[i].id.how == ROMDB_CODE;
		n_crc += rl.roms[i].id.how == ROMDB_CRC;
		n_mod += rl.roms[i].id.how == ROMDB_MODIFIED;
		n_unk += rl.roms[i].id.how == ROMDB_UNKNOWN;
		if (i && strcasecmp(rl.roms[i - 1].id.title, rl.roms[i].id.title) > 0)
			sorted = 0;
	}
	CHECK(n_code == 120 && n_crc == 20 && n_mod == 10 && n_unk == 50,
	      "counts: code %d crc %d modified %d unknown %d", n_code, n_crc,
	      n_mod, n_unk);
	CHECK(sorted, "ROM list not sorted by title");
	/* Again, with the cache: no CRC to compute. */
	romdb_cache_reset();
	t0 = now_ns();
	romlist_scan(&rl, dir);
	t_cached = now_ns() - t0;
	CHECK(rl.pending == 0, "cached: pending %d", rl.pending);
	for (int i = 0; i < rl.count; i++)
		if (rl.roms[i].id.how == ROMDB_CRC || rl.roms[i].id.how == ROMDB_MODIFIED)
			CHECK(rl.roms[i].id.have_crc, "cached crc %s", rl.roms[i].file);
	log_msg("selftest: romdb: 200 ROMs scanned in %.1f ms (30 CRCs pending), "
		"CRCs in %.1f ms, scanned again with the cache in %.1f ms",
		t_scan / 1e6, t_crc / 1e6, t_cached / 1e6);
	fprintf(stderr, "romdb: 200 ROMs scanned in %.1f ms, 30 CRCs in %.1f "
		"ms, cached scan %.1f ms\n", t_scan / 1e6, t_crc / 1e6,
		t_cached / 1e6);
	romlist_free(&rl);

	/* A game start computes a missing CRC itself and caches it. */
	romdb_cache_reset();
	unlink(path);
	snprintf(path, sizeof(path), "%s/aw.gba", dir);
	write_rom(path, "AWRE", 0, vc ? vc->crc : 0, 1);
	romdb_identify(path, 0, &id);
	CHECK(id.how == ROMDB_PENDING, "identify without crc: pending");
	romdb_identify(path, 1, &id);
	CHECK(id.how == ROMDB_CRC && id.e == vc, "identify with crc: %s",
	      id.title);
	romdb_cache_reset();
	romdb_identify(path, 0, &id);
	CHECK(id.how == ROMDB_CRC && id.have_crc, "identify from cache");
	romdb_cache_reset();

	/* In a folder of roms/: cached under its path relative to roms/,
	 * kept when another folder is scanned, found again from the cache. */
	snprintf(path, sizeof(path), "%s/roms/action/x.gba", base);
	CHECK(!strcmp(romdb_key(path), "action/x.gba"), "key: %s",
	      romdb_key(path));
	snprintf(path, sizeof(path), "%s/roms/top.gba", base);
	CHECK(!strcmp(romdb_key(path), "top.gba"), "key at the top: %s",
	      romdb_key(path));
	CHECK(!strcmp(romdb_key("/elsewhere/a.gba"), "a.gba"), "key elsewhere");
	snprintf(path, sizeof(path), "%s/config/romdb-cache.txt", base);
	unlink(path);
	snprintf(dir, sizeof(dir), "%s/roms/crc", base);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/aw.gba", dir);
	write_rom(path, "AWRE", 0, vc ? vc->crc : 0, 1);
	CHECK(romlist_scan(&rl, dir) == 1 && rl.pending == 1, "folder: pending %d",
	      rl.pending);
	for (int i = 0; rl.pending && i < 1000; i++)
		romlist_step(&rl, 8000000);
	CHECK(rl.count == 1 && rl.roms[0].id.how == ROMDB_CRC, "folder: crc");
	snprintf(path, sizeof(path), "%s/roms", base);
	romlist_scan(&rl, path);
	romdb_cache_reset();
	romlist_scan(&rl, dir);
	CHECK(rl.count == 1 && rl.pending == 0 && rl.roms[0].id.have_crc &&
	      rl.roms[0].id.how == ROMDB_CRC, "folder: cached after scanning "
	      "the top (pending %d)", rl.pending);
	romlist_free(&rl);
	snprintf(path, sizeof(path), "%s/aw.gba", dir);
	unlink(path);
	rmdir(dir);
	romdb_cache_reset();
}

/* The ROM list's folders in the menu: A opens one, B goes up with it
 * selected, the folder and ROM are kept over a game (the page entered
 * again), a folder that is gone falls back to the top. Uses the roms/
 * of test_romlist. */
static void test_romlist_folders(const char *base)
{
	static uint16_t px[480 * 320];
	struct canvas cv = { px, 480, 320 };
	char path[512], want[512], gone[400];
	const uint32_t a = JOY_BIT(A), b = JOY_BIT(B);

	romlist_page.enter(NULL, 0);
	CHECK(rl_key(a, path) == UI_NONE,
	      "folders: A on action/ opens it");
	CHECK(rl_key(a, path) == UI_NONE,
	      "folders: A on inner/ opens it");
	draw_page(&romlist_page, &cv, NULL);
	snprintf(path, sizeof(path), "%s/ui-romlist-folder.raw", base);
	write_file_atomic(path, px, sizeof(px));
	snprintf(want, sizeof(want), "%s/roms/action/inner/deep.gba", base);
	CHECK(rl_key(a, path) == UI_START_ROM &&
	      !strcmp(path, want), "folders: start %s", path);
	/* Back from the game: same folder, same ROM. */
	romlist_page.enter(NULL, 0);
	CHECK(rl_key(a, path) == UI_START_ROM &&
	      !strcmp(path, want), "folders: after the game %s", path);
	/* B: up to action/ with inner/ selected. */
	CHECK(rl_key(b, path) == UI_NONE, "folders: B");
	CHECK(rl_key(a, path) == UI_NONE &&
	      rl_key(a, path) == UI_START_ROM &&
	      !strcmp(path, want), "folders: inner/ reselected: %s", path);
	rl_key(b, path);
	rl_key(JOY_BIT(DOWN), path);
	snprintf(want, sizeof(want), "%s/roms/action/x.gba", base);
	CHECK(rl_key(a, path) == UI_START_ROM &&
	      !strcmp(path, want), "folders: start %s", path);
	/* Up to the top with action/ selected; B there does nothing. */
	rl_key(b, path);
	CHECK(rl_key(b, path) == UI_NONE,
	      "folders: B at the top");
	rl_key(a, path);
	rl_key(JOY_BIT(DOWN), path);
	CHECK(rl_key(a, path) == UI_START_ROM &&
	      !strcmp(path, want), "folders: action/ reselected: %s", path);
	/* The empty folder: B still goes up. */
	rl_key(b, path);
	rl_key(JOY_BIT(DOWN), path);
	rl_key(a, path);
	draw_page(&romlist_page, &cv, NULL);
	snprintf(path, sizeof(path), "%s/ui-romlist-empty.raw", base);
	write_file_atomic(path, px, sizeof(px));
	CHECK(rl_key(a, path) == UI_NONE,
	      "folders: A in an empty folder");
	rl_key(b, path);
	rl_key(JOY_BIT(UP), path);
	rl_key(a, path);
	CHECK(rl_key(JOY_BIT(DOWN), path) == UI_NONE &&
	      rl_key(a, path) == UI_START_ROM &&
	      !strcmp(path, want), "folders: up from empty/: %s", path);
	/* A folder removed while a game ran: back at the top. */
	rl_key(b, path);
	snprintf(gone, sizeof(gone), "%s/roms/gone", base);
	mkdir(gone, 0755);
	romlist_page.enter(NULL, 0);
	rl_key(JOY_BIT(DOWN), path);
	rl_key(JOY_BIT(DOWN), path);
	rl_key(a, path);
	rmdir(gone);
	romlist_page.enter(NULL, 0);
	rl_key(JOY_BIT(UP), path);
	snprintf(want, sizeof(want), "%s/roms/z.Gba", base);
	CHECK(rl_key(a, path) == UI_START_ROM &&
	      !strcmp(path, want), "folders: gone folder: %s", path);
}

/*
 * Resume on boot (resume.c): the record's text, its file written, read,
 * damaged and removed, every reason resume_check gives, and the ROM list
 * opened on the resumed ROM's folder. Uses the roms/ of test_romlist.
 */
static void test_resume(const char *base)
{
	struct resume_rec r, w = { .core = CORE_GPSP, .why = "power key held 2 s" };
	char buf[700], path[600], state[600], cfg[400];
	enum resume_check ck;
	struct settings keep = settings;

	snprintf(w.rom, sizeof(w.rom), "action/x.gba");
	CHECK(resume_format(&w, buf, sizeof(buf)) > 0 &&
	      !strcmp(buf, "rom=action/x.gba\ncore=gpsp\nwhy=power key held 2 s\n"),
	      "resume: format %s", buf);
	CHECK(resume_parse(&r, buf) == 0 && !strcmp(r.rom, w.rom) &&
	      r.core == CORE_GPSP && !r.attempted &&
	      !strcmp(r.why, "power key held 2 s"), "resume: parse back");
	w.attempted = 1;
	resume_format(&w, buf, sizeof(buf));
	CHECK(resume_parse(&r, buf) == 0 && r.attempted, "resume: attempted");
	CHECK(resume_parse(&r, "rom=a b/c.gba\r\ncore=mgba\r\n") == 0 &&
	      !strcmp(r.rom, "a b/c.gba") && r.core == CORE_MGBA,
	      "resume: CRLF, spaces in the path");
	CHECK(resume_parse(&r, "rom=x.gba\n") && resume_parse(&r, "core=gpsp\n") &&
	      resume_parse(&r, "rom=x.gba\ncore=snes\n") &&
	      resume_parse(&r, "rom=../x.gba\ncore=gpsp\n") &&
	      resume_parse(&r, "rom=/x.gba\ncore=gpsp\n") &&
	      resume_parse(&r, "rom=\ncore=gpsp\n") && resume_parse(&r, ""),
	      "resume: incomplete or unsafe records rejected");

	snprintf(path, sizeof(path), "%s/roms/action/x.gba", base);
	CHECK(resume_rom_rel(path, r.rom, sizeof(r.rom)) == 0 &&
	      !strcmp(r.rom, "action/x.gba"), "resume: relative path %s", r.rom);
	CHECK(resume_rom_rel("/elsewhere/x.gba", r.rom, sizeof(r.rom)) == -1,
	      "resume: a ROM outside roms/");

	/* The file: none, written, read back, damaged, removed. */
	resume_clear("selftest");
	CHECK(resume_read(&r) == -1, "resume: no record");
	CHECK(resume_check(resume_read(&r), &r, 1) == RESUME_NONE,
	      "resume: check without a record");
	w.attempted = 0;
	CHECK(resume_write(&w) == 0 && resume_read(&r) == 0 &&
	      !strcmp(r.rom, "action/x.gba") && !r.attempted,
	      "resume: written and read back");
	snprintf(cfg, sizeof(cfg), "%s/config/resume.txt", base);
	write_text(cfg, "rom=action/x.gba\ncore=");
	CHECK(resume_read(&r) == -2 &&
	      resume_check(-2, &r, 1) == RESUME_BAD, "resume: damaged");
	resume_clear("selftest");
	CHECK(access(cfg, F_OK) && resume_read(&r) == -1, "resume: removed");

	/* The reasons, in the order checked. */
	r = w;
	core_state_path(path, CORE_GPSP, state, sizeof(state));
	unlink(state);
	CHECK(resume_check(0, &r, 0) == RESUME_OFF, "resume: setting off");
	r.attempted = 1;
	CHECK(resume_check(0, &r, 1) == RESUME_ATTEMPTED, "resume: attempted");
	r.attempted = 0;
	snprintf(r.rom, sizeof(r.rom), "action/gone.gba");
	CHECK(resume_check(0, &r, 1) == RESUME_NO_ROM, "resume: ROM gone");
	snprintf(r.rom, sizeof(r.rom), "action");
	CHECK(resume_check(0, &r, 1) == RESUME_NO_ROM, "resume: a folder");
	r = w;
	CHECK(resume_check(0, &r, 1) == RESUME_NO_STATE, "resume: no state");
	write_text(state, "state");
	ck = resume_check(0, &r, 1);
	CHECK(ck == RESUME_GO, "resume: go (%s)", resume_check_names[ck]);
	CHECK(rom_settings.core == CORE_GPSP && rom_settings.color == COLOR_AUTO,
	      "resume: ROM settings reset after the check");
	snprintf(cfg, sizeof(cfg), "%s/config/roms/x.txt", base);
	write_text(cfg, "core=mgba\n");
	CHECK(resume_check(0, &r, 1) == RESUME_CORE, "resume: core changed");
	unlink(cfg);
	unlink(state);

	/* The list opens on the ROM, in its folder, A starts it. */
	romlist_menu_select("action/x.gba");
	romlist_page.enter(NULL, 0);
	CHECK(rl_key(JOY_BIT(A), buf) == UI_START_ROM && !strcmp(buf, path),
	      "resume: list on %s", buf);
	romlist_menu_select("action/inner/deep.gba");
	romlist_page.enter(NULL, 0);
	snprintf(path, sizeof(path), "%s/roms/action/inner/deep.gba", base);
	CHECK(rl_key(JOY_BIT(A), buf) == UI_START_ROM && !strcmp(buf, path),
	      "resume: list on %s", buf);
	/* Back at the top for the tests after this one. */
	romlist_menu_select("z.Gba");
	romlist_page.enter(NULL, 0);
	snprintf(path, sizeof(path), "%s/roms/z.Gba", base);
	CHECK(rl_key(JOY_BIT(A), buf) == UI_START_ROM && !strcmp(buf, path),
	      "resume: list on %s", buf);
	settings = keep;
}

static void test_files(const char *base)
{
	static uint8_t data[70000], back[80000];
	char path[300], tmp[310];

	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 7);
	snprintf(path, sizeof(path), "%s/test.sav", base);
	CHECK(write_file_atomic(path, data, sizeof(data)) == 0, "atomic write");
	CHECK(read_file(path, back, sizeof(back)) == (int)sizeof(data), "size");
	CHECK(!memcmp(data, back, sizeof(data)), "content");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	CHECK(access(tmp, F_OK) != 0, "temp file left behind");
	CHECK(write_file_atomic(path, data, 512) == 0, "overwrite");
	CHECK(read_file(path, back, sizeof(back)) == 512, "shrunk");
	snprintf(path, sizeof(path), "%s/nodir/x.sav", base);
	CHECK(write_file_atomic(path, data, 16) != 0, "write into missing dir");
	{
		/* No room for ".tmp": refused, not written to a cut name. */
		static char lng[PATH_MAX];

		memset(lng, 'a', sizeof(lng) - 3);
		lng[0] = '/';
		CHECK(write_file_atomic(lng, data, 16) != 0 &&
		      errno == ENAMETOOLONG, "temp name too long");
	}
}

/* ---------------------------------------------------------- cpufreq */

#define SIM_PERIOD_NS 16661000LL
/* A start too low jumps after a few CPU bound misses (cpuctl.c warmup). */
#define WARMUP_MISSES_MAX 4

static char sim_ps[320];
static int64_t sim_t = 10000000000000000LL;  /* after test_battery's clock */

/* Simulated frame load: base_ms at 1296 MHz, scaling with 1/f. */
struct sim {
	double base_ms, noise_ms;
	int heavy_every;        /* frames, 0: none */
	double heavy_mul;
	int burst_every, burst_len;  /* frames */
	double burst_mul;
	int skip;               /* frames skipped after each rendered one */
	double skip_ms;         /* the work of a skipped frame */
};

static long sim_read_long(const char *dir, const char *name)
{
	char path[400], buf[32];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (read_file(path, buf, sizeof(buf) - 1) <= 0)
		return -1;
	buf[31] = 0;
	return strtol(buf, NULL, 10);
}

static void sim_run(const struct sim *sm, double seconds)
{
	static unsigned pending;
	long frames = (long)(seconds * 60), last_f = 0;

	for (long i = 0; i < frames; i++) {
		long f = cpu_freq_khz();
		int skipped = sm->skip && i % (sm->skip + 1) != 0;
		double w = (skipped ? sm->skip_ms : sm->base_ms) * 1296000.0 /
			   (f > 0 ? f : 1296000);
		double period = SIM_PERIOD_NS / 1e6;

		w += sm->noise_ms * ((rnd16() & 1023) / 511.5 - 1);
		if (sm->heavy_every && i % sm->heavy_every == sm->heavy_every - 1)
			w *= sm->heavy_mul;
		if (sm->burst_every && i % sm->burst_every < sm->burst_len)
			w *= sm->burst_mul;
		if (f != last_f) {
			/* Battery current grows with the clock. */
			char v[24];

			snprintf(v, sizeof(v), "%ld\n", -(200000 + 300 * f / 1000));
			put(sim_ps, "current_avg", v);
			last_f = f;
		}
		cpuctl_game_frame((int64_t)(w * 1e6), pending, skipped, sim_t);
		/* Reported with the next frame, as the flip events do. */
		pending = w > period - 0.3 ? (unsigned)((w + 0.3) / period) : 0;
		sim_t += SIM_PERIOD_NS * (1 + pending);
		battery_poll(sim_t);
	}
}

/* Field col (0 based) of the first perf.csv row containing key. */
static double csv_field(const char *csv, const char *key, int col)
{
	const char *p = strstr(csv, key);

	if (!p)
		return -1;
	while (p > csv && p[-1] != '\n')
		p--;
	for (int i = 0; i < col && p; i++) {
		p = strchr(p, ',');
		if (p)
			p++;
	}
	return p ? atof(p) : -1;
}

static void test_cpuctl(const char *base)
{
	static char csv[65536];
	char fake[300], path[400];
	struct cpuctl_counters k;
	int learned, n;
	static const struct sim light = { 6, 0.3, 200, 1.3, 0, 0, 0, 0, 0 };
	static const struct sim heavy = { 11, 0.3, 0, 0, 0, 0, 0, 0, 0 };
	static const struct sim bursty = { 6, 0.3, 0, 0, 900, 60, 2.0, 0, 0 };
	static const struct sim mid = { 9, 0.2, 0, 0, 0, 0, 0, 0, 0 };
	static const struct sim rising = { 11.6, 0.1, 0, 0, 0, 0, 0, 0, 0 };
	/* mid, with two of every three frames skipped at a tenth of the
	 * work (fixed interval 2). */
	static const struct sim mid_skip = { 9, 0.2, 0, 0, 0, 0, 0, 2, 0.9 };

	snprintf(fake, sizeof(fake), "%s/cpufreq", base);
	snprintf(sim_ps, sizeof(sim_ps), "%s/power_supply/rk817-battery", base);
	put(sim_ps, "voltage_avg", "3912000\n");
	CHECK(cpuctl_fake_sysfs(fake) == 0, "fake cpufreq sysfs");
	cpuctl_init(fake);
	CHECK(cpuctl_opp_count() == 5 && cpuctl_opp_khz(0) == 600000 &&
	      cpuctl_opp_khz(4) == 1296000, "OPP list");
	cpuctl_set_mode(CPU_MODE_ADAPTIVE);
	CHECK(cpuctl_mode_step(CPU_MODE_ADAPTIVE, 1) == CPU_MODE_MAX &&
	      cpuctl_mode_step(CPU_MODE_MAX, 1) == 600 &&
	      cpuctl_mode_step(1200, 1) == CPU_MODE_ADAPTIVE &&
	      cpuctl_mode_step(CPU_MODE_ADAPTIVE, -1) == 1200, "mode order");

	/* Light load: steps down to the lowest OPP with margin and stays. */
	cpuctl_game_start("sim.gba", 1e9 / SIM_PERIOD_NS, 0, sim_t);
	CHECK(cpu_freq_khz() == 1296000, "start at the top: %ld", cpu_freq_khz());
	sim_run(&light, 90);
	cpuctl_counters(&k);
	log_msg("selftest: cpuctl light: %ld kHz, %u changes (%u down, %u up), "
		"missed %u", cpu_freq_khz(), k.changes, k.downs, k.ups, k.missed);
	CHECK(cpu_freq_khz() == 816000, "light load ends at %ld", cpu_freq_khz());
	CHECK(k.downs == 3 && k.ups == 0 && k.missed == 0,
	      "light load: %u down %u up %u missed", k.downs, k.ups, k.missed);
	CHECK(sim_read_long(fake, "uclamp_min") == 644, "util_min for 816 MHz");
	/* Menu: no floor; resume: back. */
	cpuctl_game_pause();
	CHECK(sim_read_long(fake, "uclamp_min") == 0 && cpu_freq_khz() == 600000,
	      "menu without floor");
	sim_t += 30 * 1000000000LL;
	cpuctl_game_resume();
	CHECK(cpu_freq_khz() == 816000, "resume at %ld", cpu_freq_khz());
	sim_run(&light, 5);
	learned = cpuctl_game_end();
	CHECK(learned == 816, "learned %d MHz", learned);
	CHECK(sim_read_long(fake, "uclamp_min") == 0, "no floor after the game");

	/* The learned start misses with a heavier load: top, and forgotten. */
	cpuctl_game_start("sim.gba", 1e9 / SIM_PERIOD_NS, learned, sim_t);
	CHECK(cpu_freq_khz() == 816000, "learned start %ld", cpu_freq_khz());
	sim_run(&heavy, 120);
	cpuctl_counters(&k);
	log_msg("selftest: cpuctl heavy: %ld kHz, %u changes, %u jumps, missed "
		"%u (cpu %u)", cpu_freq_khz(), k.changes, k.jumps, k.missed,
		k.missed_cpu);
	CHECK(k.jumps == 1 && k.missed <= WARMUP_MISSES_MAX && k.changes <= 3,
	      "heavy: %u jumps %u missed %u changes", k.jumps, k.missed,
	      k.changes);
	CHECK(cpu_freq_khz() >= 1200000, "heavy load at %ld", cpu_freq_khz());
	learned = cpuctl_game_end();
	CHECK(learned == 0, "failed start forgotten: %d", learned);

	/* Bursts that only fit the top OPP: the hold backs off. */
	cpuctl_game_start("sim.gba", 1e9 / SIM_PERIOD_NS, 0, sim_t);
	sim_run(&bursty, 600);
	cpuctl_counters(&k);
	log_msg("selftest: cpuctl bursts: %u changes, %u jumps, missed %u, hold "
		"%lld s", k.changes, k.jumps, k.missed,
		(long long)(k.hold_ns / 1000000000LL));
	CHECK(k.jumps >= 2 && k.jumps <= 8, "bursts: %u jumps", k.jumps);
	CHECK(k.hold_ns >= 80 * 1000000000LL, "hold did not back off");
	CHECK(k.missed <= 2 * k.jumps, "bursts: %u missed", k.missed);
	cpuctl_game_end();

	/* Rising load steps up before missing. */
	cpuctl_game_start("sim.gba", 1e9 / SIM_PERIOD_NS, 1008, sim_t);
	sim_run(&mid, 30);
	CHECK(cpu_freq_khz() == 1008000, "mid load at %ld", cpu_freq_khz());
	sim_run(&rising, 10);
	cpuctl_counters(&k);
	CHECK(k.ups == 1 && k.missed == 0 && cpu_freq_khz() == 1200000,
	      "rising: %u ups %u missed at %ld", k.ups, k.missed, cpu_freq_khz());

	/* Live mode changes. */
	cpuctl_set_mode(CPU_MODE_MAX);
	CHECK(cpu_freq_khz() == 1296000 &&
	      sim_read_long(fake, "uclamp_min") == 1024, "max mode");
	sim_run(&light, 30);
	CHECK(cpu_freq_khz() == 1296000, "max mode stays");
	cpuctl_set_mode(600);
	CHECK(sim_read_long(fake, "uclamp_min") == 474, "util_min for 600 MHz");
	cpuctl_set_mode(1008);
	CHECK(sim_read_long(fake, "uclamp_min") == 796, "util_min for 1008 MHz");
	cpuctl_set_mode(1200);
	CHECK(sim_read_long(fake, "uclamp_min") == 948, "util_min for 1200 MHz");
	cpuctl_set_mode(816);
	sim_run(&light, 20);
	CHECK(cpu_freq_khz() == 816000, "fixed 816");
	cpuctl_game_end();
	cpuctl_set_mode(CPU_MODE_ADAPTIVE);

	/* Skipped frames are counted but not judged: the mid load settles
	 * where it does without skipping, the work statistics are of the
	 * rendered frames. */
	cpuctl_game_start("skip.gba", 1e9 / SIM_PERIOD_NS, 0, sim_t);
	sim_run(&mid_skip, 60);
	cpuctl_counters(&k);
	log_msg("selftest: cpuctl skipping: %ld kHz, %u changes, %u skipped, "
		"missed %u", cpu_freq_khz(), k.changes, k.skipped, k.missed);
	CHECK(cpu_freq_khz() == 1008000 && k.missed == 0 && k.ups == 0,
	      "skipping: mid load at %ld, %u missed, %u ups", cpu_freq_khz(),
	      k.missed, k.ups);
	CHECK(k.skipped == 2400, "skipping: %u of 3600 frames skipped",
	      k.skipped);
	cpuctl_game_end();

	snprintf(path, sizeof(path), "%s/log/perf.csv", base);
	n = read_file(path, csv, sizeof(csv) - 1);
	CHECK(n > 0, "no perf.csv");
	csv[n > 0 ? n : 0] = 0;
	{
		int header = 0, all = 0;

		for (char *p = csv; (p = strstr(p, "\ntime,")) || 0; p++)
			header++;
		header += !strncmp(csv, "time,", 5);
		for (char *p = csv; (p = strstr(p, ",all,")); p++)
			all++;
		CHECK(header == 1 && all == 6, "perf.csv: %d headers, %d summaries",
		      header, all);
	}
	/* The skipping game's 1008 MHz row: work_avg_ms (column 7) of the
	 * rendered frames (9 ms at 1296 MHz), skipped (column 28). */
	CHECK(fabs(csv_field(csv, "\"skip.gba\",adaptive,1008000,", 7) -
		   9 * 1296.0 / 1008) < 0.5,
	      "skipping: work avg %.2f ms",
	      csv_field(csv, "\"skip.gba\",adaptive,1008000,", 7));
	CHECK(csv_field(csv, "\"skip.gba\",adaptive,all,", 28) == 2400 &&
	      csv_field(csv, "\"skip.gba\",adaptive,all,", 5) == 3600,
	      "skipping: perf.csv skipped %.0f of %.0f frames",
	      csv_field(csv, "\"skip.gba\",adaptive,all,", 28),
	      csv_field(csv, "\"skip.gba\",adaptive,all,", 5));
	/* avg_mA (column 16) of the 816 MHz row: 200 + 0.3 * 816. */
	CHECK(fabs(csv_field(csv, ",adaptive,816000,", 16) - 445) < 2,
	      "816 MHz battery current %.0f", csv_field(csv, ",adaptive,816000,",
							16));
	CHECK(csv_field(csv, ",max,1296000,", 5) >= 1800, "max mode row");
	CHECK(csv_field(csv, ",fixed,816000,", 5) >= 1200, "fixed mode row");
}

/* Simulated frames of the 32768 Hz core audio, each flushed, as in game:
 * the buffer status (core_run calls it with a core loaded), the lowest
 * occupancy and the frames it said an underrun is likely. */
static void fs_audio(int frames, unsigned *min_occ, int *low_frames)
{
	static int16_t silence[600 * 2];
	double acc = 0;

	*min_occ = 100;
	*low_frames = 0;
	for (int f = 0; f < frames; f++) {
		unsigned occ;
		int low, n;

		if (audio_buffer_status(&occ, &low)) {
			*min_occ = occ < *min_occ ? occ : *min_occ;
			*low_frames += low;
		}
		acc += 32768 / 59.7275;
		n = (int)acc;
		acc -= n;
		audio_push(silence, n);
		audio_flush();
	}
}

/* Frames of the loaded core, flushed; the frames it skipped. */
static unsigned fs_run(int frames)
{
	unsigned run0, skip0, run1, skip1;

	core_frame_counts(&run0, &skip0);
	for (int f = 0; f < frames; f++) {
		core_run(0);
		audio_flush();
	}
	core_frame_counts(&run1, &skip1);
	CHECK(run1 - run0 == (unsigned)frames, "frameskip: %u frames counted "
	      "for %d", run1 - run0, frames);
	return skip1 - skip0;
}

/*
 * Frame skipping: the setting's keys and values per core, the audio
 * buffer status on the simulated device, and through the stub core
 * (which skips like gpSP, see stub_core.c): Auto skips only while the
 * buffer runs low, a fixed interval set live skips exactly, Off none.
 */
static void test_frameskip(const char *base)
{
	static const struct {
		int v;
		const char *mode, *interval, *mgba;
	} map[] = {
		{ FRAMESKIP_OFF, "disabled", "1", "0" },
		{ FRAMESKIP_AUTO, "auto_threshold", "1", "0" },
		{ FRAMESKIP_1, "fixed_interval", "1", "1" },
		{ FRAMESKIP_2, "fixed_interval", "2", "2" },
		{ FRAMESKIP_3, "fixed_interval", "3", "3" },
	};
	const struct rom_settings keep = rom_settings;
	const int *fs = &rom_settings.frameskip;
	char rom[300], err[128];
	unsigned occ, skipped;
	int low;

	for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
		const char *m, *n, *g;

		rom_settings.frameskip = map[i].v;
		m = core_option_value("gpsp_frameskip");
		n = core_option_value("gpsp_frameskip_interval");
		g = core_option_value("mgba_frameskip");
		CHECK(m && n && g && !strcmp(m, map[i].mode) &&
		      !strcmp(n, map[i].interval) && !strcmp(g, map[i].mgba),
		      "frameskip %s: gpsp %s/%s, mgba %s", frameskip_names[map[i].v],
		      m ? m : "-", n ? n : "-", g ? g : "-");
	}
	CHECK(!core_option_value("gpsp_frameskip_threshold"),
	      "frameskip: gpSP's threshold left at its default");
	CHECK(core_setting_supported(fs, CORE_GPSP, FRAMESKIP_AUTO) &&
	      !core_setting_supported(fs, CORE_MGBA, FRAMESKIP_AUTO) &&
	      core_setting_supported(fs, CORE_MGBA, FRAMESKIP_2) &&
	      core_setting_supported(fs, CORE_MGBA, -1) &&
	      !core_setting_supported(&rom_settings.blend, CORE_MGBA, -1) &&
	      core_setting_supported(&rom_settings.blend, CORE_GPSP, 1) &&
	      !core_setting_supported(&rom_settings.color, CORE_GPSP, -1),
	      "frameskip: supported values per core");

	/* The buffer status: none before the stream starts, the level near
	 * the rate control's target (reads 50%) in steady play,
	 * none after a pause; a device playing 30% fast drains it. */
	settings.audio = AUDIO_ADAPTIVE;
	audio_init_null(0);
	audio_set_source(32768, 59.7275, 60.0186);
	CHECK(!audio_buffer_status(&occ, &low) && occ == 0 && !low,
	      "audio status before the stream");
	fs_audio(600, &occ, &low);
	CHECK(audio_buffer_status(&occ, &low), "audio status in play");
	log_msg("selftest: audio buffer status in play: %u%%, low %d", occ, low);
	CHECK(occ >= 45 && occ <= 55 && !low, "audio status in play: %u%% %d",
	      occ, low);
	audio_pause();
	CHECK(!audio_buffer_status(&occ, &low), "audio status after a pause");
	audio_init_null(0.3);
	audio_set_source(32768, 59.7275, 60.0186);
	fs_audio(600, &occ, &low);
	log_msg("selftest: audio buffer status, device 30%% fast: lowest %u%%, "
		"%d frames low", occ, low);
	CHECK(occ < 20 && low > 0, "audio status draining: %u%%, %d low", occ,
	      low);

	/* Through the stub core as gpSP. */
	snprintf(rom, sizeof(rom), "%s/fstest.gba", base);
	write_text(rom, "not a real ROM, the stub does not care");
	rom_settings = keep;
	rom_settings.core = CORE_GPSP;
	rom_settings.frameskip = FRAMESKIP_AUTO;
	audio_init_null(0);
	if (core_load(rom, CORE_GPSP, err, sizeof(err))) {
		CHECK(0, "frameskip: %s", err);
		rom_settings = keep;
		return;
	}
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	skipped = fs_run(600);
	CHECK(skipped == 0, "frameskip auto, steady audio: %u skipped", skipped);
	audio_init_null(0.3);
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	skipped = fs_run(600);
	log_msg("selftest: frameskip auto, device 30%% fast: %u of 600 skipped",
		skipped);
	CHECK(skipped > 0 && skipped < 600,
	      "frameskip auto, draining audio: %u skipped", skipped);
	audio_init_null(0);
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	rom_settings.frameskip = FRAMESKIP_2;
	core_options_changed();
	fs_run(1);  /* the stub reads the options after this run */
	skipped = fs_run(300);
	CHECK(skipped == 200, "frameskip 2 live: %u of 300 skipped", skipped);
	rom_settings.frameskip = FRAMESKIP_OFF;
	core_options_changed();
	fs_run(1);
	skipped = fs_run(300);
	CHECK(skipped == 0, "frameskip off live: %u skipped", skipped);
	/* With Off the stub removed the callback: a drained buffer does not
	 * make it skip. */
	audio_init_null(0.3);
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	skipped = fs_run(300);
	CHECK(skipped == 0, "frameskip off, draining audio: %u skipped",
	      skipped);
	core_unload();
	audio_init_null(0);
	rom_settings = keep;
}

/* The anti-aliased font: an 'H' on the selection bar has opaque stem
 * pixels and blended edges, a 2x one the same pixels doubled, and no
 * pixel outside its cell changes. */
static void test_font(void)
{
	static uint16_t px[64 * 64];
	struct canvas cv = { px, 64, 64 };
	int opaque = 0, mixed = 0, outside = 0, same = 1;

	draw_fill(&cv, 0, 0, 64, 64, COL_SEL);
	draw_text(&cv, 4, 4, COL_FG, "H");
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			uint16_t v = px[y * 64 + x];
			int in = x >= 4 && x < 4 + FONT_W && y >= 4 && y < 4 + FONT_H;

			opaque += in && v == COL_FG;
			mixed += in && v != COL_FG && v != COL_SEL;
			outside += !in && v != COL_SEL;
		}
	CHECK(opaque > 20 && mixed > 0 && outside == 0,
	      "font: %d opaque, %d blended, %d outside", opaque, mixed, outside);
	draw_fill(&cv, 0, 0, 64, 64, COL_SEL);
	draw_text(&cv, 0, 0, COL_FG, "H");
	draw_text_scaled(&cv, 2 * FONT_W, 0, 2, COL_FG, "H");
	for (int y = 0; y < 2 * FONT_H; y++)
		for (int x = 0; x < 2 * FONT_W; x++)
			same &= px[y * 64 + 2 * FONT_W + x] ==
				px[y / 2 * 64 + x / 2];
	CHECK(same, "font: 2x glyph is the 1x one doubled");
}

/* Long ROM names: the marquee timing and a scrolled, clipped row, in
 * the ROM list's columns. */
static void test_marquee(void)
{
	enum { H = FONT_H + 8 };
	static uint16_t px[480 * H], ref[480 * H];
	struct canvas cv = { px, 480, H }, rc = { ref, 480, H };
	const char *name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz *";
	const int cols = LIST_COLS, right = 8 + FONT_W * cols;
	int n = strlen(name), excess = FONT_W * (n - cols), prev = 0, bad = 0;
	char cut[96];

	CHECK(excess > 0, "marquee: test name fits in %d columns", cols);
	for (unsigned f = 0; f < 1000; f++) {
		int o = marquee_offset(excess, f);

		CHECK(marquee_offset(0, f) == 0 && marquee_offset(-16, f) == 0,
		      "marquee: a short name scrolls at frame %u", f);
		if (o != prev && o != prev + 1 && !(o == 0 && prev == excess))
			bad++;
		prev = o;
	}
	CHECK(!bad, "marquee: %d steps other than one pixel or the wrap", bad);
	CHECK(marquee_offset(excess, 0) == 0 && marquee_offset(excess, 60) == 0,
	      "marquee: 1 s pause first");
	CHECK(marquee_offset(excess, 61) == 1 && marquee_offset(excess, 70) == 10,
	      "marquee: one pixel per frame");
	CHECK(marquee_offset(excess, 60 + excess) == excess &&
	      marquee_offset(excess, 60 + excess + 59) == excess,
	      "marquee: 1 s at the end");
	CHECK(marquee_offset(excess, 120 + excess) == 0 &&
	      marquee_offset(excess, 120 + excess + 60) == 0 &&
	      marquee_offset(excess, 120 + excess + 61) == 1,
	      "marquee: back to the start, paused again");

	/* Not scrolled: cut with "~*"; a name that fits ignores the offset. */
	draw_fill(&cv, 0, 0, 480, H, 0);
	draw_rom_name(&cv, 8, 4, COL_FG, name, cols, 0);
	draw_fill(&rc, 0, 0, 480, H, 0);
	snprintf(cut, sizeof(cut), "%.*s~*", cols - 2, name);
	draw_text(&rc, 8, 4, COL_FG, cut);
	CHECK(!memcmp(px, ref, sizeof(px)), "marquee: cut row");
	draw_fill(&cv, 0, 0, 480, H, 0);
	draw_rom_name(&cv, 8, 4, COL_FG, "Short (USA) *", cols, 40);
	draw_fill(&rc, 0, 0, 480, H, 0);
	draw_text(&rc, 8, 4, COL_FG, "Short (USA) *");
	CHECK(!memcmp(px, ref, sizeof(px)), "marquee: a short name moved");

	/* Scrolled by 37 and to the end: all of the name, " *" included,
	 * shifted and clipped to the columns. */
	for (int k = 0; k < 2; k++) {
		int off = k ? excess : 37;

		draw_fill(&cv, 0, 0, 480, H, 0);
		draw_rom_name(&cv, 8, 4, COL_FG, name, cols, off);
		draw_fill(&rc, 0, 0, 480, H, 0);
		draw_text(&rc, 8 - off, 4, COL_FG, name);
		draw_fill(&rc, 0, 0, 8, H, 0);
		draw_fill(&rc, right, 0, 480, H, 0);
		CHECK(!memcmp(px, ref, sizeof(px)), "marquee: row scrolled by %d",
		      off);
	}
	/* At the end the last column holds the "*". */
	draw_fill(&rc, 0, 0, 480, H, 0);
	draw_text(&rc, right - FONT_W, 4, COL_FG, "*");
	bad = 0;
	for (int y = 0; y < H; y++)
		bad += !!memcmp(px + y * 480 + right - FONT_W,
				ref + y * 480 + right - FONT_W, FONT_W * 2);
	CHECK(!bad, "marquee: no \"*\" at the end (%d rows differ)", bad);
}

/* Pixels of col in rows [y0, y1). */
static int count_color(const uint16_t *px, int y0, int y1, uint16_t col)
{
	int n = 0;

	for (int i = y0 * 480; i < y1 * 480; i++)
		n += px[i] == col;
	return n;
}

/*
 * In-game menu: Restart game is the 2nd row and needs a second A within
 * 3 s (180 updates at 60 Hz); moving away or waiting cancels, B
 * resumes. All 11 rows fit in the box, which ends above the bottom edge.
 * Power off, the last row, asks the same way, and so does Power off in
 * the global settings.
 */
static void test_ingame_restart(const char *dir)
{
	static uint16_t px[480 * 320];
	struct canvas cv = { px, 480, 320 };
	char path[300];
	int act, armed, plain;
	/* The selection bars of the in-game menu's rows 1 and 10, its end;
	 * the global settings' row 8 (Power off) and the end of the 9. */
	const int row1 = IG_BOX_Y + IG_HEAD_H + IG_ROW_H -
			 (IG_ROW_H - FONT_H) / 2;
	const int last = row1 + 9 * IG_ROW_H, end = IG_BOX_Y + IG_BOX_H;
	const int srow8 = LIST_Y - ROW_PAD + 8 * ROW_H;
	const int send = LIST_Y - ROW_PAD + 9 * ROW_H;

	ig_open("Game");
	ig_key(JOY_BIT(DOWN));
	draw_page(&ingame_page, &cv, NULL);
	/* Row 1, its selection bar. */
	plain = count_color(px, row1, row1 + IG_ROW_H, COL_HI);
	CHECK(ig_key(JOY_BIT(A)) == UI_NONE, "restart: first A arms");
	draw_page(&ingame_page, &cv, NULL);
	armed = count_color(px, row1, row1 + IG_ROW_H, COL_HI);
	CHECK(plain == 0 && armed > 0, "restart: armed row %d/%d highlighted",
	      plain, armed);
	snprintf(path, sizeof(path), "%s/ui-ingame-restart.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	CHECK(ig_key(JOY_BIT(A)) == UI_RESET,
	      "restart: second A confirms");
	CHECK(ig_key(JOY_BIT(A)) == UI_NONE,
	      "restart: disarmed after confirming");

	/* Moving away and back cancels. */
	ig_open("Game");
	ig_key(JOY_BIT(DOWN));
	ig_key(JOY_BIT(A));
	ig_key(JOY_BIT(DOWN));
	ig_key(JOY_BIT(UP));
	CHECK(ig_key(JOY_BIT(A)) == UI_NONE,
	      "restart: moving away cancels");
	/* An A on the 179th frame after arming still confirms, on the
	 * 180th it does not. */
	ig_open("Game");
	ig_key(JOY_BIT(DOWN));
	ig_key(JOY_BIT(A));
	for (int i = 0; i < 178; i++)
		ig_key(0);
	CHECK(ig_key(JOY_BIT(A)) == UI_RESET,
	      "restart: confirmed within 3 s");
	ig_open("Game");
	ig_key(JOY_BIT(DOWN));
	ig_key(JOY_BIT(A));
	for (int i = 0; i < 179; i++)
		ig_key(0);
	CHECK(ig_key(JOY_BIT(A)) == UI_NONE,
	      "restart: times out after 3 s");
	/* That A armed it again; 180 frames later the label is back. */
	for (int i = 0; i < 180; i++)
		ig_key(0);
	draw_page(&ingame_page, &cv, NULL);
	CHECK(count_color(px, row1, row1 + IG_ROW_H, COL_HI) == 0,
	      "restart: label back after 3 s");
	/* B resumes; the menu opens again disarmed. */
	CHECK(ig_key(JOY_BIT(B)) == UI_RESUME, "restart: B resumes");
	ig_open("Game");
	ig_key(JOY_BIT(DOWN));
	draw_page(&ingame_page, &cv, NULL);
	CHECK(count_color(px, row1, row1 + IG_ROW_H, COL_HI) == 0,
	      "restart: disarmed on entering");

	/* Up from Resume wraps to Power off, the last row: its highlight
	 * ends IG_TAIL above the box, whose frame ends on screen, the rest
	 * is the dimmed game. */
	ig_open("Game");
	act = ig_key(JOY_BIT(UP));
	draw_page(&ingame_page, &cv, NULL);
	CHECK(IG_BOX_Y > 0 && end <= 320 && act == UI_NONE &&
	      px[(last + IG_ROW_H - 1) * 480 + 240] == COL_SEL &&
	      px[(end - 3) * 480 + 240] == COL_BOX &&
	      px[(end - 1) * 480 + 240] == COL_SEL &&
	      count_color(px, end, 320, COL_SEL) == 0 &&
	      count_color(px, end, 320, COL_BOX) == 0,
	      "restart: menu box fits on screen");
	CHECK(ig_key(JOY_BIT(A)) == UI_NONE,
	      "power off: first A arms");
	draw_page(&ingame_page, &cv, NULL);
	CHECK(count_color(px, last, last + IG_ROW_H, COL_HI) > 0,
	      "power off: armed row highlighted");
	snprintf(path, sizeof(path), "%s/ui-ingame-poweroff.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	CHECK(ig_key(JOY_BIT(A)) == UI_POWEROFF,
	      "power off: second A confirms");
	/* Armed on Restart game, A on Power off only arms that row. */
	ig_open("Game");
	ig_key(JOY_BIT(DOWN));
	ig_key(JOY_BIT(A));
	ig_key(JOY_BIT(UP));
	ig_key(JOY_BIT(UP));
	CHECK(ig_key(JOY_BIT(A)) == UI_NONE,
	      "power off: not confirmed by another row's arming");
	for (int i = 0; i < 179; i++)
		ig_key(0);
	CHECK(ig_key(JOY_BIT(A)) == UI_NONE,
	      "power off: times out after 3 s");

	/* Global settings: Power off is the last row. */
	settings_page.enter(NULL, 0);
	for (int i = 0; i < 8; i++)
		st_key(JOY_BIT(DOWN));
	draw_page(&settings_page, &cv, NULL);
	plain = count_color(px, srow8, srow8 + ROW_H, COL_HI);
	CHECK(st_key(JOY_BIT(A)) == UI_NONE,
	      "settings power off: first A arms");
	draw_page(&settings_page, &cv, NULL);
	armed = count_color(px, srow8, srow8 + ROW_H, COL_HI);
	CHECK(plain == 0 && armed > 0,
	      "settings power off: armed row %d/%d highlighted", plain, armed);
	CHECK(send <= FOOT_Y &&
	      count_color(px, send, FOOT_Y, COL_SEL) == 0 &&
	      count_color(px, send, FOOT_Y, COL_FG) == 0 &&
	      count_color(px, send, FOOT_Y, COL_DIM) == 0,
	      "settings: rows end above B:back");
	snprintf(path, sizeof(path), "%s/ui-settings-poweroff.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	CHECK(st_key(JOY_BIT(A)) == UI_POWEROFF,
	      "settings power off: second A confirms");
	st_key(JOY_BIT(A));
	st_key(JOY_BIT(DOWN));
	st_key(JOY_BIT(UP));
	CHECK(st_key(JOY_BIT(A)) == UI_NONE,
	      "settings power off: moving away cancels");
	settings_page.enter(NULL, 0);
	for (int i = 0; i < 8; i++)
		st_key(JOY_BIT(DOWN));
	st_key(JOY_BIT(A));
	for (int i = 0; i < 179; i++)
		st_key(0);
	CHECK(st_key(JOY_BIT(A)) == UI_NONE,
	      "settings power off: times out after 3 s");
}

/* Each directory of a path under base, then the file. */
static void put_path(const char *base, const char *rel, const char *val)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", base, rel);
	for (char *p = path + strlen(base) + 1; (p = strchr(p, '/')); p++) {
		*p = 0;
		mkdir(path, 0755);
		*p = '/';
	}
	write_text(path, val);
}

/* Rows [y, y + FONT_H) from x match text drawn there on the background. */
static int row_shows_on(const uint16_t *px, int x, int y, const char *text,
			uint16_t fg, uint16_t bg)
{
	static uint16_t ref[480 * 320];
	struct canvas rc = { ref, 480, 320 };
	int w = draw_text_width(text);

	draw_fill(&rc, 0, 0, 480, 320, bg);
	draw_text(&rc, x, y, fg, text);
	for (int r = y; r < y + FONT_H; r++)
		if (memcmp(px + r * 480 + x, ref + r * 480 + x, w * 2))
			return 0;
	return 1;
}

static int row_shows(const uint16_t *px, int x, int y, const char *text)
{
	return row_shows_on(px, x, y, text, COL_FG, COL_BG);
}

/*
 * System info: /proc/stat and /proc/meminfo parsing, then the page on a
 * fake /proc and /sys (two cores, the two PX30 tsadc zones, one hwmon
 * sensor, one cpufreq policy): CPU usage since boot, then from the
 * deltas after a second, the temperatures, and B goes back.
 */
/* Row k of the system info page shows text as its value. */
#define SI_SHOWS(k, text) \
	row_shows(px, SYSINFO_VAL_X, SYSINFO_ROW_Y + (k) * SYSINFO_ROW_H, text)

static void test_sysinfo(const char *dir)
{
	static const char stat0[] =
		"cpu  100 0 100 700 100 0 0 0 0 0\n"
		"cpu0 50 0 50 350 50 0 0 0 0 0\n"
		"cpu1 50 0 50 350 50 0 0 0 0 0\n"
		"intr 12345 0 0\nctxt 999\n";
	static const char stat1[] =
		"cpu  250 50 100 900 100 0 0 0 0 0\n"
		"cpu0 50 0 50 550 50 0 0 0 0 0\n"
		"cpu1 200 50 50 350 50 0 0 0 0 0\n";
	static uint16_t px[480 * 320];
	struct canvas cv = { px, 480, 320 };
	struct cpu_times t[4], u[4];
	char root[300], path[400];
	int n;

	memset(t, 0, sizeof(t));
	n = sysinfo_parse_stat(stat0, t, 4);
	CHECK(n == 3 && t[0].busy == 200 && t[0].total == 1000 &&
	      t[2].busy == 100 && t[2].total == 500,
	      "sysinfo: stat parse %d %llu/%llu", n, t[0].busy, t[0].total);
	CHECK(sysinfo_parse_stat("cpu0 1 2 3 4\n", u, 4) == 0,
	      "sysinfo: no total line");
	/* A core past max is ignored, the rest still counts. */
	CHECK(sysinfo_parse_stat(stat0, u, 2) == 2, "sysinfo: max cores");
	memset(u, 0, sizeof(u));
	sysinfo_parse_stat(stat1, u, 4);
	CHECK(sysinfo_usage(&t[0], &u[0]) == 50 && sysinfo_usage(&t[1], &u[1]) == 0 &&
	      sysinfo_usage(&t[2], &u[2]) == 100,
	      "sysinfo: usage %d %d %d", sysinfo_usage(&t[0], &u[0]),
	      sysinfo_usage(&t[1], &u[1]), sysinfo_usage(&t[2], &u[2]));
	CHECK(sysinfo_usage(&t[0], &t[0]) == -1, "sysinfo: no time passed");
	CHECK(sysinfo_meminfo_kb("MemTotal:     1003456 kB\nMemFree: 1 kB\n"
				 "MemAvailable:   803456 kB\n", "MemAvailable") ==
	      803456, "sysinfo: MemAvailable");
	CHECK(sysinfo_meminfo_kb("MemTotal: 5 kB\n", "Mem") == -1 &&
	      sysinfo_meminfo_kb("MemTotal: 5 kB\n", "MemAvailable") == -1,
	      "sysinfo: missing meminfo key");

	snprintf(root, sizeof(root), "%s/sysinfo", dir);
	mkdir(root, 0755);
	put_path(root, "proc/stat", stat0);
	put_path(root, "proc/meminfo", "MemTotal: 1003520 kB\n"
		 "MemAvailable: 798720 kB\n");
	put_path(root, "sys/class/thermal/thermal_zone0/type", "soc-thermal\n");
	put_path(root, "sys/class/thermal/thermal_zone0/temp", "45200\n");
	put_path(root, "sys/class/thermal/thermal_zone1/type", "gpu-thermal\n");
	put_path(root, "sys/class/thermal/thermal_zone1/temp", "44800\n");
	put_path(root, "sys/class/thermal/cooling_device0/type", "cpufreq\n");
	put_path(root, "sys/class/hwmon/hwmon0/name", "batt\n");
	put_path(root, "sys/class/hwmon/hwmon0/temp1_input", "30000\n");
	put_path(root, "sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq",
		 "1296000\n");
	sysinfo_set_root(root);
	sysinfo_page.enter("selftest", 0);
	draw_page(&sysinfo_page, &cv, NULL);
	/* CPU, Cores, Freq, Memory, Temp rows (the temperatures wrap). */
	CHECK(SI_SHOWS(0, " 20%  Adaptive"), "sysinfo: CPU since boot");
	CHECK(SI_SHOWS(1, " 20%   20%"), "sysinfo: cores since boot");
	CHECK(SI_SHOWS(2, "1296 MHz"), "sysinfo: frequency");
	CHECK(SI_SHOWS(3, "200 / 980 MB"), "sysinfo: memory");
	CHECK(SI_SHOWS(4, "soc 45.2C  gpu 44.8C"),
	      "sysinfo: thermal zones");
	CHECK(SI_SHOWS(5, "batt 30.0C"), "sysinfo: hwmon wraps");
	put_path(root, "proc/stat", stat1);
	CHECK(sysinfo_page.update(0, 500000000LL).act == UI_NONE,
	      "sysinfo: stays open");
	draw_page(&sysinfo_page, &cv, NULL);
	CHECK(SI_SHOWS(0, " 20%  Adaptive"),
	      "sysinfo: not refreshed before 1 s");
	sysinfo_page.update(0, 1000000000LL);
	draw_page(&sysinfo_page, &cv, NULL);
	CHECK(SI_SHOWS(0, " 50%  Adaptive") &&
	      SI_SHOWS(1, "  0%  100%"), "sysinfo: usage from deltas");
	snprintf(path, sizeof(path), "%s/ui-sysinfo.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	CHECK(sysinfo_page.update(JOY_BIT(B), 1100000000LL).act == UI_BACK,
	      "sysinfo: B goes back");
	sysinfo_set_root(NULL);
}

/* The system clock setter for test_clock: records, fails with fake_err. */
static struct timeval fake_tv;
static int fake_calls, fake_err;

static int fake_settime(const struct timeval *tv)
{
	fake_calls++;
	fake_tv = *tv;
	if (fake_err) {
		errno = fake_err;
		return -1;
	}
	return 0;
}

/* Date & time page: the field (0 year .. 4 minute) to value by up. */
static void dt_set(int field, int value)
{
	struct tm tm;

	for (int i = 0; i < 5 && datetime_field() != field; i++)
		dt_key(JOY_BIT(RIGHT));
	for (int i = 0; i < 100; i++) {
		int v;

		datetime_value(&tm);
		v = field == 0 ? tm.tm_year + 1900 : field == 1 ? tm.tm_mon + 1 :
		    field == 2 ? tm.tm_mday : field == 3 ? tm.tm_hour : tm.tm_min;
		if (v == value)
			return;
		dt_key(JOY_BIT(UP));
	}
	CHECK(0, "clock: field %d never reached %d", field, value);
}

static int dt_is(int y, int mo, int d, int h, int mi)
{
	struct tm tm;

	datetime_value(&tm);
	return tm.tm_year + 1900 == y && tm.tm_mon + 1 == mo && tm.tm_mday == d &&
	       tm.tm_hour == h && tm.tm_min == mi;
}

/*
 * Date & time (clock.c) with a regular file as /dev/rtc0 and a fake
 * settimeofday, so the build host's clock is never touched: month
 * lengths, field navigation, wrapping and day clamping, cancel, and saves
 * where the RTC ioctl (ENOTTY on a file), a missing RTC or the system
 * clock fail, each logged and shown on the page.
 */
static void test_clock(const char *dir)
{
	static uint16_t px[480 * 320];
	static char log[65536];
	struct stat sb;
	struct canvas cv = { px, 480, 320 };
	char rtc[300], logp[300], missing[300], msg[400];
	struct tm tm;
	time_t want;
	int n, red = 0, start;

	CHECK(clock_days_in_month(2024, 2) == 29 &&
	      clock_days_in_month(2000, 2) == 29 &&
	      clock_days_in_month(2100, 2) == 28 &&
	      clock_days_in_month(2026, 2) == 28 &&
	      clock_days_in_month(2026, 4) == 30 &&
	      clock_days_in_month(2026, 12) == 31, "clock: month lengths");

	/* Everything from here on also goes to clock-log.txt, after the
	 * lines kept in memory so far. */
	snprintf(logp, sizeof(logp), "%s/clock-log.txt", dir);
	log_open_file(logp);
	start = stat(logp, &sb) ? 0 : (int)sb.st_size;
	snprintf(rtc, sizeof(rtc), "%s/rtc0", dir);
	write_file_atomic(rtc, "", 0);
	snprintf(missing, sizeof(missing), "%s/no-rtc", dir);
	clock_set_backend(rtc, fake_settime);
	clock_boot_log();

	datetime_page.enter(NULL, 0);
	CHECK(datetime_field() == 0 && !datetime_message()[0], "clock: enter");
	dt_key(JOY_BIT(LEFT));
	CHECK(datetime_field() == 4, "clock: left wraps to the minute");
	dt_key(JOY_BIT(RIGHT));
	CHECK(datetime_field() == 0, "clock: right wraps to the year");
	dt_set(0, 2024);
	dt_set(1, 1);
	dt_set(2, 31);
	dt_set(3, 23);
	dt_set(4, 0);
	CHECK(dt_is(2024, 1, 31, 23, 0), "clock: fields set");
	/* Jan 31 -> Feb 29 in a leap year -> Feb 28 the year after. */
	dt_key(JOY_BIT(LEFT));
	dt_key(JOY_BIT(LEFT));
	dt_key(JOY_BIT(LEFT));
	CHECK(datetime_field() == 1, "clock: on the month");
	dt_key(JOY_BIT(UP));
	CHECK(dt_is(2024, 2, 29, 23, 0), "clock: Feb 29 clamped in 2024");
	dt_key(JOY_BIT(LEFT));
	dt_key(JOY_BIT(UP));
	CHECK(dt_is(2025, 2, 28, 23, 0), "clock: Feb 28 in 2025");
	/* Wrapping: year, month, day in its month, hour, minute. */
	dt_key(JOY_BIT(DOWN));
	dt_key(JOY_BIT(DOWN));
	CHECK(dt_is(2099, 2, 28, 23, 0), "clock: year wraps down");
	dt_key(JOY_BIT(UP));
	CHECK(dt_is(2024, 2, 28, 23, 0), "clock: year wraps up");
	dt_set(2, 29);
	dt_key(JOY_BIT(UP));
	CHECK(dt_is(2024, 2, 1, 23, 0), "clock: day wraps after the 29th");
	dt_key(JOY_BIT(DOWN));
	CHECK(dt_is(2024, 2, 29, 23, 0), "clock: day wraps down");
	dt_key(JOY_BIT(LEFT));
	dt_key(JOY_BIT(DOWN));
	CHECK(dt_is(2024, 1, 29, 23, 0), "clock: month down");
	dt_key(JOY_BIT(DOWN));
	CHECK(dt_is(2024, 12, 29, 23, 0), "clock: month wraps down");
	dt_key(JOY_BIT(UP));
	CHECK(dt_is(2024, 1, 29, 23, 0), "clock: month wraps up");
	dt_key(JOY_BIT(RIGHT));
	dt_key(JOY_BIT(RIGHT));
	dt_key(JOY_BIT(UP));
	CHECK(dt_is(2024, 1, 29, 0, 0), "clock: hour wraps");
	dt_key(JOY_BIT(RIGHT));
	dt_key(JOY_BIT(DOWN));
	CHECK(dt_is(2024, 1, 29, 0, 59), "clock: minute wraps");
	/* B cancels: nothing set. */
	CHECK(dt_key(JOY_BIT(B)) == UI_BACK && fake_calls == 0,
	      "clock: cancel sets nothing");

	/* A saves: the system clock gets the time with 0 seconds, the RTC
	 * ioctl fails on a regular file, the page stays with the message. */
	datetime_page.enter(NULL, 0);
	dt_set(0, 2028);
	dt_set(1, 2);
	dt_set(2, 29);
	dt_set(3, 13);
	dt_set(4, 45);
	CHECK(dt_key(JOY_BIT(A)) == UI_NONE, "clock: failed save stays");
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 2028 - 1900;
	tm.tm_mon = 1;
	tm.tm_mday = 29;
	tm.tm_hour = 13;
	tm.tm_min = 45;
	want = timegm(&tm);
	CHECK(fake_calls == 1 && fake_tv.tv_sec == want && fake_tv.tv_usec == 0,
	      "clock: settimeofday got %lld, want %lld (%d calls)",
	      (long long)fake_tv.tv_sec, (long long)want, fake_calls);
	CHECK(!strncmp(datetime_message(), "Not saved to the RTC:\n", 22),
	      "clock: message %s", datetime_message());
	draw_page(&datetime_page, &cv, NULL);
	for (int y = cv.h - 76; y < cv.h - 36; y++)
		for (int x = 0; x < cv.w; x++)
			red += px[y * cv.w + x] == COL_ERR;
	CHECK(red > 100, "clock: message not drawn (%d red pixels)", red);
	snprintf(msg, sizeof(msg), "%s/ui-datetime.raw", dir);
	write_file_atomic(msg, px, sizeof(px));
	dt_key(JOY_BIT(UP));
	CHECK(!datetime_message()[0], "clock: an edit clears the message");
	CHECK(dt_key(JOY_BIT(B)) == UI_BACK, "clock: B after a failure");

	/* No RTC device, then the system clock failing too. */
	clock_set_backend(missing, fake_settime);
	CHECK(clock_set(&tm, msg, sizeof(msg)) == ENOENT &&
	      !strncmp(msg, "Not saved to the RTC:\n", 22), "clock: no RTC: %s",
	      msg);
	fake_err = EPERM;
	CHECK(clock_set(&tm, msg, sizeof(msg)) == EPERM &&
	      !strncmp(msg, "Clock and RTC not set:\n", 23),
	      "clock: both failing: %s", msg);
	fake_err = 0;

	n = read_file(logp, log, sizeof(log) - 1);
	log[n > 0 ? n : 0] = 0;
	CHECK(n > start, "clock: log %d bytes, %d before the test", n, start);
	memmove(log, log + (n > start ? start : 0),
		(size_t)(n > start ? n - start + 1 : 1));
	CHECK(strstr(log, "clock: system ") && strstr(log, ", RTC unreadable ("),
	      "clock: boot line");
	CHECK(strstr(log, "clock: setting 2028-02-29 13:45:00 UTC; system clock "
		     "was ") != NULL, "clock: set line");
	snprintf(msg, sizeof(msg), "clock: settimeofday(%lld): ok", (long long)want);
	CHECK(strstr(log, msg) != NULL, "clock: settimeofday line");
	CHECK(strstr(log, "clock: settimeofday(") &&
	      strstr(log, strerror(EPERM)), "clock: settimeofday failure logged");
	snprintf(msg, sizeof(msg), "clock: RTC_SET_TIME on %s: %s", rtc,
		 strerror(ENOTTY));
	CHECK(strstr(log, msg) != NULL, "clock: RTC failure logged");
	snprintf(msg, sizeof(msg), "clock: RTC_SET_TIME on %s: %s", missing,
		 strerror(ENOENT));
	CHECK(strstr(log, msg) != NULL, "clock: missing RTC logged");
	CHECK(strstr(log, "clock: date & time page closed without saving") != NULL,
	      "clock: cancel logged");
	clock_set_backend(NULL, NULL);
}

/* Render the menus to <dir>/ui-*.raw (480x320 RGB565) for a visual check. */
static void test_ui(const char *dir)
{
	static uint16_t px[480 * 320], game[240 * 160];
	struct canvas cv = { px, 480, 320 };
	char path[300];

	for (int i = 0; i < 240 * 160; i++)
		game[i] = (uint16_t)(i * 37);
	romlist_page.enter(NULL, 0);
	page_key(&romlist_page, JOY_BIT(DOWN));
	draw_page(&romlist_page, &cv, NULL);
	snprintf(path, sizeof(path), "%s/ui-romlist.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	ig_open("Some Long Game Name (USA, Europe).gba");
	ig_key(1u << 5);
	draw_page(&ingame_page, &cv, game);
	snprintf(path, sizeof(path), "%s/ui-ingame.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	/* Frameskip (the 7th row): with mGBA, Auto is stepped over, and a
	 * game set to Auto reads "Auto: gpSP only". */
	rom_settings.core = CORE_MGBA;
	for (int i = 0; i < 5; i++)
		ig_key(JOY_BIT(DOWN));
	ig_key(JOY_BIT(A));
	CHECK(rom_settings.frameskip == FRAMESKIP_1,
	      "frameskip with mGBA: off -> %d", rom_settings.frameskip);
	ig_key(JOY_BIT(LEFT));
	CHECK(rom_settings.frameskip == FRAMESKIP_OFF,
	      "frameskip with mGBA: 1 -> %d", rom_settings.frameskip);
	ig_key(JOY_BIT(LEFT));
	CHECK(rom_settings.frameskip == FRAMESKIP_3,
	      "frameskip with mGBA wraps: off -> %d", rom_settings.frameskip);
	rom_settings.frameskip = FRAMESKIP_AUTO;
	ig_key(JOY_BIT(RIGHT));
	CHECK(rom_settings.frameskip == FRAMESKIP_1,
	      "frameskip with mGBA: auto -> %d", rom_settings.frameskip);
	rom_settings.frameskip = FRAMESKIP_AUTO;
	draw_page(&ingame_page, &cv, game);
	snprintf(path, sizeof(path), "%s/ui-ingame-mgba.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	CHECK(row_shows_on(px, IG_BOX_X + IG_BOX_W - FONT_W -
			   draw_text_width("Auto: gpSP only"),
			   IG_BOX_Y + IG_HEAD_H + 6 * IG_ROW_H, "Auto: gpSP only",
			   COL_DIM, COL_SEL),
	      "frameskip with mGBA: Auto reads gpSP only");
	rom_settings.core = CORE_GPSP;
	rom_settings.frameskip = FRAMESKIP_OFF;
	ig_key(JOY_BIT(A));
	CHECK(rom_settings.frameskip == FRAMESKIP_AUTO,
	      "frameskip with gpSP: off -> %d", rom_settings.frameskip);
	rom_settings.frameskip = FRAMESKIP_OFF;
	/* Frame blend, the in-game settings' 2nd row: with mGBA it reads
	 * "gpSP only" and does not change. */
	rom_settings.core = CORE_MGBA;
	game_settings_page.enter(NULL, 0);
	st_key(JOY_BIT(DOWN));
	st_key(JOY_BIT(A));
	st_key(JOY_BIT(RIGHT));
	CHECK(rom_settings.blend == 0, "blend unchanged with mGBA");
	rom_settings.core = CORE_GPSP;
	st_key(JOY_BIT(A));
	CHECK(rom_settings.blend == 1, "blend toggled with gpSP");
	rom_settings.blend = 0;

	settings_page.enter(NULL, 0);
	st_key(1u << 5);
	draw_page(&settings_page, &cv, NULL);
	snprintf(path, sizeof(path), "%s/ui-settings.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	/* Volume, Brightness, CPU, Audio, Debug log, Date & time, System
	 * info, Resume on boot, Power off: no grid rows outside a game, no
	 * Back row (a full page, B goes back). */
	st_key(JOY_BIT(DOWN));
	st_key(JOY_BIT(DOWN));
	st_key(JOY_BIT(A));
	CHECK(settings.audio == AUDIO_SIMPLE, "settings: 4th row is Audio");
	settings.audio = settings_defaults.audio;
	st_key(JOY_BIT(DOWN));
	st_key(JOY_BIT(DOWN));
	CHECK(st_key(JOY_BIT(A)) == UI_PUSH && opened == &datetime_page,
	      "settings: 6th row is Date & time");
	st_key(JOY_BIT(DOWN));
	CHECK(st_key(JOY_BIT(A)) == UI_PUSH && opened == &sysinfo_page,
	      "settings: 7th row is System info");
	st_key(JOY_BIT(DOWN));
	CHECK(st_key(JOY_BIT(A)) == UI_NONE && settings.resume == 0,
	      "settings: 8th row is Resume on boot");
	CHECK(st_key(JOY_BIT(RIGHT)) == UI_NONE && settings.resume == 1,
	      "settings: Resume on boot back on");
	st_key(JOY_BIT(DOWN));
	CHECK(st_key(JOY_BIT(A)) == UI_NONE,
	      "settings: 9th row is Power off (asks first)");
	st_key(JOY_BIT(DOWN));
	CHECK(st_key(JOY_BIT(B)) == UI_BACK,
	      "settings: wraps to Volume, B goes back");
	game_settings_page.enter(NULL, 0);
	for (int i = 0; i < 6; i++)
		st_key(JOY_BIT(DOWN));
	CHECK(st_key(JOY_BIT(A)) == UI_BACK,
	      "in-game settings: 7th row is Back");

	/* In game: a picture with detail, GBA colors and the grid. */
	for (int y = 0; y < 160; y++)
		for (int x = 0; x < 240; x++) {
			int r = x * 31 / 239, g = y * 31 / 159, b = (x ^ y) & 31;

			game[y * 240 + x] = (uint16_t)(r << 11 | g << 6 | b);
		}
	rom_settings.color = COLOR_GBA;
	settings.grid = 1;
	fx_update();
	game_settings_page.enter(NULL, 0);
	draw_page(&game_settings_page, &cv, game);
	snprintf(path, sizeof(path), "%s/ui-settings-ingame.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	settings = settings_defaults;
	rom_settings.color = COLOR_OFF;
	fx_update();
	message_draw(&cv, "No GBA partition", "Could not mount /dev/mmcblk0p3 "
		     "(FAT). ROMs, saves and logs live there.", COL_ERR);
	snprintf(path, sizeof(path), "%s/ui-fatal.raw", dir);
	write_file_atomic(path, px, sizeof(px));
}

/*
 * The page stack through ui_frame, as main.c's frame() runs it: ROM list,
 * SELECT to the settings, System info, B, B; then a stand-in game with
 * SELECT+START to the in-game menu, its Settings, B, B to resume, and
 * SELECT+START from the in-game settings and from System info resumes too
 * (but not over the ROM list). Every press is released for a frame.
 */
static int pg_acts[8], pg_nacts;
static const struct page *pg_from;

static void pg_handler(enum ui_action act, const struct page *from)
{
	if (pg_nacts < (int)ARRAY_SIZE(pg_acts))
		pg_acts[pg_nacts++] = act;
	pg_from = from;
}

static struct ui_result pg_game_update(uint32_t held, int64_t now)
{
	(void)held;
	(void)now;
	return ui_hotkey() ? UI_OPEN(&ingame_page) : UI_STAY;
}

static void test_pages(void)
{
	static const struct page game = {
		"game", NULL, pg_game_update, NULL, 1,
	};
	static uint16_t px[480 * 320];
	struct canvas cv = { px, 480, 320 };
	const uint32_t hot = JOY_BIT(SELECT) | JOY_BIT(START);
	int64_t t = 0;
	int drawn, idle = 1;

#define PRESS(b, n) do { \
	for (int i_ = 0; i_ < (n); i_++) { \
		drawn = ui_frame(&cv, (b), t += 20000000); \
		/* Not drawn again, but for the clocks shown. */ \
		idle &= !ui_frame(&cv, 0, t += 20000000) || \
			ui_top() == &sysinfo_page || \
			ui_top() == &romlist_page; \
	} \
} while (0)

	ui_set_handler(pg_handler);
	ui_reset(&romlist_page, t);
	pg_nacts = 0;
	PRESS(JOY_BIT(SELECT), 1);
	CHECK(ui_top() == &settings_page && drawn && pg_nacts == 1 &&
	      pg_acts[0] == UI_PUSH && pg_from == &romlist_page,
	      "pages: SELECT opens the settings (%s)", ui_top()->name);
	PRESS(JOY_BIT(DOWN), 6);
	PRESS(JOY_BIT(A), 1);
	CHECK(ui_top() == &sysinfo_page && !ui_in_game(),
	      "pages: settings to system info (%s)", ui_top()->name);
	PRESS(hot, 1);
	CHECK(ui_top() == &sysinfo_page, "pages: no game to resume");
	PRESS(JOY_BIT(B), 1);
	CHECK(ui_top() == &settings_page && pg_acts[pg_nacts - 1] == UI_BACK &&
	      pg_from == &sysinfo_page, "pages: B back to the settings");
	PRESS(JOY_BIT(B), 1);
	CHECK(ui_top() == &romlist_page && drawn &&
	      pg_acts[pg_nacts - 1] == UI_BACK && pg_from == &settings_page,
	      "pages: B back to the ROM list (%s)", ui_top()->name);

	ig_open("Game");
	ui_reset(&game, t);
	PRESS(hot, 1);
	CHECK(ui_top() == &ingame_page && drawn && ui_in_game(),
	      "pages: SELECT+START opens the in-game menu (%s)",
	      ui_top()->name);
	PRESS(JOY_BIT(DOWN), 7);
	PRESS(JOY_BIT(A), 1);
	CHECK(ui_top() == &game_settings_page && drawn,
	      "pages: in-game settings (%s)", ui_top()->name);
	PRESS(JOY_BIT(B), 1);
	CHECK(ui_top() == &ingame_page, "pages: B back to the in-game menu");
	pg_nacts = 0;
	PRESS(JOY_BIT(B), 1);
	CHECK(ui_top() == &game && !drawn && pg_nacts == 1 &&
	      pg_acts[0] == UI_RESUME && pg_from == &ingame_page,
	      "pages: B resumes (%s)", ui_top()->name);
	PRESS(hot, 1);
	PRESS(JOY_BIT(DOWN), 7);
	PRESS(JOY_BIT(A), 1);
	PRESS(hot, 1);
	CHECK(ui_top() == &game && pg_acts[pg_nacts - 1] == UI_RESUME &&
	      pg_from == &game_settings_page,
	      "pages: SELECT+START resumes from the in-game settings (%s)",
	      ui_top()->name);
	PRESS(hot, 1);
	PRESS(JOY_BIT(DOWN), 8);
	PRESS(JOY_BIT(A), 1);
	CHECK(ui_top() == &sysinfo_page, "pages: in-game system info");
	PRESS(hot, 1);
	CHECK(ui_top() == &game && pg_from == &sysinfo_page,
	      "pages: SELECT+START resumes from system info (%s)",
	      ui_top()->name);
	CHECK(idle, "pages: a release 20 ms after a press drew again");
#undef PRESS
	ui_set_handler(NULL);
}

/*
 * Redraws only on a change, through ui_frame like main.c's loop. A frame
 * drawn is on screen a vblank (RD_VBLANK_NS) later and ui_frame runs
 * again then; one not drawn sleeps until ui_due (rounded up to a ms, at
 * most 1 s) or the next simulated key. rd_run does that from *t to end
 * with the buttons held, counting redraws and updates.
 */
#define RD_VBLANK_NS 16661000LL
#define RD_MS 1000000LL

static unsigned rd_updates;

static int rd_run(struct canvas *cv, int64_t *t, int64_t end, uint32_t held)
{
	int draws = 0;

	while (*t < end) {
		int drew = ui_frame(cv, held, *t);
		int64_t due = ui_due(), next;

		rd_updates++;
		draws += drew;
		if (drew)
			next = *t + RD_VBLANK_NS;
		else if (due >= *t + 1000 * RD_MS)
			next = *t + 1000 * RD_MS;
		else
			next = *t + (due > *t ? (due - *t + RD_MS - 1) / RD_MS : 1) *
			       RD_MS;
		*t = next < end ? next : end;
	}
	return draws;
}

/* A press for a vblank, then released: 1 when the press was drawn. */
static int rd_press(struct canvas *cv, int64_t *t, uint32_t b)
{
	int drew = ui_frame(cv, b, *t);

	*t += RD_VBLANK_NS;
	ui_frame(cv, 0, *t);
	*t += RD_VBLANK_NS;
	return drew;
}

static void test_redraw(const char *base)
{
	static uint16_t px[480 * 320], ref[480 * 320];
	struct canvas cv = { px, 480, 320 };
	static const int64_t at[] = { 500, 1300, 2500, 4000, 6100, 9000 };
	static const int64_t rates[] = { RD_VBLANK_NS, 4166667, 33333333 };
	const struct romdb_entry *vc =
		entry_named("Advance Wars (USA) (Virtual Console)");
	const int64_t s = 1000 * RD_MS;
	char path[400], dir[340];
	int64_t t = 1000 * s, t0, armed;
	struct tm tm;
	int draws, clock = realtime_utc(&tm), bad = 0, moved = 0, scroll;

	ui_set_handler(pg_handler);
	/* Idle settings: no redraw in 10 s of vblanks, nothing to wake for;
	 * one for a key press, none for its release or a key that is held
	 * (but for the d-pad's key repeat, 350 ms then every 70 ms). */
	ui_reset(&settings_page, t);
	CHECK(ui_frame(&cv, 0, t += RD_VBLANK_NS), "redraw: a new page");
	draws = 0;
	for (int i = 0; i < 600; i++)
		draws += ui_frame(&cv, 0, t += RD_VBLANK_NS);
	CHECK(draws == 0 && ui_due() == INT64_MAX,
	      "redraw: idle settings drew %d times in 600 frames", draws);
	CHECK(ui_frame(&cv, JOY_BIT(DOWN), t += RD_VBLANK_NS) == 1 &&
	      ui_frame(&cv, JOY_BIT(DOWN), t += RD_VBLANK_NS) == 0 &&
	      ui_frame(&cv, 0, t += RD_VBLANK_NS) == 0,
	      "redraw: press drawn once");
	t0 = t;
	ui_frame(&cv, JOY_BIT(DOWN), t);
	CHECK(ui_due() == t0 + 350 * RD_MS, "redraw: woken for the key repeat");
	t += RD_VBLANK_NS;
	draws = rd_run(&cv, &t, t0 + s, JOY_BIT(DOWN));
	/* Repeats at 350, 420 ... 980 ms. */
	CHECK(draws == 10, "redraw: %d repeats drawn in 1 s held", draws);
	ui_frame(&cv, 0, t);
	CHECK(ui_due() == INT64_MAX, "redraw: released, nothing due");

	/* Power off armed: drawn so, then nothing (but a wake a second, the
	 * longest sleep) until 3 s later, when the label is drawn back and
	 * the next A only arms it again. */
	ui_reset(&settings_page, t);
	ui_frame(&cv, 0, t);
	for (int i = 0; i < 8; i++)
		rd_press(&cv, &t, JOY_BIT(DOWN));
	armed = t;
	CHECK(rd_press(&cv, &t, JOY_BIT(A)), "redraw: armed drawn");
	CHECK(ui_due() == armed + 3 * s, "redraw: woken when the arming ends");
	rd_updates = 0;
	draws = rd_run(&cv, &t, armed + 3 * s, 0);
	CHECK(draws == 0 && rd_updates <= 4,
	      "redraw: armed for 3 s: %d redraws, %u updates", draws,
	      rd_updates);
	CHECK(ui_frame(&cv, 0, t) == 1, "redraw: disarmed after 3 s, drawn");
	pg_nacts = 0;
	rd_press(&cv, &t, JOY_BIT(A));
	CHECK(pg_nacts == 0, "redraw: A after the timeout only arms");
	t += 3 * s;

	/* The in-game menu's long name scrolls at the same pace whether
	 * ui_frame runs on every vblank, at 240 Hz or at 30 Hz: the frames
	 * drawn at the same times match. Drawn on every vblank only while it
	 * moves, idle in the 1 s pauses. */
	ingame_menu_set_name("A Very Long Game Name That Will Not Fit (USA, "
			     "Europe) (Rev 1).gba");
	for (size_t k = 0; k < ARRAY_SIZE(at); k++) {
		for (size_t r = 0; r < ARRAY_SIZE(rates); r++) {
			int64_t end;

			ui_reset(&ingame_page, t0 = t);
			end = t0 + at[k] * RD_MS;
			if (!r) {
				rd_run(&cv, &t, end, 0);
			} else {
				while (t < end) {
					ui_frame(&cv, 0, t);
					t = t + rates[r] < end ? t + rates[r] : end;
				}
			}
			ui_redraw();
			ui_frame(&cv, 0, t);
			if (!r) {
				/* It moved since the last time. */
				moved += k && memcmp(ref, px, sizeof(px));
				memcpy(ref, px, sizeof(px));
			} else {
				bad += !!memcmp(ref, px, sizeof(px));
			}
			t += s;
		}
	}
	CHECK(!bad && moved == (int)ARRAY_SIZE(at) - 1,
	      "redraw: marquee pace depends on the update rate (%d, moved %d)",
	      bad, moved);
	ui_reset(&ingame_page, t0 = t);
	rd_updates = 0;
	draws = rd_run(&cv, &t, t0 + 990 * RD_MS, 0);
	CHECK(draws == 1 && rd_updates <= 3,
	      "redraw: marquee pause: %d redraws, %u updates", draws, rd_updates);
	t = t0 + 1100 * RD_MS;
	ui_frame(&cv, 0, t);
	scroll = draws = rd_run(&cv, &t, t0 + 1600 * RD_MS, 0);
	CHECK(draws >= 29 && draws <= 31,
	      "redraw: marquee: %d redraws in 0.5 s of scrolling", draws);
	t += s;

	/* System info: each sample and each second of the clock and of the
	 * uptime, up to 3 a second; Date & time: each second. */
	ui_reset(&sysinfo_page, t0 = t);
	rd_updates = 0;
	draws = rd_run(&cv, &t, t0 + 3 * s, 0);
	CHECK(draws >= 4 && draws <= 10 && rd_updates < 40,
	      "redraw: system info: %d redraws, %u updates in 3 s", draws,
	      rd_updates);
	log_msg("selftest: redraw: idle menus 0 redraws in 10 s, marquee %d in "
		"0.5 s of scrolling, system info %d in 3 s (%u updates)", scroll,
		draws, rd_updates);
	ui_reset(&datetime_page, t0 = t);
	draws = rd_run(&cv, &t, t0 + 3 * s, 0);
	CHECK(draws >= 3 && draws <= 4,
	      "redraw: date & time: %d redraws in 3 s", draws);

	/* The ROM list: idle but for the clock in the header (once a minute
	 * when set). Opening crcwait/ (the 2nd entry) with a ROM waiting for
	 * its CRC: woken within 8 ms for it, drawn once it has its name. */
	snprintf(dir, sizeof(dir), "%s/roms/crcwait", base);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/aw.gba", dir);
	write_rom(path, "AWRE", 0, vc ? vc->crc : 0, 7);
	ui_reset(&romlist_page, t);
	ui_frame(&cv, 0, t);
	draws = 0;
	for (int i = 0; i < 600; i++)
		draws += ui_frame(&cv, 0, t += RD_VBLANK_NS);
	CHECK(draws <= clock, "redraw: idle ROM list drew %d times", draws);
	rd_press(&cv, &t, JOY_BIT(LEFT));
	rd_press(&cv, &t, JOY_BIT(DOWN));
	CHECK(ui_frame(&cv, JOY_BIT(A), t) && ui_due() <= t + 8 * RD_MS,
	      "redraw: folder with a CRC to compute opened");
	t += RD_VBLANK_NS;
	draws = rd_run(&cv, &t, t + 100 * RD_MS, 0);
	CHECK(draws == 1 && ui_due() > t + 8 * RD_MS,
	      "redraw: CRC done and drawn (%d redraws)", draws);
	rd_press(&cv, &t, JOY_BIT(B));
	unlink(path);
	rmdir(dir);
	romdb_cache_reset();
	ui_set_handler(NULL);
}

/* Per boot log directories: numbered, the newest 20 kept. */
static void test_log_dirs(const char *base)
{
	char data[200], path[500], keep_data[256], keep_log[300], buf[32];
	unsigned n = 0;
	int count = 0, has_first = 0, has_last = 0;
	DIR *d;
	struct dirent *e;

	snprintf(keep_data, sizeof(keep_data), "%s", fe_data_dir);
	snprintf(keep_log, sizeof(keep_log), "%s", fe_log_dir);
	snprintf(data, sizeof(data), "%s/logtest", base);
	mkdir(data, 0755);
	snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", data);
	sys_make_dirs();
	for (int i = 0; i < 25; i++) {
		n = sys_log_init();
		snprintf(path, sizeof(path), "%s/frontend.txt", fe_log_dir);
		write_file_atomic(path, "x\n", 2);
	}
	CHECK(n == 25, "boot counter %u", n);
	snprintf(path, sizeof(path), "%s/log", data);
	d = opendir(path);
	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "boot-0", 6))
			continue;
		count++;
		has_first |= !strcmp(e->d_name, "boot-0006");
		has_last |= !strcmp(e->d_name, "boot-0025");
		CHECK(strcmp(e->d_name, "boot-0005") != 0, "boot-0005 kept");
	}
	if (d)
		closedir(d);
	CHECK(count == 20 && has_first && has_last, "%d boot dirs", count);
	snprintf(path, sizeof(path), "%s/log/LATEST.txt", data);
	CHECK(read_file(path, buf, sizeof(buf)) == 10 && !strncmp(buf, "boot-0025", 9),
	      "LATEST.txt");
	snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", keep_data);
	snprintf(fe_log_dir, sizeof(fe_log_dir), "%s", keep_log);
}


/* Power key timing: short press sleeps, holding powers off, the press
 * and release around a wake start nothing. */
static void test_pwrkey(void)
{
	const int64_t ms = 1000000;
	struct pwrkey k = { 0 };
	int64_t t = 1000 * ms;

	CHECK(pwrkey_event(&k, 1, t) == PWR_NONE, "press alone does nothing");
	CHECK(pwrkey_poll(&k, t + 500 * ms) == PWR_NONE, "held 0.5 s");
	CHECK(pwrkey_event(&k, 0, t + 600 * ms) == PWR_SHORT, "short press");
	CHECK(pwrkey_event(&k, 0, t + 700 * ms) == PWR_NONE, "double release");

	/* Long press fires while held, once; its release is nothing. */
	t += 5000 * ms;
	pwrkey_event(&k, 1, t);
	CHECK(pwrkey_poll(&k, t + PWRKEY_LONG_NS - ms) == PWR_NONE,
	      "just under the long press");
	CHECK(pwrkey_poll(&k, t + PWRKEY_LONG_NS) == PWR_LONG, "long press");
	CHECK(pwrkey_poll(&k, t + PWRKEY_LONG_NS + 500 * ms) == PWR_NONE,
	      "long press fires once");
	CHECK(pwrkey_event(&k, 0, t + 3000 * ms) == PWR_NONE,
	      "release after a long press");
	/* Without polls, the release still tells long from short. */
	t += 5000 * ms;
	pwrkey_event(&k, 1, t);
	CHECK(pwrkey_event(&k, 0, t + 2500 * ms) == PWR_LONG,
	      "long press seen on release");

	/* Suspend to RAM: the waking press and its release are swallowed,
	 * the next press works. */
	t += 5000 * ms;
	pwrkey_wake(&k, -1, t);
	CHECK(pwrkey_event(&k, 1, t + 50 * ms) == PWR_WAKE_PRESS, "wake press");
	CHECK(pwrkey_poll(&k, t + 2500 * ms) == PWR_NONE,
	      "holding the wake press does not power off");
	CHECK(pwrkey_event(&k, 0, t + 2600 * ms) == PWR_NONE, "wake release");
	pwrkey_event(&k, 1, t + 3000 * ms);
	CHECK(pwrkey_event(&k, 0, t + 3100 * ms) == PWR_SHORT,
	      "next press after a wake");
	/* The waking press never reported: a late press is a new one. */
	t += 5000 * ms;
	pwrkey_wake(&k, -1, t);
	pwrkey_poll(&k, t + PWRKEY_WAKE_NS);
	pwrkey_event(&k, 1, t + PWRKEY_WAKE_NS + 10 * ms);
	CHECK(pwrkey_event(&k, 0, t + PWRKEY_WAKE_NS + 100 * ms) == PWR_SHORT,
	      "press after the swallow window");
	/* Only a release reported: it closes the window. */
	t += 5000 * ms;
	pwrkey_wake(&k, -1, t);
	CHECK(pwrkey_event(&k, 0, t + 20 * ms) == PWR_NONE, "lone release");
	pwrkey_event(&k, 1, t + 200 * ms);
	CHECK(pwrkey_event(&k, 0, t + 300 * ms) == PWR_SHORT,
	      "press after a lone wake release");

	/* Light sleep, woken while the key is down: release is nothing,
	 * holding on does not power off. */
	t += 5000 * ms;
	pwrkey_wake(&k, 1, t);
	CHECK(pwrkey_poll(&k, t + 3000 * ms) == PWR_NONE, "held after light wake");
	CHECK(pwrkey_event(&k, 0, t + 3100 * ms) == PWR_NONE,
	      "release after light wake");
	pwrkey_event(&k, 1, t + 3500 * ms);
	CHECK(pwrkey_event(&k, 0, t + 3600 * ms) == PWR_SHORT,
	      "press after light wake");
	/* Light sleep, key already released: nothing swallowed. */
	pwrkey_wake(&k, 0, t + 4000 * ms);
	pwrkey_event(&k, 1, t + 4100 * ms);
	CHECK(pwrkey_event(&k, 0, t + 4200 * ms) == PWR_SHORT,
	      "quick press after light wake");
	/* Well below the RK817's own long press power off (several
	 * seconds, not measured on this board). */
	CHECK(PWRKEY_LONG_NS <= 3000 * ms, "long press threshold");
}

/* The lit pixel of the grid at each of the four positions. */
static void test_grid_phases(void)
{
	static uint16_t lit[FX_LUT_SIZE], dark[FX_LUT_SIZE];
	static uint16_t frame[256 * 160], a[320 * 480], b[320 * 480];
	static uint16_t px[480 * 320];
	struct canvas cv = { px, 480, 320 };
	int seen[4] = { 0 };
	uint16_t w, d;

	for (int i = 0; i < 256 * 160; i++)
		frame[i] = 0xffdf;
	fx_build(lit, dark, COLOR_OFF, 50, 0);
	w = lit[FX_INDEX(0xffdf)];
	d = dark[FX_INDEX(0xffdf)];
	for (int ph = 0; ph < 4; ph++) {
		const struct blit_fx fx = { lit, dark, ph & 1, ph >> 1 };
		int bad = 0, bad_ref = 0, bad_cv = 0;
		static const int rots[] = { 270, 90, 0 };

		blit_rotate(a, 320, 320, 480, 270, frame, 256, 240, 160, 2, &fx);
		for (int lx = 0; lx < 480; lx++)
			for (int ly = 0; ly < 320; ly++) {
				uint16_t want = lx % 2 == (ph & 1) &&
						ly % 2 == (ph >> 1) ? w : d;

				bad += a[(479 - lx) * 320 + ly] != want;
			}
		CHECK(!bad, "grid phase %d: %d pixels wrong", ph, bad);
		draw_image_fx(&cv, 0, 0, frame, 256, 240, 160, &fx);
		for (int ly = 0; ly < 320; ly++)
			for (int lx = 0; lx < 480; lx++)
				bad_cv += px[ly * 480 + lx] != a[(479 - lx) * 320 + ly];
		CHECK(!bad_cv, "grid phase %d: canvas differs in %d pixels", ph,
		      bad_cv);
		/* Random content, all rotations, scale 2 and 3, against the
		 * reference. */
		for (int i = 0; i < 256 * 160; i++)
			frame[i] = rnd16();
		for (size_t r = 0; r < ARRAY_SIZE(rots); r++)
			for (int sc = 2; sc <= 3; sc++) {
				int nw = rots[r] ? 320 : 480, nh = rots[r] ? 480 : 320;
				int sw = sc == 2 ? 240 : 160, sh = sc == 2 ? 160 : 106;

				blit_rotate(a, nw, nw, nh, rots[r], frame, 256, sw, sh,
					    sc, &fx);
				blit_reference(b, nw, nw, nh, rots[r], frame, 256, sw,
					       sh, sc, &fx);
				bad_ref += memcmp(a, b, (size_t)nw * nh * 2) != 0;
			}
		CHECK(!bad_ref, "grid phase %d: %d cases differ from the "
		      "reference", ph, bad_ref);
		for (int i = 0; i < 256 * 160; i++)
			frame[i] = 0xffdf;
	}

	/* The per boot choice: every position comes up, the active tables
	 * follow it, and the settings still apply. */
	for (uint64_t i = 0; i < 400; i++)
		seen[fx_grid_phase_from_seed(i)]++;
	for (int ph = 0; ph < 4; ph++)
		CHECK(seen[ph] > 50, "grid phase %d picked %d/400 times", ph,
		      seen[ph]);
	fx_init_grid_phase(1);
	CHECK(fx_grid_phase() >= 0 && fx_grid_phase() < 4, "phase range");
	settings.grid = 1;
	settings.grid_dim = 75;
	settings.grid_boost = 30;
	fx_set_grid_phase(3);
	fx_update();
	CHECK(fx_current() && fx_current()->dark && fx_current()->lit_x == 1 &&
	      fx_current()->lit_y == 1, "phase 3 in the active effects");
	CHECK(fx_current()->lit[FX_INDEX(0x8410)] != lit[FX_INDEX(0x8410)],
	      "grid boost still applied");
	fx_set_grid_phase(0);
	settings = settings_defaults;
	fx_update();
}

/* A /sys/power that cannot suspend: the caller must fall back. */
static void test_sleep_sysfs(const char *base)
{
	char dir[300], path[400], stats[256];
	struct sleep_result r;

	sleep_suspend("/sys/power", 1, &r);
	CHECK(!r.slept && r.err == EPERM, "real /sys/power untouched when not PID 1");

	snprintf(dir, sizeof(dir), "%s/power", base);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/suspend_stats", dir);
	mkdir(path, 0755);
	put(dir, "state", "freeze\n");
	sleep_suspend(dir, 2, &r);
	CHECK(!r.slept && r.err == ENOTSUP, "no mem in state");

	put(dir, "state", "freeze mem\n");
	put(dir, "mem_sleep", "[s2idle] deep\n");
	put(dir, "wakeup_count", "42\n");
	put(path, "success", "3\n");
	put(path, "fail", "1\n");
	put(path, "last_failed_dev", "\n");
	CHECK(sleep_stats(dir, stats, sizeof(stats)) == 3 &&
	      !strcmp(stats, "fail=1 last_failed_dev=- success=3"),
	      "suspend_stats: %s", stats);
	sleep_suspend(dir, 3, &r);
	CHECK(!r.slept && !r.err && r.success_delta == 0 && r.attempts == 1,
	      "write returning at once is no suspend (slept %d err %d delta %ld)",
	      r.slept, r.err, r.success_delta);
	CHECK(!strcmp(r.method, "mem (deep)"), "deep selected: %s", r.method);
	snprintf(path, sizeof(path), "%s/state", dir);
	CHECK(read_file(path, stats, sizeof(stats)) == 3 &&
	      !strncmp(stats, "mem", 3), "mem written");
}

/* ------------------------------------------------------ partition grow */

static void mbr_entry(uint8_t *mbr, int i, uint8_t type, uint32_t start,
		      uint32_t size)
{
	uint8_t *e = mbr + 446 + 16 * i;

	e[4] = type;
	for (int k = 0; k < 4; k++) {
		e[8 + k] = start >> (8 * k);
		e[12 + k] = size >> (8 * k);
	}
}

static void write_bytes(const char *path, size_t n, int seed, time_t mtime)
{
	struct timespec ts[2] = { { mtime, 0 }, { mtime, 0 } };
	FILE *f = fopen(path, "w");

	for (size_t i = 0; f && i < n; i++)
		fputc((int)((i * 31 + seed) & 0xff), f);
	if (f)
		fclose(f);
	utimensat(AT_FDCWD, path, ts, 0);
}

static void test_sdgrow(const char *base)
{
	static uint8_t mbr[512];
	static const struct { uint64_t card; uint32_t spc; } geo[] = {
		{ 2097152, 8 },          /* 1 GiB: 4 KiB clusters */
		{ 15523840, 8 },         /* "8 GB" */
		{ 31116288, 16 },        /* "16 GB": 8 KiB */
		{ 62521344, 32 },        /* "32 GB": 16 KiB */
		{ 124735488, 64 },       /* "64 GB": 32 KiB */
		{ 4294967295u, 64 },     /* 2 TiB */
	};
	struct sdgrow_plan p;
	struct fat32_geom g;
	struct sdgrow_tree t;
	char why[64], a[300], b[300], path[400];
	struct stat st;

	CHECK(sdgrow_crc32(0, "123456789", 9) == 0xcbf43926, "crc32");
	CHECK(sdgrow_crc32(sdgrow_crc32(0, "1234", 4), "56789", 5) == 0xcbf43926,
	      "crc32 in parts");

	/* The image's layout (genimage.cfg) on a 32 GB card. */
	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	mbr_entry(mbr, 0, 0x0c, 32768, 65536);
	mbr_entry(mbr, 1, 0x83, 98304, 32768);
	mbr_entry(mbr, 2, 0x0c, 131072, 131072);
	CHECK(sdgrow_plan(mbr, 62521344, &p, why, sizeof(why)) == 0, "plan: %s",
	      why);
	CHECK(p.start == 131072 && p.size == 131072 && p.end == 62521344 / 8192 * 8192 &&
	      p.hdr_lba == p.end - 8, "plan: end %u", p.end);
	CHECK(sdgrow_plan(mbr, 8ull << 32, &p, why, sizeof(why)) == 0 &&
	      p.end == 0xffffe000u, "plan: 2 TiB limit, end %u", p.end);
	CHECK(sdgrow_plan(mbr, 200000, &p, why, sizeof(why)) < 0,
	      "plan: p3 beyond the card");
	mbr_entry(mbr, 3, 0x83, 300000, 1000);
	CHECK(sdgrow_plan(mbr, 62521344, &p, why, sizeof(why)) < 0, "plan: p4");
	mbr_entry(mbr, 3, 0, 0, 0);
	mbr_entry(mbr, 2, 0x83, 131072, 131072);
	CHECK(sdgrow_plan(mbr, 62521344, &p, why, sizeof(why)) < 0, "plan: type");
	mbr_entry(mbr, 2, 0x0c, 65536, 131072);
	CHECK(sdgrow_plan(mbr, 62521344, &p, why, sizeof(why)) < 0,
	      "plan: p3 before p2's end");
	mbr[511] = 0;
	CHECK(sdgrow_plan(mbr, 62521344, &p, why, sizeof(why)) < 0, "plan: sig");

	for (size_t i = 0; i < ARRAY_SIZE(geo); i++) {
		uint32_t end = geo[i].card / 8192 * 8192, size = end - 131072;
		uint64_t fat_need;

		CHECK(fat32_geometry(131072, size, &g) == 0, "geometry %zu", i);
		fat_need = ((uint64_t)g.clusters + 2) * 4;
		CHECK(g.spc == geo[i].spc, "geometry %zu: spc %u", i, g.spc);
		CHECK(g.clusters >= 65541 && g.clusters <= 0x0fffffe5 &&
		      fat_need <= (uint64_t)g.fat_sectors * 512 &&
		      (uint64_t)g.data + (uint64_t)g.clusters * g.spc <= size &&
		      (uint64_t)g.data + (uint64_t)(g.clusters + 1) * g.spc > size &&
		      g.data == g.reserved + 2 * g.fat_sectors && g.reserved >= 32 &&
		      g.reserved <= 0xffff, "geometry %zu: layout", i);
		CHECK((131072 + g.data) % 8192 == 0, "geometry %zu: alignment", i);
	}
	/* 40 MiB: only 512 byte clusters give a FAT32, 20 MiB none. */
	CHECK(fat32_geometry(131072, 40 << 11, &g) == 0 && g.spc == 1,
	      "geometry 40 MiB: spc %u", g.spc);
	CHECK(fat32_geometry(131072, 20 << 11, &g) < 0, "geometry 20 MiB");

	/* Trees: nested directories, empty ones, an empty, a big and a
	 * read-only file, times kept. */
	snprintf(a, sizeof(a), "%s/grow-a", base);
	snprintf(b, sizeof(b), "%s/grow-b", base);
	mkdir(a, 0755);
	mkdir(b, 0755);
	snprintf(path, sizeof(path), "%s/roms", a);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/roms/deep", a);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/empty", a);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/roms/Game (USA).gba", a);
	write_bytes(path, 3 << 20 | 17, 1, 1700000000);
	snprintf(path, sizeof(path), "%s/roms/deep/x.sav", a);
	write_bytes(path, 1000, 2, 1700000002);
	chmod(path, 0444);
	snprintf(path, sizeof(path), "%s/zero", a);
	write_bytes(path, 0, 0, 1600000000);
	snprintf(path, sizeof(path), "%s/roms/deep", a);
	{
		struct timespec ts[2] = { { 1500000000, 0 }, { 1500000000, 0 } };

		utimensat(AT_FDCWD, path, ts, 0);
	}
	CHECK(sdgrow_copy_tree(a, b, &t) == 0, "copy tree");
	CHECK(t.files == 3 && t.dirs == 3 && t.bytes == (3 << 20 | 17) + 1000,
	      "copy tree: %u files %u dirs %llu bytes", t.files, t.dirs,
	      (unsigned long long)t.bytes);
	snprintf(path, sizeof(path), "%s/roms/Game (USA).gba", b);
	CHECK(!stat(path, &st) && st.st_mtime == 1700000000, "copy: mtime");
	snprintf(path, sizeof(path), "%s/roms/deep/x.sav", b);
	CHECK(!stat(path, &st) && !(st.st_mode & 0222), "copy: read-only");
	snprintf(path, sizeof(path), "%s/roms/deep", b);
	CHECK(!stat(path, &st) && st.st_mtime == 1500000000, "copy: dir mtime");
	CHECK(sdgrow_compare_tree(a, b, &t) == 0 && t.files == 3, "compare");
	/* Differences are found: content, size, an extra file, mtime. */
	snprintf(path, sizeof(path), "%s/roms/Game (USA).gba", b);
	chmod(path, 0644);
	write_bytes(path, 3 << 20 | 17, 9, 1700000000);
	CHECK(sdgrow_compare_tree(a, b, &t) < 0, "compare: content");
	write_bytes(path, 3 << 20, 1, 1700000000);
	CHECK(sdgrow_compare_tree(a, b, &t) < 0, "compare: size");
	write_bytes(path, 3 << 20 | 17, 1, 1700000010);
	CHECK(sdgrow_compare_tree(a, b, &t) < 0, "compare: mtime");
	write_bytes(path, 3 << 20 | 17, 1, 1700000000);
	CHECK(sdgrow_compare_tree(a, b, &t) == 0, "compare: same again");
	snprintf(path, sizeof(path), "%s/empty/new", b);
	write_bytes(path, 1, 1, 1700000000);
	CHECK(sdgrow_compare_tree(a, b, &t) < 0, "compare: extra file");
}


/* ---------------------------------------------------------- video core */

/* The video player's play position, from its save state (video_core.c's
 * struct video_state: magic, then the position). */
static double video_pos(void)
{
	const struct core_api *api = core_api_get(CORE_VIDEO);
	uint8_t st[64];
	double pos = -1;

	if (api->serialize_size() <= sizeof(st) &&
	    api->serialize(st, api->serialize_size()))
		memcpy(&pos, st + 8, sizeof(pos));
	return pos;
}

/* n runs with the buttons held for the first; new pictures among them. */
static int video_runs(int n, uint32_t first)
{
	int pics = 0;

	for (int i = 0; i < n; i++) {
		pics += !core_run(i ? 0 : first);
		audio_flush();
	}
	return pics;
}

static int video_load(const char *path, char *err, size_t errlen)
{
	if (core_load(path, CORE_VIDEO, err, errlen))
		return -1;
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	return 0;
}

/*
 * The video player on clips made by mk/frontend.mk with ffmpeg
 * (FE_TEST_VIDEOS:
 * clip.mpg 480x270 30 fps stereo 44.1 kHz 30 s, big.mpg 640x360 25 fps
 * mono 32 kHz 3 s, silent.mpg 240x160 30 fps without audio 2 s): the
 * list, names and file names, the core picked by extension, timing,
 * Fit and Crop switched live (also while paused, and from the in-game
 * menu, which has Picture and no Core for a video), seeking, the resume
 * state, the end and playing again, the downscale and the silence.
 */
static void test_video(const char *base)
{
	static uint16_t px[480 * 320];
	struct canvas cv = { px, 480, 320 };
	const char *dir = getenv("FE_TEST_VIDEOS");
	struct rom_settings keep = rom_settings;
	int keep_scale = settings.video_scale;
	char clip[300], big[300], silent[300], d[300], path[400], err[128];
	struct romlist rl = { 0 };
	struct audio_stats as;
	const uint16_t *fr;
	int w, h, pitch, pics, bh, by;
	double pos;

	if (!dir) {
		log_msg("selftest: video: FE_TEST_VIDEOS not set, skipped");
		return;
	}
	snprintf(clip, sizeof(clip), "%s/clip.mpg", dir);
	snprintf(big, sizeof(big), "%s/big.mpg", dir);
	snprintf(silent, sizeof(silent), "%s/silent.mpg", dir);

	/* Listed with the ROMs, by file name, never looked up. */
	snprintf(d, sizeof(d), "%s/videolist", base);
	mkdir(d, 0755);
	touch(d, "Movie.MPG");
	touch(d, "movie.gba");
	touch(d, "notes.txt");
	CHECK(romlist_scan(&rl, d) == 2, "video list: %d entries", rl.count);
	for (int i = 0; i < rl.count; i++) {
		int vid = romdb_is_video(rl.roms[i].file);

		CHECK(!strcmp(rl.roms[i].id.title, vid ? "Movie" : "movie"),
		      "video list: %s shown as %s", rl.roms[i].file,
		      rl.roms[i].id.title);
		CHECK(!vid || rl.roms[i].id.how == ROMDB_UNKNOWN,
		      "video list: not looked up");
	}
	romlist_free(&rl);
	/* Its own settings and state names, the video core whatever the
	 * game setting says, which never offers the video core. */
	rom_settings.core = CORE_MGBA;
	CHECK(core_for_rom("a/Movie.MPG") == CORE_VIDEO &&
	      core_for_rom("movie.gba") == CORE_MGBA, "video: core by extension");
	core_state_path("x/movie.mpg", CORE_VIDEO, path, sizeof(path));
	CHECK(strstr(path, "/saves/movie.mpg.video.state") != NULL,
	      "video: state path %s", path);
	rom_settings_load("movie.mpg");
	rom_settings.cpu_start = 600;
	rom_settings_save();
	snprintf(path, sizeof(path), "%s/config/roms/movie.mpg.txt", base);
	CHECK(access(path, F_OK) == 0, "video: settings in movie.mpg.txt");
	rom_settings_load("movie.gba");
	CHECK(rom_settings.cpu_start == 0, "video: x.gba keeps its own settings");
	CHECK(rom_settings_parse(&rom_settings, "core=video\n") == 0 &&
	      rom_settings.core != CORE_VIDEO, "video: not a game core setting");
	rom_settings_load(NULL);

	/* Playing: 60 runs per second, a 30 fps clip a picture every other
	 * run, at 1x with Fit. */
	settings.video_scale = VIDEO_FIT;
	audio_init_null(0.001);
	if (video_load(clip, err, sizeof(err))) {
		CHECK(0, "video: %s", err);
		goto out;
	}
	CHECK(core_info()->core == CORE_VIDEO && core_info()->fps == 60.0 &&
	      core_info()->sample_rate == 44100, "video: %.3f fps %.0f Hz",
	      core_info()->fps, core_info()->sample_rate);
	pics = video_runs(600, 0);
	fr = core_frame(&w, &h, &pitch);
	CHECK(fr && w == 480 && h == 270 && pitch == 480,
	      "video: fit %dx%d pitch %d", w, h, pitch);
	CHECK(pics >= 299 && pics <= 302, "video: %d pictures in 600 runs", pics);
	pos = video_pos();
	CHECK(fabs(pos - 10.0) < 0.05, "video: at %.3f s after 600 runs", pos);
	audio_take_stats(&as);
	CHECK(as.xruns == 0, "video: %u underruns", as.xruns);

	/* Crop applied on the next run. */
	settings.video_scale = VIDEO_CROP;
	core_options_changed();
	CHECK(!core_run(0), "video: crop shows a new picture");
	fr = core_frame(&w, &h, &pitch);
	CHECK(fr && w == 480 && h == 320, "video: crop %dx%d", w, h);
	/* Paused (A): the clock stands, repeats, Fit still shows at once. */
	pos = video_pos();
	pics = video_runs(120, JOY_BIT(A));
	CHECK(pics == 0 && fabs(video_pos() - pos) < 0.02,
	      "video: paused: %d pictures, %.3f -> %.3f s", pics, pos,
	      video_pos());
	settings.video_scale = VIDEO_FIT;
	core_options_changed();
	CHECK(!core_run(0), "video: fit while paused shows a new picture");
	fr = core_frame(&w, &h, &pitch);
	CHECK(w == 480 && h == 270, "video: fit while paused %dx%d", w, h);
	video_runs(1, JOY_BIT(A));  /* playing again */

	/* The in-game menu of a video: Picture (row 5) toggles it live. */
	ig_open("clip");
	for (int i = 0; i < 5; i++)
		ig_key(JOY_BIT(DOWN));
	ig_key(JOY_BIT(A));
	CHECK(settings.video_scale == VIDEO_CROP, "video menu: Picture -> crop");
	draw_page(&ingame_page, &cv, NULL);
	bh = IG_BOX_H - (IG_ROWS - 9) * IG_ROW_H;
	by = (320 - bh) / 2;
	CHECK(row_shows_on(px, IG_BOX_X + FONT_W, by + IG_HEAD_H + 5 * IG_ROW_H,
			   "Picture", COL_FG, COL_SEL) &&
	      row_shows_on(px, IG_BOX_X + IG_BOX_W - FONT_W -
			   draw_text_width("Crop"), by + IG_HEAD_H + 5 * IG_ROW_H,
			   "Crop", COL_FG, COL_SEL), "video menu: Picture Crop row");
	CHECK(row_shows_on(px, IG_BOX_X + FONT_W, by + IG_HEAD_H + 7 * IG_ROW_H,
			   "Quit to ROM list", COL_DIM, COL_BOX),
	      "video menu: no Core or Frameskip rows");
	snprintf(path, sizeof(path), "%s/ui-ingame-video.raw", base);
	write_file_atomic(path, px, sizeof(px));
	CHECK(!core_run(0), "video menu: crop on the next run");
	fr = core_frame(&w, &h, &pitch);
	CHECK(w == 480 && h == 320, "video menu: crop %dx%d", w, h);
	ig_key(JOY_BIT(A));
	CHECK(settings.video_scale == VIDEO_FIT, "video menu: Picture -> fit");
	ig_key(JOY_BIT(B));

	/* Seeks: +10 s lands on the intra frame at or before the target,
	 * -60 s at the start. */
	video_runs(2, 0);
	pos = video_pos();
	video_runs(1, JOY_BIT(RIGHT));
	CHECK(video_pos() > pos + 9 && video_pos() <= pos + 10.05,
	      "video: +10 s from %.2f to %.2f", pos, video_pos());
	video_runs(1, JOY_BIT(L));
	CHECK(video_pos() < 0.05, "video: -60 s to %.2f", video_pos());
	video_runs(1, JOY_BIT(R));
	video_runs(1, JOY_BIT(RIGHT));
	pos = video_pos();
	CHECK(pos > 19 && pos < 30, "video: at %.2f s", pos);
	/* The resume state brings it back there. */
	CHECK(core_state_save("selftest") == 0, "video: state written");
	core_unload();
	if (video_load(clip, err, sizeof(err))) {
		CHECK(0, "video again: %s", err);
		goto out;
	}
	CHECK(core_state_resumed() && fabs(video_pos() - pos) < 0.6,
	      "video: resumed at %.2f s, was %.2f", video_pos(), pos);
	core_unload();

	/* Larger than the screen: shrunk to fit; mono 32 kHz. */
	if (video_load(big, err, sizeof(err))) {
		CHECK(0, "video big: %s", err);
		goto out;
	}
	CHECK(core_info()->sample_rate == 32000, "video big: %.0f Hz",
	      core_info()->sample_rate);
	video_runs(2, 0);
	fr = core_frame(&w, &h, &pitch);
	CHECK(w == 480 && h == 270, "video big: fit %dx%d", w, h);
	settings.video_scale = VIDEO_CROP;
	core_options_changed();
	video_runs(1, 0);
	fr = core_frame(&w, &h, &pitch);
	CHECK(w == 480 && h == 320, "video big: crop %dx%d", w, h);
	settings.video_scale = VIDEO_FIT;
	core_unload();

	/* No audio, the end: the last picture stays, the state says start
	 * over, A plays again. */
	if (video_load(silent, err, sizeof(err))) {
		CHECK(0, "video silent: %s", err);
		goto out;
	}
	pics = video_runs(200, 0);
	fr = core_frame(&w, &h, &pitch);
	CHECK(fr && w == 240 && h == 160 && pics >= 59 && pics <= 61,
	      "video silent: %dx%d, %d pictures", w, h, pics);
	CHECK(video_runs(30, 0) == 0 && video_pos() == 0,
	      "video silent: ended, state at %.2f", video_pos());
	CHECK(video_runs(4, JOY_BIT(A)) >= 2, "video silent: A plays again");
	audio_take_stats(&as);
	CHECK(as.xruns == 0, "video silent: %u underruns", as.xruns);
	core_unload();
	log_msg("selftest: video core done");
out:
	core_unload();
	audio_init_null(0);
	rom_settings = keep;
	settings.video_scale = keep_scale;
}

int selftest(const char *dir)
{
	mkdir(dir, 0755);
	snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", dir);
	test_blit();
	test_resampler();
	test_drc(0);
	test_drc(0.0005);
	test_drc(-0.0005);
	test_drc(0.003);
	test_drc(-0.003);
	test_audio_simple(0, 60);
	test_audio_simple(0.00005, 300);  /* 50 ppm for 5 minutes */
	test_audio_simple(-0.0002, 300);  /* slower codec: blocks, never xruns */
	test_log_dirs(dir);
	test_fx_tables();
	test_grid_pattern();
	test_grid_phases();
	test_pwrkey();
	test_romlist(dir);
	test_romdb(dir);
	test_files(dir);
	test_settings(dir);
	test_battery(dir);
	test_sleep_sysfs(dir);
	test_sdgrow(dir);
	test_cpuctl(dir);
	test_frameskip(dir);
	test_font();
	test_marquee();
	test_ingame_restart(dir);
	test_romlist_folders(dir);
	test_resume(dir);
	test_ui(dir);
	test_sysinfo(dir);
	test_clock(dir);
	test_pages();
	test_redraw(dir);
	test_video(dir);
	log_msg("selftest: %s (%d failures)", failures ? "FAILED" : "passed",
		failures);
	return failures ? 1 : 0;
}

/* ------------------------------------------------------ core switching */

/*
 * A minimal GBA ROM for the save path: it adds one to the SRAM byte at
 * 0x0E000000 (0xFF, blank, counts as 0) and writes 0x5A after it, then
 * spins. The header carries the bytes gpSP checks and an "SRAM_V" tag,
 * so both cores see 32KiB SRAM.
 */
static void save_test_rom(const char *path)
{
	static const uint32_t code[] = {
		0xe3a0040e,  /* mov   r0, #0x0e000000 */
		0xe5d01000,  /* ldrb  r1, [r0] */
		0xe35100ff,  /* cmp   r1, #0xff */
		0x03a01000,  /* moveq r1, #0 */
		0xe2811001,  /* add   r1, r1, #1 */
		0xe5c01000,  /* strb  r1, [r0] */
		0xe3a0205a,  /* mov   r2, #0x5a */
		0xe5c02001,  /* strb  r2, [r0, #1] */
		0xeafffffe,  /* b     . */
	};
	static uint8_t rom[4096];
	uint32_t b = 0xea00002e;  /* b 0xc0 */

	memset(rom, 0, sizeof(rom));
	memcpy(rom, &b, 4);
	memcpy(rom + 0xa0, "SAVETEST", 8);
	memcpy(rom + 0xac, "ZSVE", 4);
	rom[0xb2] = 0x96;
	memcpy(rom + 0xc0, code, sizeof(code));
	memcpy(rom + 0x800, "SRAM_V113", 9);
	CHECK(write_file_atomic(path, rom, sizeof(rom)) == 0, "write %s", path);
}

/*
 * A ROM whose picture changes every frame: display on with no layers (the
 * backdrop, palette entry 0, everywhere), and at every vblank the
 * backdrop color goes up by one. A frame the core draws differs from the
 * one before.
 */
static void color_test_rom(const char *path)
{
	static const uint32_t code[] = {
		0xe3a00301,  /* mov   r0, #0x04000000 */
		0xe3a01000,  /* mov   r1, #0 */
		0xe1c010b0,  /* strh  r1, [r0]          DISPCNT: mode 0 */
		0xe3a02405,  /* mov   r2, #0x05000000   palette */
		0xe3a03000,  /* mov   r3, #0 */
		0xe1d040b6,  /* 1: ldrh r4, [r0, #6]    VCOUNT */
		0xe35400a0,  /* cmp   r4, #160 */
		0x0afffffc,  /* beq   1b */
		0xe1d040b6,  /* 2: ldrh r4, [r0, #6] */
		0xe35400a0,  /* cmp   r4, #160 */
		0x1afffffc,  /* bne   2b */
		0xe2833001,  /* add   r3, r3, #1 */
		0xe1c230b0,  /* strh  r3, [r2] */
		0xeafffff6,  /* b     1b */
	};
	static uint8_t rom[4096];
	uint32_t b = 0xea00002e;  /* b 0xc0 */

	memset(rom, 0, sizeof(rom));
	memcpy(rom, &b, 4);
	memcpy(rom + 0xa0, "COLORTEST", 9);
	memcpy(rom + 0xac, "ZCLR", 4);
	rom[0xb2] = 0x96;
	memcpy(rom + 0xc0, code, sizeof(code));
	CHECK(write_file_atomic(path, rom, sizeof(rom)) == 0, "write %s", path);
}

static void run_frames(long frames)
{
	for (long i = 0; i < frames; i++) {
		pid_t pid;
		int st;

		core_run(0);
		audio_flush();
		core_save_tick(i * 16661000LL);
		while ((pid = sys_reap(&st)) > 0)
			core_child_exited(pid, st);
	}
}

static int switch_round(const char *rom, const char *sav, int core,
			int expect, long frames)
{
	uint8_t buf[1 << 17];
	char err[128];
	int n;

	if (core_load(rom, core, err, sizeof(err))) {
		CHECK(0, "%s: %s", core_names[core], err);
		return -1;
	}
	CHECK(core_info()->core == core, "running %s", core_names[core]);
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	run_frames(frames);
	core_unload();
	n = read_file(sav, buf, sizeof(buf));
	CHECK(n == 32768, "%s: save size %d", core_names[core], n);
	CHECK(n > 1 && buf[0] == expect && buf[1] == 0x5a,
	      "%s: save counter %d, expected %d", core_names[core],
	      n > 0 ? buf[0] : -1, expect);
	log_msg("core switch: %s wrote counter %d", core_names[core],
		n > 0 ? buf[0] : -1);
	return 0;
}

/*
 * Restart game: the core's reset writes the save first, the game boots
 * again and sees it (the counter goes up once more), and the save from
 * after the reset is written on unload.
 */
static void reset_round(const char *rom, const char *sav, int core,
			int expect, long frames)
{
	uint8_t buf[64];
	char err[128];
	int n;

	if (core_load(rom, core, err, sizeof(err))) {
		CHECK(0, "reset: %s: %s", core_names[core], err);
		return;
	}
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	run_frames(frames);
	core_reset();
	n = read_file(sav, buf, sizeof(buf));
	CHECK(n > 1 && buf[0] == expect && buf[1] == 0x5a,
	      "reset: %s: save before the reset %d, expected %d",
	      core_names[core], n > 0 ? buf[0] : -1, expect);
	run_frames(frames);
	core_unload();
	n = read_file(sav, buf, sizeof(buf));
	CHECK(n > 1 && buf[0] == expect + 1,
	      "reset: %s: save after the reset %d, expected %d",
	      core_names[core], n > 0 ? buf[0] : -1, expect + 1);
	log_msg("core switch: %s reset, counter %d", core_names[core],
		n > 0 ? buf[0] : -1);
}

/*
 * Frame blending switched in game: gpSP reads the option again after the
 * next retro_run and then hands out its mixed buffer instead of the
 * rendered one; mGBA keeps the setting without a change.
 */
static void blend_live(const char *rom)
{
	const uint16_t *f0, *f1, *f2;
	char err[128];
	int w, h, p;

	rom_settings.blend = 0;
	if (core_load(rom, CORE_GPSP, err, sizeof(err))) {
		CHECK(0, "blend: %s", err);
		return;
	}
	run_frames(2);
	f0 = core_frame(&w, &h, &p);
	rom_settings.blend = 1;
	core_options_changed();
	run_frames(2);  /* the first run reads the option, the next mixes */
	f1 = core_frame(&w, &h, &p);
	rom_settings.blend = 0;
	core_options_changed();
	run_frames(2);
	f2 = core_frame(&w, &h, &p);
	CHECK(f0 && f1 && f2 && f1 != f0 && f2 != f1,
	      "blend live: frames %p %p %p", (const void *)f0, (const void *)f1,
	      (const void *)f2);
	core_unload();
	if (core_load(rom, CORE_MGBA, err, sizeof(err))) {
		CHECK(0, "blend: %s", err);
		return;
	}
	run_frames(2);
	rom_settings.blend = 1;
	core_options_changed();
	run_frames(2);
	core_unload();
	rom_settings.blend = 0;
}

/* Frames skipped by the loaded core in the next n. */
static unsigned skipped_in(long n)
{
	unsigned r0, s0, r1, s1;

	core_frame_counts(&r0, &s0);
	run_frames(n);
	core_frame_counts(&r1, &s1);
	return s1 - s0;
}

/*
 * mGBA's skipped frames as core.c follows its counter, against what mGBA
 * draws: with color_test_rom every drawn frame has a new backdrop color,
 * a skipped one leaves the buffer as it was. Off first (every frame
 * drawn: the ROM works), then 2 and 1 set live, frame by frame.
 */
static void mgba_skip_check(const char *data)
{
	static const int settings_seq[] = {
		FRAMESKIP_OFF, FRAMESKIP_2, FRAMESKIP_1, FRAMESKIP_3,
	};
	char rom[300], err[128];
	uint16_t prev = 0;
	int w, h, p, mismatch = 0, drawn_off = 0;

	snprintf(rom, sizeof(rom), "%s/colortest.gba", data);
	color_test_rom(rom);
	rom_settings.frameskip = FRAMESKIP_OFF;
	if (core_load(rom, CORE_MGBA, err, sizeof(err))) {
		CHECK(0, "mgba skip check: %s", err);
		return;
	}
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	run_frames(5);
	for (size_t k = 0; k < ARRAY_SIZE(settings_seq); k++) {
		rom_settings.frameskip = settings_seq[k];
		core_options_changed();
		for (int i = 0; i < 40; i++) {
			int skipped = core_run(0);
			const uint16_t *f = core_frame(&w, &h, &p);
			int drawn = f && f[p * 80 + 120] != prev;

			audio_flush();
			if (f)
				prev = f[p * 80 + 120];
			mismatch += i > 0 && drawn == skipped;
			drawn_off += k == 0 && drawn;
		}
	}
	core_unload();
	CHECK(drawn_off == 40, "mgba skip check: %d of 40 drawn with Off",
	      drawn_off);
	CHECK(mismatch == 0, "mgba skip check: %d frames counted wrong",
	      mismatch);
	log_msg("core switch: mgba skip check: %d of 40 drawn with off, %d "
		"frames counted wrong", drawn_off, mismatch);
	rom_settings.frameskip = FRAMESKIP_OFF;
}

/*
 * Frame skipping switched in game with the real cores: a fixed interval
 * of 2 skips two of every three frames (NULL frames from gpSP, mGBA's
 * counter followed, see mgba_skip_check), and Off stops it; Auto skips
 * with gpSP while the audio buffer drains (a simulated device playing
 * 30% fast), and is not in mGBA.
 */
static void frameskip_live(const char *rom)
{
	static const int cores[] = { CORE_GPSP, CORE_MGBA };
	char err[128];
	unsigned n;

	for (size_t i = 0; i < ARRAY_SIZE(cores); i++) {
		const char *name = core_names[cores[i]];

		rom_settings.frameskip = FRAMESKIP_OFF;
		audio_init_null(0.001);
		if (core_load(rom, cores[i], err, sizeof(err))) {
			CHECK(0, "frameskip: %s: %s", name, err);
			continue;
		}
		audio_set_source(core_info()->sample_rate, core_info()->fps,
				 60.0186);
		n = skipped_in(10);
		CHECK(n == 0, "frameskip %s off: %u skipped", name, n);
		rom_settings.frameskip = FRAMESKIP_2;
		core_options_changed();
		run_frames(3);  /* read after the next run, then a cycle */
		n = skipped_in(30);
		CHECK(n == 20, "frameskip %s 2: %u of 30 skipped", name, n);
		rom_settings.frameskip = FRAMESKIP_OFF;
		core_options_changed();
		run_frames(3);
		n = skipped_in(30);
		CHECK(n == 0, "frameskip %s back off: %u skipped", name, n);
		rom_settings.frameskip = FRAMESKIP_AUTO;
		core_options_changed();
		audio_init_null(0.3);
		audio_set_source(core_info()->sample_rate, core_info()->fps,
				 60.0186);
		n = skipped_in(300);
		log_msg("core switch: %s frameskip auto, audio draining: %u of 300 "
			"skipped", name, n);
		CHECK(cores[i] == CORE_GPSP ? n > 0 : n == 0,
		      "frameskip %s auto: %u skipped", name, n);
		core_unload();
		log_msg("core switch: %s frameskip done", name);
	}
	rom_settings.frameskip = FRAMESKIP_OFF;
	audio_init_null(0.001);
}

/*
 * In-game menu with a core running: Core set to another one reads
 * "Restart with", which asks for a second A like Restart game; changing
 * the core again in between cancels.
 */
static void menu_restart_with(const char *rom)
{
	char err[128];
	int act;

	rom_settings.core = CORE_GPSP;
	if (core_load(rom, CORE_GPSP, err, sizeof(err))) {
		CHECK(0, "restart with: %s", err);
		return;
	}
	ig_open("Game");
	for (int i = 0; i < 5; i++)
		ig_key(JOY_BIT(DOWN));
	ig_key(JOY_BIT(RIGHT));
	CHECK(rom_settings.core == CORE_MGBA, "restart with: core picked");
	CHECK(ig_key(JOY_BIT(A)) == UI_NONE,
	      "restart with: first A arms");
	CHECK(ig_key(JOY_BIT(A)) == UI_RESTART,
	      "restart with: second A confirms");
	ig_key(JOY_BIT(A));
	ig_key(JOY_BIT(RIGHT));
	ig_key(JOY_BIT(RIGHT));
	act = ig_key(JOY_BIT(A));
	CHECK(rom_settings.core == CORE_MGBA && act == UI_NONE,
	      "restart with: changing the core cancels");
	core_unload();
	rom_settings.core = CORE_GPSP;
}

/* The save test ROM's counter in the .sav, -1 if there is none. */
static int sav_counter(const char *sav)
{
	uint8_t buf[4];

	return read_file(sav, buf, sizeof(buf)) > 1 && buf[1] == 0x5a ?
	       buf[0] : -1;
}

/* Load, run, optionally write the resume state, unload; the .sav counter
 * after it. The ROM bumps the counter once per boot, so a resumed game
 * leaves it alone. */
static int state_run(const char *rom, const char *sav, int core, long frames,
		     int save_state, int reset)
{
	char err[128];

	if (core_load(rom, core, err, sizeof(err))) {
		CHECK(0, "state: %s: %s", core_names[core], err);
		return -1;
	}
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	run_frames(frames);
	if (reset) {
		core_reset();
		run_frames(frames);
	}
	if (save_state)
		core_state_save("test");
	core_unload();
	return sav_counter(sav);
}

/*
 * Resume states with one core: written on a quit and resumed on the next
 * start (the boot counter stays), skipped when the .sav changed, removed
 * by Restart game, and skipped when truncated, damaged, or (valid file,
 * payload the core does not accept) rejected by the core; every skip
 * boots the game normally.
 */
static void state_rounds(const char *data, int core, long frames)
{
	const char *name = core_api_get(core) ? core_api_get(core)->name : "?";
	char rom[300], sav[300], st[300];
	uint8_t blank[2] = { 41, 0x5a }, *buf;
	struct stat sb;
	int n, k;

	snprintf(rom, sizeof(rom), "%s/statetest.gba", data);
	snprintf(sav, sizeof(sav), "%s/saves/statetest.sav", data);
	snprintf(st, sizeof(st), "%s/saves/statetest.%s.state", data, name);
	save_test_rom(rom);
	unlink(sav);
	unlink(st);
	n = state_run(rom, sav, core, frames, 1, 0);
	CHECK(n == 1 && access(st, R_OK) == 0, "state %s: written, counter %d",
	      name, n);
	n = state_run(rom, sav, core, frames, 0, 0);
	CHECK(n == 1, "state %s: resumed, counter %d (1 if not booted again)",
	      name, n);
	/* The .sav replaced (from a PC, say): the state is stale. */
	CHECK(write_file_atomic(sav, blank, sizeof(blank)) == 0, "write .sav");
	n = state_run(rom, sav, core, frames, 1, 0);
	CHECK(n == 42, "state %s: .sav changed, booted: counter %d", name, n);
	n = state_run(rom, sav, core, frames, 0, 0);
	CHECK(n == 42, "state %s: resumed after the .sav change, counter %d",
	      name, n);
	/* Restart game: resumed, reset (boots: 43), state gone. */
	n = state_run(rom, sav, core, frames, 0, 1);
	CHECK(n == 43 && access(st, F_OK) != 0,
	      "state %s: restart game, counter %d, state %s", name, n,
	      access(st, F_OK) ? "removed" : "still there");
	n = state_run(rom, sav, core, frames, 1, 0);
	CHECK(n == 44, "state %s: no state after restart, counter %d", name, n);
	/* Truncated. */
	CHECK(stat(st, &sb) == 0 && truncate(st, sb.st_size / 2) == 0,
	      "truncate state");
	n = state_run(rom, sav, core, frames, 1, 0);
	CHECK(n == 45, "state %s: truncated, booted: counter %d", name, n);
	/* One byte damaged in the payload. */
	buf = malloc(sb.st_size);
	k = buf ? read_file(st, buf, sb.st_size) : -1;
	CHECK(k > 200, "read state: %d", k);
	if (k > 200) {
		buf[k / 2] ^= 0x10;
		write_file_atomic(st, buf, k);
	}
	n = state_run(rom, sav, core, frames, 1, 0);
	CHECK(n == 46, "state %s: damaged, booted: counter %d", name, n);
	/* A payload the core does not accept, with a matching CRC (header:
	 * payload_crc at byte 20, payload from byte 40). */
	k = buf ? read_file(st, buf, sb.st_size) : -1;
	if (k > 200) {
		uint32_t crc;

		memset(buf + 40, 0, 16);
		crc = romdb_crc32(0, buf + 40, k - 40);
		memcpy(buf + 20, &crc, 4);
		write_file_atomic(st, buf, k);
	}
	free(buf);
	n = state_run(rom, sav, core, frames, 1, 0);
	CHECK(n == 47, "state %s: rejected by the core, booted: counter %d",
	      name, n);
	n = state_run(rom, sav, core, frames, 0, 0);
	CHECK(n == 47, "state %s: resumed at the end, counter %d", name, n);
	log_msg("core switch: %s state rounds done, counter %d", name, n);
}

/*
 * The other core does not see a state: with mGBA's state on the card
 * gpSP boots the game (the counter goes up); with the .sav put back,
 * mGBA then resumes its own state.
 */
static void state_other_core(const char *data, long frames)
{
	static uint8_t keep[1 << 15];
	char rom[300], sav[300], st[300];
	int n, len;

	snprintf(rom, sizeof(rom), "%s/statetest.gba", data);
	snprintf(sav, sizeof(sav), "%s/saves/statetest.sav", data);
	snprintf(st, sizeof(st), "%s/saves/statetest.gpsp.state", data);
	unlink(st);
	n = sav_counter(sav);
	len = read_file(sav, keep, sizeof(keep));
	CHECK(state_run(rom, sav, CORE_GPSP, frames, 0, 0) == n + 1,
	      "state: gpSP booted beside an mGBA state");
	CHECK(len > 0 && write_file_atomic(sav, keep, len) == 0,
	      "restore .sav");
	CHECK(state_run(rom, sav, CORE_MGBA, frames, 0, 0) == n,
	      "state: mGBA resumed its own state after gpSP");
}

/*
 * mGBA -> gpSP -> mGBA in one process on the same .sav, then a RetroArch
 * .srm picked up by gpSP when there is no .sav (and left untouched), a
 * reset in each core on that save, then frame blending and frame
 * skipping switched live, the in-game menu's Restart with, and the resume
 * states with each core.
 */
int selftest_core_switch(const char *data, long frames)
{
	char rom[300], sav[300], srm[300], path[300];
	uint8_t buf[64];

	mkdir(data, 0755);
	snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", data);
	sys_make_dirs();
	snprintf(path, sizeof(path), "%s/log/frontend.txt", fe_data_dir);
	log_open_file(path);
	settings_load();
	audio_init_null(0.001);
	snprintf(rom, sizeof(rom), "%s/savetest.gba", data);
	snprintf(sav, sizeof(sav), "%s/saves/savetest.sav", data);
	snprintf(srm, sizeof(srm), "%s/saves/savetest.srm", data);
	save_test_rom(rom);
	unlink(sav);
	unlink(srm);
	switch_round(rom, sav, CORE_MGBA, 1, frames);
	switch_round(rom, sav, CORE_GPSP, 2, frames);
	switch_round(rom, sav, CORE_MGBA, 3, frames);
	CHECK(rename(sav, srm) == 0, "rename to .srm");
	switch_round(rom, sav, CORE_GPSP, 4, frames);
	CHECK(read_file(srm, buf, sizeof(buf)) == sizeof(buf) && buf[0] == 3,
	      ".srm left alone");
	reset_round(rom, sav, CORE_GPSP, 5, frames);
	reset_round(rom, sav, CORE_MGBA, 7, frames);
	blend_live(rom);
	frameskip_live(rom);
	mgba_skip_check(data);
	menu_restart_with(rom);
	state_rounds(data, CORE_GPSP, frames);
	state_rounds(data, CORE_MGBA, frames);
	state_other_core(data, frames);
	log_msg("core switch: %s (%d failures)", failures ? "FAILED" : "passed",
		failures);
	return failures ? 1 : 0;
}
