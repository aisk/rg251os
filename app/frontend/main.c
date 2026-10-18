/*
 * frontend: the RG351P GBA frontend, running as PID 1 (/sbin/init).
 *
 * One static program: mounts, logging, KMS output, evdev input, ALSA
 * audio and a statically linked libretro core (mGBA). The main loop is
 * driven by page flip events: whenever the previous frame is on screen,
 * one retro_run is done, the frame is scaled 2x and rotated straight into
 * the back buffer and flipped, so emulation runs at the panel's 60.02 Hz
 * and audio is resampled to match (see audio.c).
 *
 * Status LED (one GPIO, red or green):
 *   red steady      starting up (set as soon as PID 1 runs)
 *   green steady    up: first frame on screen and the startup log written
 *                   to <data>/log/frontend.txt
 *   red/green blink fatal error, shown on screen if possible and logged;
 *                   waiting for the power key
 *   red steady      powering off (saving)
 * Before PID 1 starts the kernel shows its heartbeat trigger (see dts).
 *
 * Logs on the GBA partition (log/): frontend.txt (this program; the
 * previous boot's is frontend-prev.txt), dmesg-5s.txt, dmesg-20s.txt and
 * dmesg-poweroff.txt.
 *
 * Keys: SELECT+START in game opens the in-game menu (the RG351P has no
 * menu/function button; SELECT is also the hotkey enabler in AmberELEC
 * and ROCKNIX). The power key saves and powers off at any time.
 *
 * Not PID 1 (testing on a PC or under qemu-user):
 *   frontend --selftest DIR            unit tests, using DIR as scratch
 *   frontend --headless DATA ROM N     run N frames without display or
 *                                      sound device, DATA as GBA partition
 *   frontend --data DIR                normal UI with DATA from DIR
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "audio.h"
#include "blit.h"
#include "core.h"
#include "display.h"
#include "draw.h"
#include "fe.h"
#include "input.h"
#include "libretro.h"
#include "menu.h"
#include "perf.h"

#define BIT(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)
#define HOTKEY (BIT(SELECT) | BIT(START))
#define PERF_INTERVAL_NS (10 * 1000000000LL)
#define MAX_POLL 24

int selftest(const char *dir);

enum state { ST_ROMLIST, ST_GAME, ST_MENU, ST_FATAL, ST_POWEROFF };

static struct {
	enum state st;
	uint16_t canvas_px[480 * 320];
	struct canvas cv;
	char rom_path[512];
	char rom_name[128];
	uint32_t suppress;     /* held buttons hidden until released */
	uint32_t prev;
	int led_green;
	int dumped5, dumped20;
	int64_t poweroff_at;
	int64_t next_rescan, next_drm_try, next_blink, next_perf;
	int blink;
	char fatal_title[64], fatal_msg[256];
	struct settings menu_settings;
	struct perf_series p_run, p_blit, p_audio;
	uint64_t perf_frames;
	unsigned missed;
	unsigned xrun_total;
	unsigned reports;
	struct audio_stats last_as;
} m = { .cv = { .w = 480, .h = 320 } };

/* ------------------------------------------------------- input events */

void on_power_key(void)
{
	if (m.poweroff_at)
		return;
	log_msg("power key pressed");
	m.st = ST_POWEROFF;
	/* Leave time for one frame with the message. */
	m.poweroff_at = now_ns() + 150000000LL;
}

void on_volume_key(int dir)
{
	int v = settings.volume + dir * 8;

	log_verbose("volume key %+d", dir);
	settings.volume = v < 0 ? 0 : v > 255 ? 255 : v;
	audio_set_volume(settings.volume);
}

void on_jack(int inserted)
{
	audio_jack(inserted);
}

/* -------------------------------------------------------------- states */

static void fatal(const char *title, const char *msg)
{
	snprintf(m.fatal_title, sizeof(m.fatal_title), "%s", title);
	snprintf(m.fatal_msg, sizeof(m.fatal_msg), "%s", msg);
	log_msg("FATAL: %s: %s", title, msg);
	log_sync();
	if (core_loaded())
		core_unload();
	audio_pause();
	m.st = ST_FATAL;
}

static void do_poweroff(void)
{
	led_set_red(1);
	audio_pause();
	core_unload();
	if (memcmp(&settings, &m.menu_settings, sizeof(settings)))
		settings_save();
	log_msg("uptime %lld ms, dumping dmesg", (long long)boot_ms());
	log_sync();
	klog_dump("dmesg-poweroff.txt");
	sys_poweroff();
}

static void start_game(const char *path)
{
	const struct display_info *di = display_info();
	const char *base = strrchr(path, '/');
	char err[128];

	base = base ? base + 1 : path;
	snprintf(m.rom_path, sizeof(m.rom_path), "%s", path);
	snprintf(m.rom_name, sizeof(m.rom_name), "%.127s", base);
	log_msg("starting %s", path);
	if (core_load(path, err, sizeof(err))) {
		romlist_menu_message(err);
		log_sync();
		return;
	}
	audio_init();
	audio_set_source(core_info()->sample_rate, core_info()->fps,
			 display_ready() ? di->refresh_hz : 60.0);
	m.st = ST_GAME;
	m.suppress = input_buttons();
	m.menu_settings = settings;
	display_take_missed();
	m.next_perf = now_ns() + PERF_INTERVAL_NS;
	log_sync();
}

static void quit_game(void)
{
	audio_pause();
	core_unload();
	if (memcmp(&settings, &m.menu_settings, sizeof(settings)))
		settings_save();
	romlist_menu_enter();
	m.st = ST_ROMLIST;
	m.suppress = input_buttons();
	log_sync();
}

static void open_menu(uint32_t buttons)
{
	audio_pause();
	core_save_sync();
	ingame_menu_enter(m.rom_name);
	m.menu_settings = settings;
	m.suppress = buttons;
	m.st = ST_MENU;
	log_msg("in-game menu");
	log_sync();
}

static void resume_game(uint32_t buttons)
{
	if (memcmp(&settings, &m.menu_settings, sizeof(settings))) {
		settings_save();
		m.menu_settings = settings;
	}
	m.suppress = buttons;
	m.st = ST_GAME;
	display_take_missed();
}

/* ------------------------------------------------------------- frames */

/* SELECT+START (one of them newly pressed), or Esc/F1 on a keyboard. */
static int menu_hotkey(uint32_t buttons)
{
	return ((buttons & HOTKEY) == HOTKEY && (~m.prev & HOTKEY)) ||
	       ((buttons & BTN_BIT_MENU) && !(m.prev & BTN_BIT_MENU));
}

static void perf_report(void)
{
	char run[48], blit[48], aud[48];
	struct audio_stats as;
	long khz = cpu_freq_khz(), mc = soc_temp_mc();
	char temp[24] = "n/a";

	if (mc != -1)
		snprintf(temp, sizeof(temp), "%.1f C", mc / 1000.0);

	audio_take_stats(&as);
	m.xrun_total += as.xruns;
	m.last_as = as;
	perf_format(&m.p_run, run, sizeof(run));
	perf_format(&m.p_blit, blit, sizeof(blit));
	perf_format(&m.p_audio, aud, sizeof(aud));
	log_msg("perf: %llu frames, ms avg/p99/max: run %s blit %s audio %s; "
		"missed vblanks %u, underruns %u, overflow %u frames, fill %d..%d, "
		"drc %.4f, cpu0 %ld MHz, temp %s",
		(unsigned long long)m.perf_frames, run, blit, aud, m.missed,
		as.xruns, as.overflows, as.fill_min, as.fill_max, as.adj,
		khz > 0 ? khz / 1000 : -1, temp);
	m.perf_frames = 0;
	m.missed = 0;
	if (++m.reports % 6 == 0 && fe_is_init)
		log_sync_async();
}

static void blit_canvas(void)
{
	const struct display_info *di = display_info();
	uint16_t *dst;
	int pitch;

	dst = display_backbuf(&pitch);
	blit_rotate(dst, pitch, di->native_w, di->native_h, di->rotation,
		    m.cv.px, m.cv.w, m.cv.w, m.cv.h, 1);
	display_present();
}

static void game_frame(uint32_t buttons, int64_t now)
{
	const struct display_info *di = display_info();
	const uint16_t *fr;
	int64_t t0, t1, t2, t3, push;
	int fw, fh, fp, pitch, scale;
	uint16_t *dst;

	if (menu_hotkey(buttons)) {
		open_menu(buttons);
		return;
	}
	t0 = now_ns();
	core_run(buttons & ~m.suppress & 0xffff);
	t1 = now_ns();
	push = audio_take_time_ns();
	audio_flush();
	t2 = now_ns();

	fr = core_frame(&fw, &fh, &fp);
	if (display_ready()) {
		dst = display_backbuf(&pitch);
		if (fr) {
			scale = di->logical_w / fw < di->logical_h / fh ?
				di->logical_w / fw : di->logical_h / fh;
			blit_rotate(dst, pitch, di->native_w, di->native_h,
				    di->rotation, fr, fp, fw, fh, scale < 1 ? 1 : scale);
		}
		t3 = now_ns();
		display_present();
	} else {
		t3 = t2;
	}

	perf_add(&m.p_run, t1 - t0 - push);
	perf_add(&m.p_audio, audio_take_time_ns() + push);
	perf_add(&m.p_blit, t3 - t2);
	m.perf_frames++;
	m.missed += display_take_missed();
	if (now >= m.next_perf) {
		perf_report();
		m.next_perf = now + PERF_INTERVAL_NS;
	}
	core_save_tick(now);
}

static void frame(int64_t now)
{
	uint32_t buttons = input_buttons(), pressed;
	const uint16_t *fr;
	int fw, fh, fp;
	char path[512];

	m.suppress &= buttons;
	switch (m.st) {
	case ST_GAME:
		game_frame(buttons, now);
		break;
	case ST_ROMLIST:
		pressed = ui_repeat(buttons & ~m.suppress, now);
		if (romlist_menu_update(pressed, path, sizeof(path)) == UI_START_ROM)
			start_game(path);
		if (m.st == ST_ROMLIST) {
			romlist_menu_draw(&m.cv);
			blit_canvas();
		}
		break;
	case ST_MENU:
		pressed = ui_repeat(buttons & ~m.suppress, now);
		if (menu_hotkey(buttons)) {
			resume_game(buttons);
			break;
		}
		switch (ingame_menu_update(pressed)) {
		case UI_RESUME:
			resume_game(buttons);
			break;
		case UI_QUIT:
			quit_game();
			break;
		case UI_POWEROFF:
			on_power_key();
			break;
		}
		fr = core_frame(&fw, &fh, &fp);
		ingame_menu_draw(&m.cv, fr, fp, fw, fh);
		blit_canvas();
		break;
	case ST_FATAL:
		message_draw(&m.cv, m.fatal_title, m.fatal_msg, COL_ERR);
		blit_canvas();
		break;
	case ST_POWEROFF:
		message_draw(&m.cv, "Powering off", "Saving...", COL_HI);
		blit_canvas();
		break;
	}
	m.prev = buttons;
}

/* --------------------------------------------------------------- setup */

static void startup(void)
{
	struct utsname u;
	char path[300];

	m.cv.px = m.canvas_px;
	sys_early_init();
	led_set_red(1);
	uname(&u);
	log_msg("frontend started at %lld ms (CLOCK_BOOTTIME), pid %d, kernel %s",
		(long long)boot_ms(), (int)getpid(), u.release);

	if (sys_mount_data()) {
		fatal("No GBA partition",
		      "Could not mount /dev/mmcblk0p3 (FAT). ROMs, saves and logs "
		      "live there.");
	} else {
		sys_make_dirs();
		snprintf(path, sizeof(path), "%s/log/frontend.txt", fe_data_dir);
		log_open_file(path);
	}
	settings_load();
	m.menu_settings = settings;
	backlight_init(settings.brightness);
	cpu_info_log();
	if (display_init())
		m.next_drm_try = now_ns() + 200000000LL;
	input_init();
	audio_init();
	if (m.st != ST_FATAL) {
		romlist_menu_enter();
		m.st = ST_ROMLIST;
	}
	log_msg("startup done at %lld ms", (long long)boot_ms());
	log_sync();
}

static void timers(int64_t now)
{
	int64_t up = boot_ms();
	pid_t pid;
	int st;

	while ((pid = sys_reap(&st)) > 0)
		core_child_exited(pid, st);

	if (!m.dumped5 && up >= 5000) {
		m.dumped5 = 1;
		klog_dump_async("dmesg-5s.txt");
	}
	if (!m.dumped20 && up >= 20000) {
		m.dumped20 = 1;
		klog_dump_async("dmesg-20s.txt");
		log_sync();
	}
	if (!display_ready() && now >= m.next_drm_try) {
		if (display_init()) {
			m.next_drm_try = now + 200000000LL;
			if (up > 15000 && m.st != ST_FATAL)
				fatal("No display", "The DRM device did not come up.");
		}
	}
	if (now >= m.next_rescan) {
		input_rescan();
		audio_init();
		m.next_rescan = now + 2000000000LL;
	}
	if (!m.led_green && m.st != ST_FATAL && display_frames() > 0) {
		log_msg("up: first frame shown at %lld ms", (long long)up);
		log_sync();
		led_set_red(0);
		m.led_green = 1;
	}
	if (m.st == ST_FATAL && now >= m.next_blink) {
		m.blink ^= 1;
		led_set_red(m.blink);
		m.next_blink = now + 500000000LL;
	}
	if (m.poweroff_at && now >= m.poweroff_at)
		do_poweroff();
}

static void run(void)
{
	for (;;) {
		struct pollfd pfd[MAX_POLL];
		int64_t now = now_ns();
		int n = 0, timeout;

		timers(now);
		if (display_can_draw())
			frame(now);

		if (display_ready()) {
			pfd[n].fd = display_fd();
			pfd[n].events = POLLIN;
			pfd[n++].revents = 0;
		}
		n += input_pollfds(pfd + n, MAX_POLL - n);
		timeout = display_poll_timeout(now_ns());
		if (timeout > 100)
			timeout = 100;
		if (poll(pfd, n, timeout) <= 0)
			continue;
		if (display_ready()) {
			if (pfd[0].revents & POLLIN)
				display_handle_events();
			input_dispatch(pfd + 1, n - 1);
		} else {
			input_dispatch(pfd, n);
		}
	}
}

/* --------------------------------------------------------- headless */

/*
 * Runs a ROM without display or sound device (for qemu-user): exercises
 * core glue, the scaler, the resampler and rate control against a
 * simulated codec running 0.1% fast, and the save path.
 */
static int headless(const char *data, const char *rom, long frames)
{
	static uint16_t native[320 * 480];
	char err[128], path[300];
	const struct audio_stats *as = &m.last_as;
	const uint16_t *fr;
	int fw = 0, fh = 0, fp = 0, st;
	pid_t pid;

	snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", data);
	sys_make_dirs();
	snprintf(path, sizeof(path), "%s/log/frontend.txt", fe_data_dir);
	log_open_file(path);
	audio_init_null(48000, 0.001);
	if (core_load(rom, err, sizeof(err))) {
		log_msg("headless: %s", err);
		return 1;
	}
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	for (long i = 0; i < frames; i++) {
		/* Tap A for 10 frames every 2 s of emulated time. */
		uint32_t b = i % 120 < 10 ? BIT(A) : 0;
		int64_t t0 = now_ns(), t1, t2, push;

		core_run(b);
		t1 = now_ns();
		push = audio_take_time_ns();
		audio_flush();
		fr = core_frame(&fw, &fh, &fp);
		t2 = now_ns();
		if (fr)
			blit_rotate(native, 320, 320, 480, 270, fr, fp, fw, fh, 2);
		perf_add(&m.p_run, t1 - t0 - push);
		perf_add(&m.p_audio, audio_take_time_ns() + push);
		perf_add(&m.p_blit, now_ns() - t2);
		m.perf_frames++;
		/* Fake time for the save timer: 60 frames per second. */
		core_save_tick((int64_t)i * 16661000LL);
		while ((pid = sys_reap(&st)) > 0)
			core_child_exited(pid, st);
		if (i % 600 == 599) {
			perf_report();
			/* The first 20 s are warmup for the rate control. */
			if (i < 1200)
				m.xrun_total = 0;
		}
	}
	if (m.perf_frames)
		perf_report();
	log_msg("headless: %ld frames, frame %dx%d pitch %d, final drc %.4f, "
		"fill %d..%d, underruns after warmup %u",
		frames, fw, fh, fp, as->adj, as->fill_min, as->fill_max,
		m.xrun_total);
	core_unload();
	snprintf(path, sizeof(path), "%s/native.raw", fe_data_dir);
	write_file_atomic(path, native, sizeof(native));
	return m.xrun_total ? 2 : 0;
}

int main(int argc, char **argv)
{
	fe_is_init = getpid() == 1;
	if (!fe_is_init) {
		if (argc == 3 && !strcmp(argv[1], "--selftest"))
			return selftest(argv[2]);
		if (argc == 5 && !strcmp(argv[1], "--headless"))
			return headless(argv[2], argv[3], strtol(argv[4], NULL, 0));
		if (argc == 3 && !strcmp(argv[1], "--data")) {
			snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", argv[2]);
		} else if (argc != 1) {
			fprintf(stderr, "usage: %s [--selftest DIR | --headless DATA "
				"ROM FRAMES | --data DIR]\n", argv[0]);
			return 1;
		}
	}
	startup();
	run();
}
