/*
 * Unit tests for the parts that do not need the device; run with
 * `frontend --selftest DIR`, e.g. under qemu-aarch64.
 */
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
#include "cpuctl.h"
#include "draw.h"
#include "menu.h"
#include "fe.h"
#include "fx.h"
#include "resample.h"
#include "romlist.h"

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
		const struct blit_fx fxs[2] = { { lit, NULL }, { lit, dark } };

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

#define CAL_TOL 0.02

static void test_drc(double drift)
{
	static int16_t silence[1200 * 2];
	struct audio_stats st;
	double acc = 0;
	unsigned xruns = 0;
	int fmin = 1 << 30, fmax = 0;

	audio_init_null(48000, drift);
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
	log_msg("selftest: drc drift %+.3f%%: device %.0f Hz, buffer %u, adj %.4f, "
		"fill %d..%d of target %u, underruns %u", drift * 100,
		audio_device_rate(), audio_buffer(), st.adj, fmin, fmax,
		audio_target(), xruns);
	CHECK(xruns == 0, "underruns with drift %f", drift);
	CHECK(fmin > (int)audio_target() / 4, "fill too low with drift %f", drift);
	CHECK(fabs(st.adj - 1) < 0.005, "drc saturated with drift %f", drift);
	CHECK(fmax + audio_device_rate() / 60.0186 <= audio_buffer() + 1,
	      "buffer overflow with drift %f", drift);
	CHECK(st.overflows == 0, "overflow with drift %f", drift);
	if (fabs(drift) < CAL_TOL) {
		/* Small drift: nominal rate, DRC absorbs the rest. */
		CHECK(audio_device_rate() == 48000, "device rate %f with drift %f",
		      audio_device_rate(), drift);
		CHECK(audio_buffer() == 4096, "buffer %u with drift %f",
		      audio_buffer(), drift);
	} else {
		/* A device playing far off its nominal rate is measured and
		 * resampled for, with the big fallback buffer. */
		CHECK(fabs(audio_device_rate() / (48000 * (1 + drift)) - 1) < 0.002,
		      "device rate %f not measured with drift %f",
		      audio_device_rate(), drift);
		CHECK(audio_buffer() == 8192, "buffer %u with drift %f",
		      audio_buffer(), drift);
	}
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
	const struct blit_fx fx = { lit, dark };
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
	CHECK(settings_format(&s, buf, sizeof(buf)) > 0, "format");
	CHECK(settings_parse(&t, buf) == 6, "parse back: %s", buf);
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
	rom_settings_load(NULL);
	settings = keep;
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
	test_drc(1.0);    /* the first hardware run: device at 2x */
	test_drc(-0.25);  /* device slower than nominal */
	test_fx_tables();
	test_grid_pattern();
	test_romlist(dir);
	test_files(dir);
	test_settings(dir);
	test_battery(dir);
	test_cpuctl(dir);
	test_ui(dir);
	log_msg("selftest: %s (%d failures)", failures ? "FAILED" : "passed",
		failures);
	return failures ? 1 : 0;
}
