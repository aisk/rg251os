/*
 * Unit tests for the parts that do not need the device; run with
 * `frontend --selftest DIR`, e.g. under qemu-aarch64.
 */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio.h"
#include "battery.h"
#include "blit.h"
#include "core.h"
#include "cpuctl.h"
#include "draw.h"
#include "menu.h"
#include "fe.h"
#include "fx.h"
#include "input.h"
#include "resample.h"
#include "romlist.h"
#include "sleep.h"

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

	/* Orientation, as verified with hwtest: rotation 270 puts logical
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

	for (int mode = COLOR_GBA; mode < COLOR_COUNT; mode++) {
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
	CHECK(r.color == COLOR_OFF, "rom default");
	CHECK(rom_settings_parse(&r, "color=GBA-light\r\n") == 1 &&
	      r.color == COLOR_GBA_LIGHT, "rom parse");
	CHECK(rom_settings_parse(&r, "color=7\ncolor=sepia\n") == 0 &&
	      r.color == COLOR_GBA_LIGHT, "rom parse range");
	CHECK(r.core == CORE_GPSP, "core default gpsp");
	CHECK(rom_settings_parse(&r, "core=gpSP\n") == 1 && r.core == CORE_GPSP,
	      "core parse");
	CHECK(rom_settings_parse(&r, "core=vba\ncore=2\n") == 0 &&
	      r.core == CORE_GPSP, "core parse range");

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

	rom_settings_load("Some Game (USA).gba");
	CHECK(rom_settings.color == COLOR_OFF, "rom without file");
	rom_settings.color = COLOR_SP101;
	rom_settings_save();
	snprintf(path, sizeof(path), "%s/config/roms/Some Game (USA).txt", base);
	CHECK(access(path, F_OK) == 0, "per ROM file name");
	rom_settings_load(NULL);
	CHECK(rom_settings.color == COLOR_OFF, "no game");
	rom_settings_load("Some Game (USA).gba");
	CHECK(rom_settings.color == COLOR_SP101, "per ROM round trip");
	rom_settings_load("Other.gba");
	CHECK(rom_settings.color == COLOR_OFF, "other ROM unaffected");
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
	rom_settings_load(NULL);
	CHECK(rom_settings.core == CORE_GPSP, "no game: default core");
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

static void test_tone(void)
{
	unsigned len = 48000, zero = 0;
	int peak = 0;

	CHECK(audio_tone_sample(0, len, 48000, 440) == 0, "tone starts at 0");
	for (unsigned i = 1; i < len; i++) {
		int a = audio_tone_sample(i - 1, len, 48000, 440);
		int b = audio_tone_sample(i, len, 48000, 440);

		if ((a < 0) != (b < 0))
			zero++;
		if (abs(b) > peak)
			peak = abs(b);
	}
	/* 440 Hz: 880 sign changes per second, about -10 dBFS. */
	CHECK(zero >= 875 && zero <= 885, "tone zero crossings %u", zero);
	CHECK(peak > 9000 && peak < 10500, "tone peak %d", peak);
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
	char dir[300];
	struct romlist rl = { 0 };

	snprintf(dir, sizeof(dir), "%s/roms", base);
	mkdir(dir, 0755);
	for (size_t i = 0; i < ARRAY_SIZE(files); i++)
		touch(dir, files[i]);
	CHECK(romlist_scan(&rl, dir) == 3, "romlist found %d", rl.count);
	if (rl.count == 3) {
		CHECK(!strcmp(rl.names[0], "A.GBA"), "order 0: %s", rl.names[0]);
		CHECK(!strcmp(rl.names[1], "b.gba"), "order 1: %s", rl.names[1]);
		CHECK(!strcmp(rl.names[2], "z.Gba"), "order 2: %s", rl.names[2]);
	}
	romlist_free(&rl);
	snprintf(dir, sizeof(dir), "%s/empty-roms", base);
	mkdir(dir, 0755);
	CHECK(romlist_scan(&rl, dir) == 0, "empty dir");
	CHECK(romlist_scan(&rl, "/nonexistent/roms") < 0, "missing dir");
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

	settings_menu_enter(0);
	settings_menu_update(1u << 5);
	settings_menu_draw(&cv, NULL, 0, 0, 0);
	snprintf(path, sizeof(path), "%s/ui-settings.raw", dir);
	write_file_atomic(path, px, sizeof(px));
	settings.grid = 1;
	settings_menu_update(1u << 5);
	settings_menu_update(1u << 5);
	fx_update();
	settings_menu_draw(&cv, NULL, 0, 0, 0);
	snprintf(path, sizeof(path), "%s/ui-settings-grid.raw", dir);
	write_file_atomic(path, px, sizeof(px));

	/* In game: a picture with detail, GBA colors and the grid. */
	for (int y = 0; y < 160; y++)
		for (int x = 0; x < 240; x++) {
			int r = x * 31 / 239, g = y * 31 / 159, b = (x ^ y) & 31;

			game[y * 240 + x] = (uint16_t)(r << 11 | g << 6 | b);
		}
	rom_settings.color = COLOR_GBA;
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
	test_tone();
	test_log_dirs(dir);
	test_fx_tables();
	test_grid_pattern();
	test_grid_phases();
	test_pwrkey();
	test_romlist(dir);
	test_files(dir);
	test_settings(dir);
	test_battery(dir);
	test_sleep_sysfs(dir);
	test_cpuctl(dir);
	test_ui(dir);
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
 * mGBA -> gpSP -> mGBA in one process on the same .sav, then a RetroArch
 * .srm picked up by gpSP when there is no .sav (and left untouched).
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
	log_msg("core switch: %s (%d failures)", failures ? "FAILED" : "passed",
		failures);
	return failures ? 1 : 0;
}
