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
#include "blit.h"
#include "draw.h"
#include "menu.h"
#include "fe.h"
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
			    cases[k].sw, cases[k].sh, cases[k].scale);
		blit_reference(b, dpitch, nw, nh, cases[k].rot, src,
			       cases[k].spitch, cases[k].sw, cases[k].sh,
			       cases[k].scale);
		for (int y = 0; y < nh; y++)
			diff += memcmp(a + y * dpitch, b + y * dpitch, nw * 2) != 0;
		CHECK(!diff, "blit case %zu: %d rows differ", k, diff);
	}

	/* Orientation, as verified with hwtest: rotation 270 puts logical
	 * (0,0), the user's top-left, at native column 0 of the last row. */
	blit_rotate(a, dpitch, 320, 480, 270, src, 256, 240, 160, 2);
	CHECK(a[479 * dpitch + 0] == src[0], "top-left pixel not at native (0,479)");
	CHECK(a[478 * dpitch + 1] == src[0], "2x scaling of top-left pixel");
	CHECK(a[0 * dpitch + 319] == src[159 * 256 + 239],
	      "bottom-right pixel not at native (319,0)");
	CHECK(a[479 * dpitch + 2] == src[256], "logical y runs along native x");

	t0 = now_ns();
	for (int i = 0; i < 50; i++)
		blit_rotate(a, dpitch, 320, 480, 270, src, 256, 240, 160, 2);
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
	log_msg("selftest: drc drift %+.3f%%: adj %.4f, fill %d..%d of target %u, "
		"underruns %u", drift * 100, st.adj, fmin, fmax, audio_target(), xruns);
	CHECK(xruns == 0, "underruns with drift %f", drift);
	CHECK(fmin > (int)audio_target() / 4, "fill too low with drift %f", drift);
	CHECK(fabs(st.adj - 1) < 0.005, "drc saturated with drift %f", drift);
	CHECK(fmax + 800 <= 2880, "buffer overflow with drift %f", drift);
	CHECK(st.overflows == 0, "overflow with drift %f", drift);
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
	struct settings s = { 255, 50 };

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

	CHECK(settings_parse(&s, "volume=200\nbrightness=70\nbogus=1\n") == 2,
	      "settings parse");
	CHECK(s.volume == 200 && s.brightness == 70, "settings values");
	settings_parse(&s, "volume=999\nbrightness=1");
	CHECK(s.volume == 200 && s.brightness == 70, "settings range check");
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
	test_romlist(dir);
	test_files(dir);
	test_ui(dir);
	log_msg("selftest: %s (%d failures)", failures ? "FAILED" : "passed",
		failures);
	return failures ? 1 : 0;
}
