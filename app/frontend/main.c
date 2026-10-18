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
 *   green, short red flash every 2 s
 *                   battery low (below 10% and not charging)
 *   red/green blink fatal error, shown on screen if possible and logged;
 *                   waiting for the power key
 *   red steady      powering off (saving)
 * Before PID 1 starts the kernel shows its heartbeat trigger (see dts).
 *
 * Logs on the GBA partition: log/boot-NNNN/ per boot (the last 20 are
 * kept, log/LATEST.txt names the newest, see sys_log_init) with
 * frontend.txt (this program), dmesg-5s.txt, dmesg-20s.txt,
 * dmesg-game<N>-30s.txt and dmesg-game<N>-end.txt (each game),
 * dmesg-poweroff.txt; and log/perf.csv across boots (per game and CPU
 * frequency timing, power and audio statistics, appended at the end of
 * every game, see cpuctl.c).
 *
 * CPU frequency: schedutil throughout; in game cpuctl.c sets a floor from
 * the frame times (CPU mode setting: adaptive, max or a fixed OPP).
 *
 * Keys: SELECT+START in game opens the in-game menu (the RG351P has no
 * menu/function button; SELECT is also the hotkey enabler in AmberELEC
 * and ROCKNIX). SELECT in the ROM list opens the settings. The power key
 * saves and powers off at any time. See menu.c for the menus.
 *
 * Settings (settings.c) are saved when leaving the settings screen in the
 * ROM list, on resume, quit and power off, whenever they differ from what
 * was last loaded or saved.
 *
 * Not PID 1 (testing on a PC or under qemu-user):
 *   frontend --selftest DIR            unit tests, using DIR as scratch
 *   frontend --headless DATA ROM N     run N frames without display or
 *                                      sound device, DATA as GBA partition
 *                                      (core from DATA's ROM settings)
 *   frontend --core-switch DATA N      mGBA, gpSP, mGBA on one save in one
 *                                      process, N frames each
 *   frontend --data DIR                normal UI with DATA from DIR
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "battery.h"
#include "blit.h"
#include "core.h"
#include "cpuctl.h"
#include "display.h"
#include "draw.h"
#include "fe.h"
#include "fx.h"
#include "input.h"
#include "libretro.h"
#include "menu.h"
#include "perf.h"

#define BIT(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)
#define HOTKEY (BIT(SELECT) | BIT(START))
#define PERF_INTERVAL_NS (10 * 1000000000LL)
#define MAX_POLL 24

int selftest(const char *dir);
int selftest_core_switch(const char *data, long frames);

enum state {
	ST_ROMLIST, ST_GAME, ST_MENU, ST_SETTINGS, ST_FATAL, ST_POWEROFF
};

static struct {
	enum state st;
	uint16_t canvas_px[480 * 320];
	struct canvas cv;
	char rom_path[512];
	char rom_name[128];
	uint32_t suppress;     /* held buttons hidden until released */
	uint32_t prev;
	int led_green, led_red;
	int dumped5, dumped20;
	int64_t poweroff_at;
	int64_t next_rescan, next_drm_try, next_blink, next_perf;
	int blink;
	char fatal_title[64], fatal_msg[256];
	enum state settings_from;  /* ST_ROMLIST or ST_MENU */
	/* As last loaded or saved, to save only what changed. */
	struct settings saved;
	struct rom_settings saved_rom;
	struct perf_series p_run, p_blit, p_audio;
	uint64_t perf_frames;
	unsigned missed;
	unsigned xrun_total;
	unsigned reports;
	int genpd_logged;
	struct audio_stats last_as;
	unsigned games;        /* started this boot, numbers the dmesg dumps */
	uint64_t game_frames;  /* frames of the current game */
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

static void save_settings(void)
{
	if (memcmp(&settings, &m.saved, sizeof(settings))) {
		settings_save();
		m.saved = settings;
	}
	if (memcmp(&rom_settings, &m.saved_rom, sizeof(rom_settings))) {
		rom_settings_save();
		m.saved_rom = rom_settings;
	}
}

/* Game statistics out and the learned start OPP into the ROM settings. */
static void end_game_stats(void)
{
	if (cpuctl_in_game())
		rom_settings.cpu_start = cpuctl_game_end();
}

/* Audio summary and a kernel log dump (XRUN / hw_ptr reports) at the end
 * of a game. Before audio_pause, while the session numbers are intact. */
static void game_end_logs(const char *why)
{
	char name[48];

	if (!core_loaded())
		return;
	audio_summary(why);
	/* Without debug logging only a fatal error keeps a kernel log (and
	 * power off its own dmesg-poweroff.txt). */
	if (!log_debug() && strcmp(why, "fatal"))
		return;
	snprintf(name, sizeof(name), "dmesg-game%u-end.txt", m.games);
	klog_dump(name);
}

static void fatal(const char *title, const char *msg)
{
	snprintf(m.fatal_title, sizeof(m.fatal_title), "%s", title);
	snprintf(m.fatal_msg, sizeof(m.fatal_msg), "%s", msg);
	log_msg("FATAL: %s: %s", title, msg);
	log_sync();
	game_end_logs("fatal");
	if (core_loaded())
		core_unload();
	end_game_stats();
	audio_pause();
	m.st = ST_FATAL;
}

static void do_poweroff(void)
{
	led_set_red(1);
	game_end_logs("power off");
	audio_pause();
	core_unload();
	end_game_stats();
	save_settings();
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
	/* The ROM's settings first: they pick the core. */
	rom_settings_load(m.rom_name);
	m.saved_rom = rom_settings;
	log_msg("starting %s with %s", path, core_names[rom_settings.core]);
	if (core_load(path, rom_settings.core, err, sizeof(err))) {
		rom_settings_load(NULL);
		m.saved_rom = rom_settings;
		romlist_menu_message(err);
		log_sync();
		return;
	}
	audio_init();
	audio_set_source(core_info()->sample_rate, core_info()->fps,
			 display_ready() ? di->refresh_hz : 60.0);
	m.games++;
	m.game_frames = 0;
	fx_update();
	cpuctl_game_start(m.rom_name, display_ready() ? di->refresh_hz : 60.0,
			  rom_settings.cpu_start, now_ns());
	m.st = ST_GAME;
	m.suppress = input_buttons();
	display_take_missed();
	m.next_perf = now_ns() + PERF_INTERVAL_NS;
	log_sync();
}

static void quit_game(void)
{
	game_end_logs("quit");
	audio_pause();
	core_unload();
	end_game_stats();
	save_settings();
	rom_settings_load(NULL);
	m.saved_rom = rom_settings;
	fx_update();
	romlist_menu_enter();
	m.st = ST_ROMLIST;
	m.suppress = input_buttons();
	log_sync();
}

/* After the core was changed in the in-game menu: the save is written
 * and the game starts again with the new core. */
static void restart_game(void)
{
	char path[sizeof(m.rom_path)];

	snprintf(path, sizeof(path), "%s", m.rom_path);
	log_msg("restarting with %s", core_names[rom_settings.core]);
	game_end_logs("restart");
	audio_pause();
	core_unload();
	end_game_stats();
	save_settings();
	start_game(path);
	if (m.st != ST_GAME) {
		romlist_menu_enter();
		m.st = ST_ROMLIST;
		m.suppress = input_buttons();
	}
}

static void open_menu(uint32_t buttons)
{
	audio_pause();
	cpuctl_game_pause();
	core_save_sync();
	ingame_menu_enter(m.rom_name);
	m.suppress = buttons;
	m.st = ST_MENU;
	log_msg("in-game menu");
	log_sync();
}

static void resume_game(uint32_t buttons)
{
	save_settings();
	m.suppress = buttons;
	m.st = ST_GAME;
	display_take_missed();
	cpuctl_game_resume();
}

static void open_settings(uint32_t buttons)
{
	m.settings_from = m.st;
	settings_menu_enter(m.st == ST_MENU);
	m.suppress = buttons;
	m.st = ST_SETTINGS;
}

static void close_settings(uint32_t buttons)
{
	if (m.settings_from == ST_ROMLIST)
		save_settings();
	m.suppress = buttons;
	m.st = m.settings_from;
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
	char run[48], blit[48], aud[48], bat[80] = "n/a", cpu[96];
	const struct battery *b = battery_get();
	struct audio_stats as;
	long khz, mc;
	char temp[24] = "n/a";

	if (!log_debug()) {
		/* Only the counters (per game summary, headless result). */
		audio_take_stats(&as);
		m.xrun_total += as.xruns;
		m.last_as = as;
		m.perf_frames = 0;
		m.missed = 0;
		return;
	}
	khz = cpu_freq_khz();
	mc = soc_temp_mc();
	if (mc != -1)
		snprintf(temp, sizeof(temp), "%.1f C", mc / 1000.0);
	/* RK817 current is negative while discharging. */
	if (b->valid)
		snprintf(bat, sizeof(bat), "%d%% %.3f V %+.0f mA %.0f mW%s",
			 b->capacity, b->uv / 1e6, b->ua / 1e3,
			 (b->ua < 0 ? -b->ua : b->ua) * (double)b->uv / 1e9,
			 b->online ? " (charger)" : "");
	cpuctl_format(cpu, sizeof(cpu));

	audio_take_stats(&as);
	m.xrun_total += as.xruns;
	m.last_as = as;
	perf_format(&m.p_run, run, sizeof(run));
	perf_format(&m.p_blit, blit, sizeof(blit));
	perf_format(&m.p_audio, aud, sizeof(aud));
	log_msg("perf: %llu frames, ms avg/p99/max: run %s blit %s audio %s; "
		"missed vblanks %u, underruns %u, overflow %u frames, fill %d..%d, "
		"drc %.4f, codec %.0f Hz, cpu0 %ld MHz, %s, temp %s, bat %s, "
		"backlight %d%%",
		(unsigned long long)m.perf_frames, run, blit, aud, m.missed,
		as.xruns, as.overflows, as.fill_min, as.fill_max, as.adj,
		as.hw_rate, khz > 0 ? khz / 1000 : -1, cpu, temp, bat,
		settings.brightness);
	if (!m.genpd_logged) {
		m.genpd_logged = 1;
		genpd_log("in game");
	}
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
		    m.cv.px, m.cv.w, m.cv.w, m.cv.h, 1, NULL);
	display_present();
}

static void game_frame(uint32_t buttons, int64_t now)
{
	const struct display_info *di = display_info();
	const uint16_t *fr;
	int64_t t0, t1, t2, t3, push;
	int fw, fh, fp, pitch, scale;
	unsigned missed;
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
				    di->rotation, fr, fp, fw, fh, scale < 1 ? 1 : scale,
				    fx_current());
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
	missed = display_take_missed();
	m.missed += missed;
	/* Work: CPU time until the frame is ready for the flip. */
	cpuctl_game_frame(t3 - t0, missed, now);
	if (now >= m.next_perf) {
		perf_report();
		m.next_perf = now + PERF_INTERVAL_NS;
	}
	core_save_tick(now);
	if (++m.game_frames == 30 * 60 && log_debug()) {
		char name[48];

		/* Early XRUN / hw_ptr reports, before the ring buffer wraps. */
		snprintf(name, sizeof(name), "dmesg-game%u-30s.txt", m.games);
		klog_dump_async(name);
	}
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
		switch (romlist_menu_update(pressed, path, sizeof(path))) {
		case UI_START_ROM:
			start_game(path);
			break;
		case UI_SETTINGS:
			open_settings(buttons);
			break;
		}
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
		case UI_SETTINGS:
			open_settings(buttons);
			break;
		case UI_QUIT:
			quit_game();
			break;
		case UI_RESTART:
			restart_game();
			break;
		case UI_POWEROFF:
			on_power_key();
			break;
		}
		if (m.st != ST_MENU)
			break;
		fr = core_frame(&fw, &fh, &fp);
		ingame_menu_draw(&m.cv, fr, fp, fw, fh);
		blit_canvas();
		break;
	case ST_SETTINGS:
		pressed = ui_repeat(buttons & ~m.suppress, now);
		if (m.settings_from == ST_MENU && menu_hotkey(buttons)) {
			resume_game(buttons);
			break;
		}
		if (settings_menu_update(pressed) == UI_BACK)
			close_settings(buttons);
		fr = NULL;
		fw = fh = fp = 0;
		if (m.settings_from == ST_MENU)
			fr = core_frame(&fw, &fh, &fp);
		if (m.st == ST_SETTINGS)
			settings_menu_draw(&m.cv, fr, fp, fw, fh);
		else if (m.st == ST_MENU)
			ingame_menu_draw(&m.cv, fr, fp, fw, fh);
		else
			romlist_menu_draw(&m.cv);
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

/* CLOCK_REALTIME comes from the RK817 RTC (RTC_HCTOSYS) if it was set. */
static void clock_log(void)
{
	struct timespec ts;
	struct tm tm;
	char buf[32] = "?";

	clock_gettime(CLOCK_REALTIME, &ts);
	if (gmtime_r(&ts.tv_sec, &tm))
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
	log_msg("clock: realtime %s%s", buf,
		ts.tv_sec > 1704067200 ? "" : " (RTC not set, perf.csv uses boot time)");
}

static void startup(void)
{
	struct utsname u;
	char path[400];
	unsigned boot;

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
		boot = sys_log_init();
		snprintf(path, sizeof(path), "%s/frontend.txt", fe_log_dir);
		log_open_file(path);
		log_msg("log: boot %u, logging to %s", boot, fe_log_dir);
	}
	settings_load();
	m.saved = settings;
	rom_settings_load(NULL);
	m.saved_rom = rom_settings;
	fx_update();
	backlight_init(settings.brightness);
	cpu_info_log();
	cpuctl_init(NULL);
	cpuctl_set_mode(settings.cpu);
	clock_log();
	battery_init("/sys/class/power_supply");
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

	if (!m.dumped5 && up >= 5000 && log_debug()) {
		m.dumped5 = 1;
		klog_dump_async("dmesg-5s.txt");
		genpd_log("5 s");
	}
	if (!m.dumped20 && up >= 20000 && log_debug()) {
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
	battery_poll(now);
	if (!m.led_green && m.st != ST_FATAL && display_frames() > 0) {
		log_msg("up: first frame shown at %lld ms", (long long)up);
		log_sync();
		led_set_red(0);
		m.led_green = 1;
	}
	if (m.led_green && m.st != ST_FATAL && m.st != ST_POWEROFF) {
		/* Low battery: a 250 ms red flash every 2 s. */
		int red = battery_low() && now / 1000000 % 2000 < 250;

		if (red != m.led_red) {
			led_set_red(red);
			m.led_red = red;
		}
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
	/* Settings from DATA, so effects can be exercised too. */
	settings_load();
	rom_settings_load(strrchr(rom, '/') ? strrchr(rom, '/') + 1 : rom);
	fx_update();
	/* FE_AUDIO=simple: the default path with a realistic 50 ppm codec
	 * drift (it has no rate control); otherwise adaptive with 0.1%. */
	{
		const char *mode = getenv("FE_AUDIO");

		settings.audio = mode && !strcmp(mode, "simple") ? AUDIO_SIMPLE :
				 AUDIO_ADAPTIVE;
		audio_init_null(48000, settings.audio == AUDIO_SIMPLE ? 0.00005 :
				0.001);
	}
	/* Simulated cpufreq: real work times, fake 60 Hz clock. */
	snprintf(path, sizeof(path), "%s/fake-cpufreq", fe_data_dir);
	cpuctl_fake_sysfs(path);
	cpuctl_init(path);
	cpuctl_set_mode(settings.cpu);
	if (core_load(rom, rom_settings.core, err, sizeof(err))) {
		log_msg("headless: %s", err);
		return 1;
	}
	audio_set_source(core_info()->sample_rate, core_info()->fps, 60.0186);
	cpuctl_game_start(strrchr(rom, '/') ? strrchr(rom, '/') + 1 : rom,
			  60.0186, rom_settings.cpu_start, 0);
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
			blit_rotate(native, 320, 320, 480, 270, fr, fp, fw, fh, 2,
				    fx_current());
		perf_add(&m.p_run, t1 - t0 - push);
		perf_add(&m.p_audio, audio_take_time_ns() + push);
		perf_add(&m.p_blit, now_ns() - t2);
		m.perf_frames++;
		cpuctl_game_frame(now_ns() - t0, 0, (int64_t)(i + 1) * 16661000LL);
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
	audio_summary("headless");
	log_msg("headless: %ld frames, frame %dx%d pitch %d, final drc %.4f, "
		"fill %d..%d, underruns after warmup %u",
		frames, fw, fh, fp, as->adj, as->fill_min, as->fill_max,
		m.xrun_total);
	core_unload();
	{
		int learned = cpuctl_game_end();

		if (learned != rom_settings.cpu_start) {
			rom_settings.cpu_start = learned;
			rom_settings_save();
		}
	}
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
		if (argc == 4 && !strcmp(argv[1], "--core-switch"))
			return selftest_core_switch(argv[2],
						    strtol(argv[3], NULL, 0));
		if (argc == 3 && !strcmp(argv[1], "--data")) {
			snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", argv[2]);
		} else if (argc != 1) {
			fprintf(stderr, "usage: %s [--selftest DIR | --headless DATA "
				"ROM FRAMES | --core-switch DATA FRAMES | --data DIR]\n",
				argv[0]);
			return 1;
		}
	}
	startup();
	run();
}
