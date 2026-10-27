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
#include "romdb.h"
#include "romlist.h"
#include "sdgrow.h"
#include "sleep.h"
#include "sysinfo.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
	} \
} while (0)

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
	CHECK(settings_format(&s, buf, sizeof(buf)) > 0, "format");
	CHECK(settings_parse(&t, buf) == 8, "parse back: %s", buf);
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
	      t.grid_dim == 50 && t.grid_boost == 0, "settings range check");
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

	/* Files: missing, old location, config/, per ROM. */
	sys_make_dirs();
	settings_load();
	CHECK(!memcmp(&settings, &settings_defaults, sizeof(settings)),
	      "no file gives defaults");
	snprintf(path, sizeof(path), "%s/settings.txt", base);
	write_text(path, "volume=40\nbrightness=30\n");
	settings_load();
	CHECK(settings.volume == 40 && settings.brightness == 30,
	      "old settings.txt still read");
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
	rom_settings_load(NULL);
	CHECK(rom_settings.core == CORE_GPSP, "no game: default core");
	CHECK(rom_settings.blend == 0, "no game: blend off");
	settings = keep;
	log_set_debug(1);
}

static void put(const char *dir, const char *name, const char *val)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	write_text(path, val);
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

	battery_init("/nonexistent/power_supply");
	CHECK(battery_read(&b) != 0 && !b.valid, "no battery");
	battery_init(ps);  /* back, for the UI renders */
	battery_poll(5000000000000000LL);
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
 * selected, the folder and ROM are kept over a game (romlist_menu_enter
 * again), a folder that is gone falls back to the top. Uses the roms/
 * of test_romlist. */
static void test_romlist_folders(const char *base)
{
	static uint16_t px[480 * 320];
	struct canvas cv = { px, 480, 320 };
	char path[512], want[512], gone[400];
	const uint32_t a = JOY_BIT(A), b = JOY_BIT(B);

	romlist_menu_enter();
	CHECK(romlist_menu_update(a, path, sizeof(path)) == UI_NONE,
	      "folders: A on action/ opens it");
	CHECK(romlist_menu_update(a, path, sizeof(path)) == UI_NONE,
	      "folders: A on inner/ opens it");
	romlist_menu_draw(&cv);
	snprintf(path, sizeof(path), "%s/ui-romlist-folder.raw", base);
	write_file_atomic(path, px, sizeof(px));
	snprintf(want, sizeof(want), "%s/roms/action/inner/deep.gba", base);
	CHECK(romlist_menu_update(a, path, sizeof(path)) == UI_START_ROM &&
	      !strcmp(path, want), "folders: start %s", path);
	/* Back from the game: same folder, same ROM. */
	romlist_menu_enter();
	CHECK(romlist_menu_update(a, path, sizeof(path)) == UI_START_ROM &&
	      !strcmp(path, want), "folders: after the game %s", path);
	/* B: up to action/ with inner/ selected. */
	CHECK(romlist_menu_update(b, path, sizeof(path)) == UI_NONE, "folders: B");
	CHECK(romlist_menu_update(a, path, sizeof(path)) == UI_NONE &&
	      romlist_menu_update(a, path, sizeof(path)) == UI_START_ROM &&
	      !strcmp(path, want), "folders: inner/ reselected: %s", path);
	romlist_menu_update(b, path, sizeof(path));
	romlist_menu_update(JOY_BIT(DOWN), path, sizeof(path));
	snprintf(want, sizeof(want), "%s/roms/action/x.gba", base);
	CHECK(romlist_menu_update(a, path, sizeof(path)) == UI_START_ROM &&
	      !strcmp(path, want), "folders: start %s", path);
	/* Up to the top with action/ selected; B there does nothing. */
	romlist_menu_update(b, path, sizeof(path));
	CHECK(romlist_menu_update(b, path, sizeof(path)) == UI_NONE,
	      "folders: B at the top");
	romlist_menu_update(a, path, sizeof(path));
	romlist_menu_update(JOY_BIT(DOWN), path, sizeof(path));
	CHECK(romlist_menu_update(a, path, sizeof(path)) == UI_START_ROM &&
	      !strcmp(path, want), "folders: action/ reselected: %s", path);
	/* The empty folder: B still goes up. */
	romlist_menu_update(b, path, sizeof(path));
	romlist_menu_update(JOY_BIT(DOWN), path, sizeof(path));
	romlist_menu_update(a, path, sizeof(path));
	romlist_menu_draw(&cv);
	snprintf(path, sizeof(path), "%s/ui-romlist-empty.raw", base);
	write_file_atomic(path, px, sizeof(px));
	CHECK(romlist_menu_update(a, path, sizeof(path)) == UI_NONE,
	      "folders: A in an empty folder");
	romlist_menu_update(b, path, sizeof(path));
	romlist_menu_update(JOY_BIT(UP), path, sizeof(path));
	romlist_menu_update(a, path, sizeof(path));
	CHECK(romlist_menu_update(JOY_BIT(DOWN), path, sizeof(path)) == UI_NONE &&
	      romlist_menu_update(a, path, sizeof(path)) == UI_START_ROM &&
	      !strcmp(path, want), "folders: up from empty/: %s", path);
	/* A folder removed while a game ran: back at the top. */
	romlist_menu_update(b, path, sizeof(path));
	snprintf(gone, sizeof(gone), "%s/roms/gone", base);
	mkdir(gone, 0755);
	romlist_menu_enter();
	romlist_menu_update(JOY_BIT(DOWN), path, sizeof(path));
	romlist_menu_update(JOY_BIT(DOWN), path, sizeof(path));
	romlist_menu_update(a, path, sizeof(path));
	rmdir(gone);
	romlist_menu_enter();
	romlist_menu_update(JOY_BIT(UP), path, sizeof(path));
	snprintf(want, sizeof(want), "%s/roms/z.Gba", base);
	CHECK(romlist_menu_update(a, path, sizeof(path)) == UI_START_ROM &&
	      !strcmp(path, want), "folders: gone folder: %s", path);
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
		double w = sm->base_ms * 1296000.0 / (f > 0 ? f : 1296000);
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
		cpuctl_game_frame((int64_t)(w * 1e6), pending, sim_t);
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
	static const struct sim light = { 6, 0.3, 200, 1.3, 0, 0, 0 };
	static const struct sim heavy = { 11, 0.3, 0, 0, 0, 0, 0 };
	static const struct sim bursty = { 6, 0.3, 0, 0, 900, 60, 2.0 };
	static const struct sim mid = { 9, 0.2, 0, 0, 0, 0, 0 };
	static const struct sim rising = { 11.6, 0.1, 0, 0, 0, 0, 0 };

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
		CHECK(header == 1 && all == 5, "perf.csv: %d headers, %d summaries",
		      header, all);
	}
	/* avg_mA (column 16) of the 816 MHz row: 200 + 0.3 * 816. */
	CHECK(fabs(csv_field(csv, ",adaptive,816000,", 16) - 445) < 2,
	      "816 MHz battery current %.0f", csv_field(csv, ",adaptive,816000,",
							16));
	CHECK(csv_field(csv, ",max,1296000,", 5) >= 1800, "max mode row");
	CHECK(csv_field(csv, ",fixed,816000,", 5) >= 1200, "fixed mode row");
}

/* Long ROM names: the marquee timing and a scrolled, clipped row. */
static void test_marquee(void)
{
	static uint16_t px[480 * 24], ref[480 * 24];
	struct canvas cv = { px, 480, 24 }, rc = { ref, 480, 24 };
	const char *name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghij *";
	int n = strlen(name), excess = 16 * (n - 29), prev = 0, bad = 0;

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
	draw_fill(&cv, 0, 0, 480, 24, 0);
	draw_rom_name(&cv, 8, 4, COL_FG, name, 29, 0);
	draw_fill(&rc, 0, 0, 480, 24, 0);
	draw_text(&rc, 8, 4, 2, COL_FG, "ABCDEFGHIJKLMNOPQRSTUVWXYZ ~*");
	CHECK(!memcmp(px, ref, sizeof(px)), "marquee: cut row");
	draw_fill(&cv, 0, 0, 480, 24, 0);
	draw_rom_name(&cv, 8, 4, COL_FG, "Short (USA) *", 29, 40);
	draw_fill(&rc, 0, 0, 480, 24, 0);
	draw_text(&rc, 8, 4, 2, COL_FG, "Short (USA) *");
	CHECK(!memcmp(px, ref, sizeof(px)), "marquee: a short name moved");

	/* Scrolled by 37 and to the end: all of the name, " *" included,
	 * shifted and clipped to the 29 columns. */
	for (int k = 0; k < 2; k++) {
		int off = k ? excess : 37;

		draw_fill(&cv, 0, 0, 480, 24, 0);
		draw_rom_name(&cv, 8, 4, COL_FG, name, 29, off);
		draw_fill(&rc, 0, 0, 480, 24, 0);
		draw_text(&rc, 8 - off, 4, 2, COL_FG, name);
		draw_fill(&rc, 0, 0, 8, 24, 0);
		draw_fill(&rc, 8 + 16 * 29, 0, 480, 24, 0);
		CHECK(!memcmp(px, ref, sizeof(px)), "marquee: row scrolled by %d",
		      off);
	}
	/* At the end the last column holds the "*". */
	draw_fill(&rc, 0, 0, 480, 24, 0);
	draw_text(&rc, 8 + 16 * 28, 4, 2, COL_FG, "*");
	bad = 0;
	for (int y = 0; y < 24; y++)
		bad += !!memcmp(px + y * 480 + 8 + 16 * 28,
				ref + y * 480 + 8 + 16 * 28, 16 * 2);
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
 * 3 s (180 updates, one per vblank); moving away or waiting cancels, B
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

	ingame_menu_enter("Game");
	ingame_menu_update(JOY_BIT(DOWN));
	ingame_menu_draw(&cv, NULL, 0, 0, 0);
	/* Row 1 at y 92 (box at 34, 36 px name, 22 px rows). */
	plain = count_color(px, 92, 116, COL_HI);
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_NONE, "restart: first A arms");
	ingame_menu_draw(&cv, NULL, 0, 0, 0);
	armed = count_color(px, 92, 116, COL_HI);
	CHECK(plain == 0 && armed > 0, "restart: armed row %d/%d highlighted",
	      plain, armed);
	snprintf(path, sizeof(path), "%s/ui-ingame-restart.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_RESET,
	      "restart: second A confirms");
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_NONE,
	      "restart: disarmed after confirming");

	/* Moving away and back cancels. */
	ingame_menu_enter("Game");
	ingame_menu_update(JOY_BIT(DOWN));
	ingame_menu_update(JOY_BIT(A));
	ingame_menu_update(JOY_BIT(DOWN));
	ingame_menu_update(JOY_BIT(UP));
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_NONE,
	      "restart: moving away cancels");
	/* An A on the 179th frame after arming still confirms, on the
	 * 180th it does not. */
	ingame_menu_enter("Game");
	ingame_menu_update(JOY_BIT(DOWN));
	ingame_menu_update(JOY_BIT(A));
	for (int i = 0; i < 178; i++)
		ingame_menu_update(0);
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_RESET,
	      "restart: confirmed within 3 s");
	ingame_menu_enter("Game");
	ingame_menu_update(JOY_BIT(DOWN));
	ingame_menu_update(JOY_BIT(A));
	for (int i = 0; i < 179; i++)
		ingame_menu_update(0);
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_NONE,
	      "restart: times out after 3 s");
	/* That A armed it again; 180 frames later the label is back. */
	for (int i = 0; i < 180; i++)
		ingame_menu_update(0);
	ingame_menu_draw(&cv, NULL, 0, 0, 0);
	CHECK(count_color(px, 92, 116, COL_HI) == 0,
	      "restart: label back after 3 s");
	/* B resumes; the menu opens again disarmed. */
	CHECK(ingame_menu_update(JOY_BIT(B)) == UI_RESUME, "restart: B resumes");
	ingame_menu_enter("Game");
	ingame_menu_update(JOY_BIT(DOWN));
	ingame_menu_draw(&cv, NULL, 0, 0, 0);
	CHECK(count_color(px, 92, 116, COL_HI) == 0,
	      "restart: disarmed on entering");

	/* Up from Resume wraps to Power off, the last row: its highlight
	 * and the box frame end at y 312, the rest is the dimmed game. */
	ingame_menu_enter("Game");
	act = ingame_menu_update(JOY_BIT(UP));
	ingame_menu_draw(&cv, NULL, 0, 0, 0);
	CHECK(act == UI_NONE && px[300 * 480 + 240] == COL_SEL &&
	      px[309 * 480 + 240] == COL_BOX && px[311 * 480 + 240] == COL_SEL &&
	      count_color(px, 312, 320, COL_SEL) == 0 &&
	      count_color(px, 312, 320, COL_BOX) == 0,
	      "restart: menu box fits on screen");
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_NONE,
	      "power off: first A arms");
	ingame_menu_draw(&cv, NULL, 0, 0, 0);
	CHECK(count_color(px, 290, 312, COL_HI) > 0,
	      "power off: armed row highlighted");
	snprintf(path, sizeof(path), "%s/ui-ingame-poweroff.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_POWEROFF,
	      "power off: second A confirms");
	/* Armed on Restart game, A on Power off only arms that row. */
	ingame_menu_enter("Game");
	ingame_menu_update(JOY_BIT(DOWN));
	ingame_menu_update(JOY_BIT(A));
	ingame_menu_update(JOY_BIT(UP));
	ingame_menu_update(JOY_BIT(UP));
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_NONE,
	      "power off: not confirmed by another row's arming");
	for (int i = 0; i < 179; i++)
		ingame_menu_update(0);
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_NONE,
	      "power off: times out after 3 s");

	/* Global settings: Power off is the row before Back. */
	settings_menu_enter(0);
	for (int i = 0; i < 7; i++)
		settings_menu_update(JOY_BIT(DOWN));
	settings_menu_draw(&cv, NULL, 0, 0, 0);
	/* Row 7 at y 46 + 7 * 26 = 228. */
	plain = count_color(px, 223, 249, COL_HI);
	CHECK(settings_menu_update(JOY_BIT(A)) == UI_NONE,
	      "settings power off: first A arms");
	settings_menu_draw(&cv, NULL, 0, 0, 0);
	armed = count_color(px, 223, 249, COL_HI);
	CHECK(plain == 0 && armed > 0,
	      "settings power off: armed row %d/%d highlighted", plain, armed);
	CHECK(count_color(px, 280, 300, COL_SEL) == 0 &&
	      count_color(px, 280, 300, COL_FG) == 0 &&
	      count_color(px, 280, 300, COL_DIM) == 0,
	      "settings: rows end above B:back");
	snprintf(path, sizeof(path), "%s/ui-settings-poweroff.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	CHECK(settings_menu_update(JOY_BIT(A)) == UI_POWEROFF,
	      "settings power off: second A confirms");
	settings_menu_update(JOY_BIT(A));
	settings_menu_update(JOY_BIT(DOWN));
	settings_menu_update(JOY_BIT(UP));
	CHECK(settings_menu_update(JOY_BIT(A)) == UI_NONE,
	      "settings power off: moving away cancels");
	settings_menu_enter(0);
	for (int i = 0; i < 7; i++)
		settings_menu_update(JOY_BIT(DOWN));
	settings_menu_update(JOY_BIT(A));
	for (int i = 0; i < 179; i++)
		settings_menu_update(0);
	CHECK(settings_menu_update(JOY_BIT(A)) == UI_NONE,
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

/* Rows [y, y + 16) from x match text drawn there on the background. */
static int row_shows(const uint16_t *px, int x, int y, const char *text)
{
	static uint16_t ref[480 * 320];
	struct canvas rc = { ref, 480, 320 };
	int w = 16 * (int)strlen(text);

	draw_fill(&rc, 0, 0, 480, 320, COL_BG);
	draw_text(&rc, x, y, 2, COL_FG, text);
	for (int r = y; r < y + 16; r++)
		if (memcmp(px + r * 480 + x, ref + r * 480 + x, w * 2))
			return 0;
	return 1;
}

/*
 * System info: /proc/stat and /proc/meminfo parsing, then the page on a
 * fake /proc and /sys (two cores, the two PX30 tsadc zones, one hwmon
 * sensor, one cpufreq policy): CPU usage since boot, then from the
 * deltas after a second, the temperatures, and B goes back.
 */
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
	sysinfo_enter("selftest", 0);
	sysinfo_draw(&cv);
	/* CPU, Cores, Freq, Memory, Temp rows from y 40, 20 px apart. */
	CHECK(row_shows(px, 136, 40, " 20%  Adaptive"), "sysinfo: CPU since boot");
	CHECK(row_shows(px, 136, 60, " 20%   20%"), "sysinfo: cores since boot");
	CHECK(row_shows(px, 136, 80, "1296 MHz"), "sysinfo: frequency");
	CHECK(row_shows(px, 136, 100, "200 / 980 MB"), "sysinfo: memory");
	CHECK(row_shows(px, 136, 120, "soc 45.2C  gpu 44.8C"),
	      "sysinfo: thermal zones");
	CHECK(row_shows(px, 136, 140, "batt 30.0C"), "sysinfo: hwmon wraps");
	put_path(root, "proc/stat", stat1);
	CHECK(sysinfo_update(0, 500000000LL) == UI_NONE, "sysinfo: stays open");
	sysinfo_draw(&cv);
	CHECK(row_shows(px, 136, 40, " 20%  Adaptive"),
	      "sysinfo: not refreshed before 1 s");
	sysinfo_update(0, 1000000000LL);
	sysinfo_draw(&cv);
	CHECK(row_shows(px, 136, 40, " 50%  Adaptive") &&
	      row_shows(px, 136, 60, "  0%  100%"), "sysinfo: usage from deltas");
	snprintf(path, sizeof(path), "%s/ui-sysinfo.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	CHECK(sysinfo_update(JOY_BIT(B), 1100000000LL) == UI_BACK,
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
		datetime_update(JOY_BIT(RIGHT));
	for (int i = 0; i < 100; i++) {
		int v;

		datetime_value(&tm);
		v = field == 0 ? tm.tm_year + 1900 : field == 1 ? tm.tm_mon + 1 :
		    field == 2 ? tm.tm_mday : field == 3 ? tm.tm_hour : tm.tm_min;
		if (v == value)
			return;
		datetime_update(JOY_BIT(UP));
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

	datetime_enter();
	CHECK(datetime_field() == 0 && !datetime_message()[0], "clock: enter");
	datetime_update(JOY_BIT(LEFT));
	CHECK(datetime_field() == 4, "clock: left wraps to the minute");
	datetime_update(JOY_BIT(RIGHT));
	CHECK(datetime_field() == 0, "clock: right wraps to the year");
	dt_set(0, 2024);
	dt_set(1, 1);
	dt_set(2, 31);
	dt_set(3, 23);
	dt_set(4, 0);
	CHECK(dt_is(2024, 1, 31, 23, 0), "clock: fields set");
	/* Jan 31 -> Feb 29 in a leap year -> Feb 28 the year after. */
	datetime_update(JOY_BIT(LEFT));
	datetime_update(JOY_BIT(LEFT));
	datetime_update(JOY_BIT(LEFT));
	CHECK(datetime_field() == 1, "clock: on the month");
	datetime_update(JOY_BIT(UP));
	CHECK(dt_is(2024, 2, 29, 23, 0), "clock: Feb 29 clamped in 2024");
	datetime_update(JOY_BIT(LEFT));
	datetime_update(JOY_BIT(UP));
	CHECK(dt_is(2025, 2, 28, 23, 0), "clock: Feb 28 in 2025");
	/* Wrapping: year, month, day in its month, hour, minute. */
	datetime_update(JOY_BIT(DOWN));
	datetime_update(JOY_BIT(DOWN));
	CHECK(dt_is(2099, 2, 28, 23, 0), "clock: year wraps down");
	datetime_update(JOY_BIT(UP));
	CHECK(dt_is(2024, 2, 28, 23, 0), "clock: year wraps up");
	dt_set(2, 29);
	datetime_update(JOY_BIT(UP));
	CHECK(dt_is(2024, 2, 1, 23, 0), "clock: day wraps after the 29th");
	datetime_update(JOY_BIT(DOWN));
	CHECK(dt_is(2024, 2, 29, 23, 0), "clock: day wraps down");
	datetime_update(JOY_BIT(LEFT));
	datetime_update(JOY_BIT(DOWN));
	CHECK(dt_is(2024, 1, 29, 23, 0), "clock: month down");
	datetime_update(JOY_BIT(DOWN));
	CHECK(dt_is(2024, 12, 29, 23, 0), "clock: month wraps down");
	datetime_update(JOY_BIT(UP));
	CHECK(dt_is(2024, 1, 29, 23, 0), "clock: month wraps up");
	datetime_update(JOY_BIT(RIGHT));
	datetime_update(JOY_BIT(RIGHT));
	datetime_update(JOY_BIT(UP));
	CHECK(dt_is(2024, 1, 29, 0, 0), "clock: hour wraps");
	datetime_update(JOY_BIT(RIGHT));
	datetime_update(JOY_BIT(DOWN));
	CHECK(dt_is(2024, 1, 29, 0, 59), "clock: minute wraps");
	/* B cancels: nothing set. */
	CHECK(datetime_update(JOY_BIT(B)) == UI_BACK && fake_calls == 0,
	      "clock: cancel sets nothing");

	/* A saves: the system clock gets the time with 0 seconds, the RTC
	 * ioctl fails on a regular file, the page stays with the message. */
	datetime_enter();
	dt_set(0, 2028);
	dt_set(1, 2);
	dt_set(2, 29);
	dt_set(3, 13);
	dt_set(4, 45);
	CHECK(datetime_update(JOY_BIT(A)) == UI_NONE, "clock: failed save stays");
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
	datetime_draw(&cv);
	for (int y = cv.h - 76; y < cv.h - 36; y++)
		for (int x = 0; x < cv.w; x++)
			red += px[y * cv.w + x] == COL_ERR;
	CHECK(red > 100, "clock: message not drawn (%d red pixels)", red);
	snprintf(msg, sizeof(msg), "%s/ui-datetime.raw", dir);
	write_file_atomic(msg, px, sizeof(px));
	datetime_update(JOY_BIT(UP));
	CHECK(!datetime_message()[0], "clock: an edit clears the message");
	CHECK(datetime_update(JOY_BIT(B)) == UI_BACK, "clock: B after a failure");

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
	romlist_menu_enter();
	romlist_menu_update(1u << 5 /* down */, path, sizeof(path));
	romlist_menu_draw(&cv);
	snprintf(path, sizeof(path), "%s/ui-romlist.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	ingame_menu_enter("Some Long Game Name (USA, Europe).gba");
	ingame_menu_update(1u << 5);
	ingame_menu_draw(&cv, game, 240, 240, 160);
	snprintf(path, sizeof(path), "%s/ui-ingame.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	/* With mGBA, Frame blend reads "gpSP only" and does not change. */
	rom_settings.core = CORE_MGBA;
	for (int i = 0; i < 5; i++)
		ingame_menu_update(JOY_BIT(DOWN));
	ingame_menu_update(JOY_BIT(A));
	ingame_menu_update(JOY_BIT(RIGHT));
	CHECK(rom_settings.blend == 0, "blend unchanged with mGBA");
	ingame_menu_draw(&cv, game, 240, 240, 160);
	snprintf(path, sizeof(path), "%s/ui-ingame-mgba.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	rom_settings.core = CORE_GPSP;
	ingame_menu_update(JOY_BIT(A));
	CHECK(rom_settings.blend == 1, "blend toggled with gpSP");
	rom_settings.blend = 0;

	settings_menu_enter(0);
	settings_menu_update(1u << 5);
	settings_menu_draw(&cv, NULL, 0, 0, 0);
	snprintf(path, sizeof(path), "%s/ui-settings.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	/* Volume, Brightness, CPU, Audio, Debug log, Date & time, System
	 * info, Power off, Back: no grid rows outside a game. */
	settings_menu_update(JOY_BIT(DOWN));
	settings_menu_update(JOY_BIT(DOWN));
	settings_menu_update(JOY_BIT(A));
	CHECK(settings.audio == AUDIO_SIMPLE, "settings: 4th row is Audio");
	settings.audio = settings_defaults.audio;
	settings_menu_update(JOY_BIT(DOWN));
	settings_menu_update(JOY_BIT(DOWN));
	CHECK(settings_menu_update(JOY_BIT(A)) == UI_DATETIME,
	      "settings: 6th row is Date & time");
	settings_menu_update(JOY_BIT(DOWN));
	CHECK(settings_menu_update(JOY_BIT(A)) == UI_SYSINFO,
	      "settings: 7th row is System info");
	settings_menu_update(JOY_BIT(DOWN));
	CHECK(settings_menu_update(JOY_BIT(A)) == UI_NONE,
	      "settings: 8th row is Power off (asks first)");
	settings_menu_update(JOY_BIT(DOWN));
	CHECK(settings_menu_update(JOY_BIT(A)) == UI_BACK,
	      "settings: 9th row is Back");
	settings_menu_enter(1);
	for (int i = 0; i < 5; i++)
		settings_menu_update(JOY_BIT(DOWN));
	CHECK(settings_menu_update(JOY_BIT(A)) == UI_BACK,
	      "in-game settings: 6th row is Back");

	/* In game: a picture with detail, GBA colors and the grid. */
	for (int y = 0; y < 160; y++)
		for (int x = 0; x < 240; x++) {
			int r = x * 31 / 239, g = y * 31 / 159, b = (x ^ y) & 31;

			game[y * 240 + x] = (uint16_t)(r << 11 | g << 6 | b);
		}
	rom_settings.color = COLOR_GBA;
	settings.grid = 1;
	fx_update();
	settings_menu_enter(1);
	settings_menu_draw(&cv, game, 240, 240, 160);
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
	test_marquee();
	test_ingame_restart(dir);
	test_romlist_folders(dir);
	test_ui(dir);
	test_sysinfo(dir);
	test_clock(dir);
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
	rom_settings.blend = 1;
	if (core_load(rom, CORE_MGBA, err, sizeof(err))) {
		CHECK(0, "blend: %s", err);
		return;
	}
	run_frames(2);
	core_options_changed();
	run_frames(2);
	core_unload();
	rom_settings.blend = 0;
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
	ingame_menu_enter("Game");
	for (int i = 0; i < 5; i++)
		ingame_menu_update(JOY_BIT(DOWN));
	ingame_menu_update(JOY_BIT(RIGHT));
	CHECK(rom_settings.core == CORE_MGBA, "restart with: core picked");
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_NONE,
	      "restart with: first A arms");
	CHECK(ingame_menu_update(JOY_BIT(A)) == UI_RESTART,
	      "restart with: second A confirms");
	ingame_menu_update(JOY_BIT(A));
	ingame_menu_update(JOY_BIT(RIGHT));
	ingame_menu_update(JOY_BIT(RIGHT));
	act = ingame_menu_update(JOY_BIT(A));
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
 * reset in each core on that save, then frame blending switched live,
 * the in-game menu's Restart with, and the resume states with each core.
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
	menu_restart_with(rom);
	state_rounds(data, CORE_GPSP, frames);
	state_rounds(data, CORE_MGBA, frames);
	state_other_core(data, frames);
	log_msg("core switch: %s (%d failures)", failures ? "FAILED" : "passed",
		failures);
	return failures ? 1 : 0;
}
