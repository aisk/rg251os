/*
 * frontend: the RG351P GBA frontend, running as PID 1 (/sbin/init). This
 * file has startup, the main loop and the game's lifecycle; the work is
 * in the modules (sys.c mounts and system helpers, display.c KMS, input.c
 * evdev and the power key timing, audio.c ALSA, core.c the libretro
 * cores, ui.c the page stack, menu.c the ROM list and menus, settings.c,
 * sleep.c, cpuctl.c, sdgrow.c, clock.c, sysinfo.c).
 *
 * Startup: proc/sys/dev mounts, the first boot's partition grow
 * (sdgrow.c, with its messages on screen), the GBA partition mounted and
 * a new log/boot-NNNN/ opened, settings loaded, then backlight, CPU
 * control, battery, display, input and audio; the ROM list once that is
 * done. The main loop is driven by page flip events: whenever the
 * previous frame is on screen, the page stack runs (frame()); in game that
 * is one retro_run with the frame scaled 2x and rotated straight into the
 * back buffer, so emulation runs at the panel's 60.02 Hz and audio is
 * resampled to match (audio.c). The menus are drawn only when something
 * changed (ui.c); when one was not, nothing is flipped and the loop sleeps
 * in poll() until input or the earliest of the times the page and the
 * timers here need (idle_timeout, at most IDLE_MAX_MS).
 *
 * Screens are pages on ui.c's stack: ROM list, game, in-game menu
 * (SELECT+START, the RG351P has no menu button), settings (SELECT in the
 * ROM list, or from the in-game menu), system info, date and time, the
 * fatal error screen and the power off message. The game, error and
 * power off pages are here, and on_ui does what the pages ask for: start
 * a game, resume, quit, restart, power off. A game ends by Quit to ROM
 * list, Restart with another core or power off: its save and resume state
 * are written (core_state_save) before the core is unloaded; Restart game
 * resets the core in the same session; a fatal error writes no state.
 *
 * Power key (timing in input.c): a short press sleeps (do_sleep: game
 * paused, save and state written, audio closed, backlight and panel off,
 * suspend to RAM via sleep.c, or a light sleep blocked in poll() on the
 * input devices with the CPU at its lowest OPP if the kernel does not
 * suspend; every step logged as "sleep #N: ...", the charger's state as
 * "charger: sleep #N before/after: ..."); holding it 2 s, Power
 * off in the menus or the power key on the error screen power off
 * (do_poweroff: game ended, settings saved, children waited for,
 * dmesg-poweroff.txt written, then sys_poweroff closes the log, unmounts
 * the GBA partition and powers off).
 *
 * Settings are saved, when they differ from what was last loaded or
 * saved, on leaving the ROM list's settings, resuming a game from the
 * in-game menu, quitting, restarting with another core, sleeping and
 * powering off.
 *
 * Status LED (one GPIO, red or green): red while starting, green once the
 * first frame is on screen, a short red flash every 2 s on low battery,
 * red/green blinking on a fatal error, red while powering off. Before
 * PID 1 runs the kernel shows its heartbeat trigger (see dts).
 *
 * Logs from here into log/boot-NNNN/: the 10 s perf line in game
 * (perf_report) and, with debug logging, the menus' redraws and the time
 * the loop slept, every 60 s in the menus and on leaving them
 * (menu_stats), dmesg-5s.txt, dmesg-20s.txt, dmesg-game<N>-30s.txt,
 * dmesg-game<N>-end.txt (also after a fatal error) and
 * dmesg-sleep<N>.txt; dmesg-poweroff.txt always.
 *
 * Not PID 1 (testing on a PC or under qemu-user):
 *   frontend --selftest DIR            unit tests, using DIR as scratch
 *   frontend --headless DATA ROM N     run N frames without display or
 *                                      sound device, DATA as GBA partition
 *                                      (core from DATA's ROM settings),
 *                                      resuming and writing the state like
 *                                      a start and a quit
 *   frontend --core-switch DATA N      mGBA, gpSP, mGBA on one save in one
 *                                      process, N frames each, then the
 *                                      resume states with both cores
 *   frontend --data DIR                normal UI with DATA from DIR
 *   frontend --sdgrow IMG DIR HOOK     the first boot's partition grow on
 *                                      a card image (see sdgrow.h)
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "battery.h"
#include "blit.h"
#include "clock.h"
#include "core.h"
#include "cpuctl.h"
#include "display.h"
#include "draw.h"
#include "fe.h"
#include "fx.h"
#include "input.h"
#include "menu.h"
#include "perf.h"
#include "romdb.h"
#include "sdgrow.h"
#include "settings.h"
#include "sleep.h"
#include "sysinfo.h"
#include "ui.h"

#define PERF_INTERVAL_NS (10 * 1000000000LL)
#define MENU_STATS_NS (60 * 1000000000LL)
/* The longest sleep in the menus: children reaped, nothing else waits. */
#define IDLE_MAX_MS 1000
#define MAX_POLL 24
#define HEADLESS_HZ 60.0186
#define HEADLESS_FRAME_NS 16661000LL

int selftest(const char *dir);
int selftest_core_switch(const char *data, long frames);

static const struct page game_page, fatal_page, poweroff_page;

static struct {
	uint16_t canvas_px[CANVAS_W * CANVAS_H];
	struct canvas cv;
	char rom_path[512];
	char rom_name[128];
	struct rom_id rom_id;  /* the game's name and database entry */
	int led_green, led_red;
	int dumped5, dumped20;
	int64_t poweroff_at;
	int64_t next_rescan, next_drm_try, next_blink, next_perf;
	int blink;
	char fatal_title[64], fatal_msg[256];
	/* As last loaded or saved, to save only what changed. */
	struct settings saved;
	struct rom_settings saved_rom;
	struct perf_series p_run, p_blit, p_audio;
	uint64_t perf_frames;
	unsigned perf_skipped;  /* frames the core skipped (frameskip) */
	unsigned missed;
	unsigned xrun_total;
	unsigned reports;
	struct audio_stats last_as;
	/* The menus since since (0: in game): ui_frame calls, redraws, ns
	 * slept in poll(). */
	struct {
		int64_t since, slept;
		unsigned updates, draws;
	} ms;
	int64_t next_timer;     /* the earliest time timers() needs */
	char bat_shown[24];     /* the battery as the menus show it */
	unsigned games;        /* started this boot, numbers the dmesg dumps */
	uint64_t game_frames;  /* frames of the current game */
	int64_t game_start_ns;
	double nodisplay_hz;   /* refresh rate without a display */
	/* Power key and sleep */
	struct pwrkey pk;
	int sleep_req;         /* short press seen, sleep from the main loop */
	int sleeping;          /* in light sleep: a press wakes */
	int wake, wake_key_down;
	unsigned sleeps;
	int wake_pending;      /* log the first frame after a wake */
	uint64_t wake_frames;
	int64_t wake_ms;
	const char *power_dir;
	int sleep_test;        /* headless: light sleep wakes by itself */
} m = {
	.cv = { .w = CANVAS_W, .h = CANVAS_H },
	.power_dir = "/sys/power",
	.nodisplay_hz = 60.0,
};

/* ------------------------------------------------------- input events */

static void request_poweroff(const char *why)
{
	if (m.poweroff_at)
		return;
	log_msg("power off: %s", why);
	ui_reset(&poweroff_page, now_ns());
	/* Leave time for one frame with the message. */
	m.poweroff_at = now_ns() + 150000000LL;
}

static void power_action(int act)
{
	switch (act) {
	case PWR_SHORT:
		if (ui_top() == &fatal_page) {
			request_poweroff("power key on the error screen");
		} else if (ui_top() != &poweroff_page && !m.sleep_req) {
			log_msg("power key: short press, going to sleep");
			m.sleep_req = 1;
		}
		break;
	case PWR_LONG:
		request_poweroff("power key held 2 s");
		break;
	case PWR_WAKE_PRESS:
		log_msg("power key: the press that woke the device, ignored");
		break;
	}
}

void on_power_key(int down)
{
	log_verbose("power key %s", down ? "down" : "up");
	if (m.sleeping) {
		/* Light sleep: going down wakes, nothing else counts. */
		if (down && !m.wake)
			m.wake = 1;
		m.wake_key_down = down;
		return;
	}
	power_action(pwrkey_event(&m.pk, down, now_ns()));
}

void on_volume_key(int dir)
{
	log_verbose("volume key %+d", dir);
	audio_volume_step(dir);
	ui_redraw();  /* the Volume row */
}

void on_jack(int inserted)
{
	audio_jack(inserted);
}

/* --------------------------------------------------------- the game */

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

/* Stop the game if one runs: logs, save and resume state written (not
 * after a fatal error), core unloaded, game statistics out and the
 * learned start OPP into the ROM settings. */
static void end_game(const char *why)
{
	game_end_logs(why);
	audio_pause();
	if (core_loaded() && strcmp(why, "fatal"))
		core_state_save(why);
	core_unload();
	if (cpuctl_in_game())
		rom_settings.cpu_start = cpuctl_game_end();
}

static void fatal(const char *title, const char *msg)
{
	snprintf(m.fatal_title, sizeof(m.fatal_title), "%s", title);
	snprintf(m.fatal_msg, sizeof(m.fatal_msg), "%s", msg);
	log_msg("FATAL: %s: %s", title, msg);
	log_sync();
	end_game("fatal");
	ui_reset(&fatal_page, now_ns());
}

static void menu_stats(int64_t now, const char *why);

static void do_poweroff(void)
{
	menu_stats(now_ns(), "power off");
	led_set_red(1);
	end_game("power off");
	save_settings();
	/* The save child is done (core_unload), others may still run. */
	sys_wait_children(3000);
	log_msg("uptime %lld ms, dumping dmesg", (long long)boot_ms());
	log_sync();
	klog_dump("dmesg-poweroff.txt");
	sys_poweroff();
}

/* The game's name (CRC computed if needed) and what color=auto means
 * for it; after rom_settings_load. */
static void identify_game(const char *path)
{
	int64_t t0 = now_ns();
	char why[96];
	int color;

	romdb_identify(path, 1, &m.rom_id);
	romdb_log(m.rom_name, &m.rom_id);
	log_msg("romdb: %s is \"%s\" (%s, %.1f ms)", m.rom_name, m.rom_id.title,
		romdb_how_names[m.rom_id.how], (now_ns() - t0) / 1e6);
	color = romdb_auto_color(&m.rom_id, why, sizeof(why));
	rom_settings_set_auto(color, why);
}

static void start_game(const char *path)
{
	double hz = display_ready() ? display_info()->refresh_hz :
		    m.nodisplay_hz;
	const char *base = strrchr(path, '/');
	char err[128];

	base = base ? base + 1 : path;
	snprintf(m.rom_path, sizeof(m.rom_path), "%s", path);
	snprintf(m.rom_name, sizeof(m.rom_name), "%.127s", base);
	/* The ROM's settings first: they pick the core. */
	rom_settings_load(m.rom_name);
	m.saved_rom = rom_settings;
	identify_game(path);
	ingame_menu_set_name(m.rom_id.title);
	log_msg("starting %s with %s", path, core_names[rom_settings.core]);
	if (core_load(path, rom_settings.core, err, sizeof(err))) {
		rom_settings_load(NULL);
		m.saved_rom = rom_settings;
		romlist_menu_message(err);
		log_sync();
		return;
	}
	audio_init();
	audio_set_source(core_info()->sample_rate, core_info()->fps, hz);
	m.games++;
	m.game_frames = 0;
	fx_update();
	m.game_start_ns = now_ns();
	cpuctl_game_start(m.rom_name, hz, rom_settings.cpu_start,
			  m.game_start_ns);
	ui_reset(&game_page, now_ns());
	ui_suppress(input_buttons());
	display_take_missed();
	m.next_perf = now_ns() + PERF_INTERVAL_NS;
	log_sync();
}

static void quit_game(void)
{
	end_game("quit");
	save_settings();
	rom_settings_load(NULL);
	m.saved_rom = rom_settings;
	fx_update();
	ui_reset(&romlist_page, now_ns());
	ui_suppress(input_buttons());
	log_sync();
}

/* After the core was changed in the in-game menu: the save is written
 * and the game starts again with the new core. */
static void restart_game(void)
{
	char path[sizeof(m.rom_path)];

	snprintf(path, sizeof(path), "%s", m.rom_path);
	log_msg("restarting with %s", core_names[rom_settings.core]);
	end_game("restart");
	save_settings();
	start_game(path);
	if (ui_top() != &game_page) {
		ui_reset(&romlist_page, now_ns());
		ui_suppress(input_buttons());
	}
}

/* SELECT+START in game: paused under the in-game menu. */
static struct ui_result open_menu(void)
{
	audio_pause();
	cpuctl_game_pause();
	core_save_sync();
	log_msg("in-game menu");
	log_sync();
	return UI_OPEN(&ingame_page);
}

/* Back into the game (the pages over it already closed). */
static void resume_game(void)
{
	save_settings();
	display_take_missed();
	cpuctl_game_resume();
}

/*
 * Restart game in the in-game menu: the save written, then the core's
 * reset, and back into the game like a resume (the output restarts with
 * its silence fill, frame pacing and the CPU floor pick up from the
 * pause, so nothing is caught up). The ROM, core and audio rates stay,
 * so it is the same session: no new game number, its statistics and
 * perf.csv row run on until the game ends.
 */
static void reset_game(void)
{
	log_msg("restart: %s from the beginning (%s), same session",
		m.rom_name, core_labels[core_info()->core]);
	core_reset();
	ui_to_game();
	resume_game();
	log_sync();
}

/* -------------------------------------------------------------- sleep */

static void perf_report(void);

/* Blocks until the power key goes down (or, in the headless test, after
 * a simulated press). */
static void light_sleep(void)
{
	int64_t t0 = now_ns();
	unsigned polls = 0;

	log_msg("sleep #%u: light sleep: emulation paused, audio closed, "
		"backlight off, CPU at its lowest OPP, waiting for the power key",
		m.sleeps);
	cpuctl_sleep(1);
	log_sync();
	m.sleeping = 1;
	m.wake = 0;
	m.wake_key_down = 0;
	while (!m.wake) {
		struct pollfd pfd[MAX_POLL];
		int n = input_pollfds(pfd, MAX_POLL);
		pid_t pid;
		int st;

		polls++;
		if (m.sleep_test && polls > 3) {
			on_power_key(1);
			on_power_key(0);
			break;
		}
		/* The timeout only reaps children and catches hot plug. */
		if (poll(pfd, n, m.sleep_test ? 10 : 2000) > 0)
			input_dispatch(pfd, n);
		else if (!m.sleep_test)
			input_rescan();
		while ((pid = sys_reap(&st)) > 0)
			core_child_exited(pid, st);
	}
	m.sleeping = 0;
	cpuctl_sleep(0);
	log_msg("sleep #%u: light sleep woken by the power key after %lld ms "
		"(%u polls)", m.sleeps, (long long)((now_ns() - t0) / 1000000),
		polls);
}

static void do_sleep(void)
{
	char bat[96], marker[48], why[32];
	struct sleep_result r;
	struct battery b;
	int in_game = ui_top() == &game_page, light;
	int64_t t0 = boot_ms(), t1;

	menu_stats(now_ns(), "sleep");
	m.sleeps++;
	snprintf(marker, sizeof(marker), "frontend: sleep #%u: entering", m.sleeps);
	log_msg("sleep #%u: entering from %s at %lld ms", m.sleeps,
		ui_top()->name, (long long)t0);
	if (in_game) {
		cpuctl_game_pause();
		perf_report();
	}
	audio_suspend();
	/* The battery may run out asleep: resume from here then. */
	if (core_loaded())
		core_state_save("sleep");
	save_settings();
	if (m.led_green && m.led_red) {
		led_set_red(0);
		m.led_red = 0;
	}
	backlight_off();
	if (display_ready())
		display_set_power(0);
	battery_read(&b);
	battery_format(&b, bat, sizeof(bat));
	log_msg("sleep #%u: battery before: %s", m.sleeps, bat);
	snprintf(why, sizeof(why), "sleep #%u before", m.sleeps);
	battery_charger_log(why);
	log_msg("sleep #%u: prepared in %lld ms", m.sleeps,
		(long long)(boot_ms() - t0));

	sleep_suspend(m.power_dir, m.sleeps, &r);
	light = !r.slept;
	if (light)
		light_sleep();
	t1 = boot_ms();
	m.wake_ms = t1;

	/* The press that woke a suspend to RAM may still be reported. */
	pwrkey_wake(&m.pk, light ? m.wake_key_down : -1, now_ns());
	if (display_ready())
		display_set_power(1);
	backlight_set(settings.brightness);
	battery_read(&b);
	battery_format(&b, bat, sizeof(bat));
	log_msg("sleep #%u: battery after: %s", m.sleeps, bat);
	snprintf(why, sizeof(why), "sleep #%u after", m.sleeps);
	battery_charger_log(why);
	if (log_debug()) {
		char name[48];

		snprintf(name, sizeof(name), "dmesg-sleep%u.txt", m.sleeps);
		klog_dump_since(name, marker);
	}
	audio_resume();
	if (in_game) {
		cpuctl_game_resume();
		m.next_perf = now_ns() + PERF_INTERVAL_NS;
	}
	display_resync();
	display_take_missed();
	ui_suppress(input_buttons());
	ui_redraw();
	m.wake_frames = display_frames();
	m.wake_pending = display_ready();
	log_msg("sleep #%u: awake (%s), %lld ms since entering, restored in "
		"%lld ms, back to %s", m.sleeps,
		light ? "light sleep" : r.method, (long long)(boot_ms() - t0),
		(long long)(boot_ms() - t1), ui_top()->name);
	log_sync();
}

/* ------------------------------------------------------------- frames */

/* Every PERF_INTERVAL_NS in game and before sleeping: the audio counters,
 * and with debug logging the perf line. */
static void perf_report(void)
{
	char run[48], blit[48], aud[48], cpu[96], bat[96], temp[24] = "n/a";
	struct audio_stats as;
	long khz, mc;

	audio_take_stats(&as);
	m.xrun_total += as.xruns;
	m.last_as = as;
	if (!log_debug()) {
		perf_reset(&m.p_run);
		perf_reset(&m.p_blit);
		perf_reset(&m.p_audio);
		m.perf_frames = 0;
		m.perf_skipped = 0;
		m.missed = 0;
		return;
	}
	khz = cpu_freq_khz();
	mc = soc_temp_mc();
	if (mc != -1)
		snprintf(temp, sizeof(temp), "%.1f C", mc / 1000.0);
	battery_format(battery_get(), bat, sizeof(bat));
	cpuctl_format(cpu, sizeof(cpu));
	perf_format(&m.p_run, run, sizeof(run));
	perf_format(&m.p_blit, blit, sizeof(blit));
	perf_format(&m.p_audio, aud, sizeof(aud));
	log_msg("perf: %llu frames (%u skipped), ms avg/p99/max: run %s blit %s "
		"audio %s; missed vblanks %u, underruns %u, overflow %u frames, "
		"fill %d..%d, drc %.4f, codec %.0f Hz, cpu0 %ld MHz, %s, temp %s, "
		"bat %s, backlight %d%%",
		(unsigned long long)m.perf_frames, m.perf_skipped, run, blit, aud,
		m.missed,
		as.xruns, as.overflows, as.fill_min, as.fill_max, as.adj,
		as.hw_rate, khz > 0 ? khz / 1000 : -1, cpu, temp, bat,
		settings.brightness);
	m.perf_frames = 0;
	m.perf_skipped = 0;
	m.missed = 0;
	if (++m.reports % 6 == 0 && fe_is_init)
		log_sync_async();
}

/* One retro_run (the core pushes its audio meanwhile) and the audio
 * write, into the run and audio timing series; 1 if the core skipped
 * the frame (frameskip). */
static int emulate(uint32_t buttons)
{
	int64_t t0 = now_ns(), t1, push;
	int skipped = core_run(buttons);

	t1 = now_ns();
	push = audio_take_time_ns();
	audio_flush();
	perf_add(&m.p_run, t1 - t0 - push);
	perf_add(&m.p_audio, audio_take_time_ns() + push);
	m.perf_frames++;
	m.perf_skipped += (unsigned)skipped;
	return skipped;
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

/* The game page: one frame emulated with the held buttons and shown,
 * or the in-game menu opened. */
static struct ui_result game_update(uint32_t held, int64_t now)
{
	const struct display_info *di = display_info();
	const uint16_t *fr;
	int64_t t0, t2, t3;
	int fw, fh, fp, pitch, scale;
	unsigned missed;
	uint16_t *dst;
	int skipped;

	if (ui_hotkey())
		return open_menu();
	t0 = now_ns();
	skipped = emulate(held & 0xffff);
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

	perf_add(&m.p_blit, t3 - t2);
	missed = display_take_missed();
	m.missed += missed;
	/* Work: CPU time until the frame is ready for the flip. A skipped
	 * frame does not tell the controller how much the clock needs. */
	cpuctl_game_frame(t3 - t0, missed, skipped, now);
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
	return UI_STAY;
}

static void fatal_draw(struct canvas *c, const struct game_view *g)
{
	(void)g;
	message_draw(c, m.fatal_title, m.fatal_msg, COL_ERR);
}

static void poweroff_draw(struct canvas *c, const struct game_view *g)
{
	(void)g;
	message_draw(c, "Powering off", "Saving...", COL_HI);
}

static const struct page game_page = { "game", NULL, game_update, NULL, 1 };
static const struct page fatal_page = {
	"error screen", NULL, NULL, fatal_draw, 0,
};
static const struct page poweroff_page = {
	"power off", NULL, NULL, poweroff_draw, 0,
};

/* What the pages ask main.c for; the stack is already changed. */
static void on_ui(enum ui_action act, const struct page *from)
{
	switch (act) {
	case UI_BACK:
		if (from == &settings_page)
			save_settings();
		break;
	case UI_START_ROM:
		start_game(romlist_menu_rom());
		break;
	case UI_RESUME:
		resume_game();
		break;
	case UI_QUIT:
		quit_game();
		break;
	case UI_RESTART:
		restart_game();
		break;
	case UI_RESET:
		reset_game();
		break;
	case UI_POWEROFF:
		request_poweroff(from->name);
		break;
	default:
		break;
	}
}

/* The page stack once; returns 1 when a menu was drawn and shown. */
static int frame(int64_t now)
{
	int drew = ui_frame(&m.cv, input_buttons(), now);

	if (drew)
		blit_canvas();
	m.ms.updates++;
	m.ms.draws += drew;
	return drew;
}

/*
 * With debug logging, how often the menus were drawn and how much of the
 * time the loop slept in poll() (waiting for a flip or idle), with the
 * battery for its power draw: every MENU_STATS_NS in the menus, and why
 * they were left (the game, sleep, power off). Starts over afterwards.
 */
static void menu_stats(int64_t now, const char *why)
{
	double s = (now - m.ms.since) / 1e9;
	char bat[96];

	if (m.ms.since && s > 0 && log_debug()) {
		battery_format(battery_get(), bat, sizeof(bat));
		log_msg("ui: menus for %.1f s (%s): %u redraws (%.1f/s), %u "
			"updates, %.1f%% asleep in poll; bat %s", s, why,
			m.ms.draws, m.ms.draws / s, m.ms.updates,
			100.0 * m.ms.slept / (now - m.ms.since), bat);
	}
	m.ms.since = 0;
}

static void menu_stats_tick(int64_t now)
{
	if (ui_top()->game) {
		if (m.ms.since)
			menu_stats(now, "left for the game");
		return;
	}
	if (m.ms.since && now - m.ms.since >= MENU_STATS_NS)
		menu_stats(now, "every 60 s");
	if (!m.ms.since) {
		m.ms.since = now;
		m.ms.slept = 0;
		m.ms.updates = 0;
		m.ms.draws = 0;
	}
}

/* --------------------------------------------------------------- setup */

/*
 * The first boot's partition grow (sdgrow.c) runs before anything else
 * shows; its messages bring the display up early. Blocking: the next
 * flip waits until the previous one is done.
 */
static void grow_show(const char *msg)
{
	static int tried_display;
	struct pollfd pfd;

	/* The panel may still be probing this early: wait up to 3 s once. */
	for (int i = 0; !display_ready() && i < 30 && !tried_display; i++) {
		if (!display_init()) {
			backlight_init(settings_defaults.brightness);
			break;
		}
		usleep(100000);
	}
	tried_display = 1;
	if (!display_ready())
		return;
	/* Draw when a buffer is free, then let the flip complete. */
	for (int i = 0; i < 2; i++) {
		if (i || !display_can_draw()) {
			pfd.fd = display_fd();
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 200) > 0)
				display_handle_events();
		}
		if (!i && display_can_draw()) {
			message_draw(&m.cv, "Preparing SD card", msg, COL_HI);
			/* No power key handling meanwhile. */
			draw_fill(&m.cv, 0, m.cv.h - 32, m.cv.w, 32, COL_BG);
			blit_canvas();
		}
	}
}

static void startup(void)
{
	struct utsname u;
	char path[400];
	unsigned boot;

	m.cv.px = m.canvas_px;
	ui_set_handler(on_ui);
	sys_early_init();
	led_set_red(1);
	uname(&u);
	log_msg("frontend started at %lld ms (CLOCK_BOOTTIME), pid %d, kernel %s",
		(long long)boot_ms(), (int)getpid(), u.release);

	sdgrow_boot(grow_show);
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
		log_msg("ui: font %dx%d, canvas %dx%d, ROM list %d rows of %d px, "
			"%d columns, in-game menu rows %d px", FONT_W, FONT_H,
			CANVAS_W, CANVAS_H, LIST_ROWS, ROW_H, LIST_COLS, IG_ROW_H);
		fx_init_grid_phase(boot);
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
	clock_boot_log();
	battery_init("/sys/class/power_supply");
	if (!display_ready() && display_init())
		m.next_drm_try = now_ns() + 200000000LL;
	input_init();
	audio_init();
	if (ui_top() != &fatal_page)
		ui_reset(&romlist_page, now_ns());
	log_msg("startup done at %lld ms", (long long)boot_ms());
	log_sync();
}

/* The earliest time timers() has something to do, for idle_timeout. */
static void timer_at(int64_t t)
{
	if (t < m.next_timer)
		m.next_timer = t;
}

static void timers(int64_t now)
{
	int64_t up = boot_ms();
	char label[16], bat[24];
	pid_t pid;
	int st;

	m.next_timer = INT64_MAX;
	while ((pid = sys_reap(&st)) > 0)
		core_child_exited(pid, st);

	power_action(pwrkey_poll(&m.pk, now));
	if (m.pk.down && !m.pk.fired)
		timer_at(m.pk.down_ns + PWRKEY_LONG_NS);
	if (m.wake_pending && display_frames() > m.wake_frames) {
		m.wake_pending = 0;
		log_msg("sleep #%u: first frame on screen %lld ms after resume",
			m.sleeps, (long long)(up - m.wake_ms));
	}

	if (!m.dumped5 && up >= 5000 && log_debug()) {
		m.dumped5 = 1;
		klog_dump_async("dmesg-5s.txt");
	}
	if (!m.dumped20 && up >= 20000 && log_debug()) {
		m.dumped20 = 1;
		klog_dump_async("dmesg-20s.txt");
		log_sync();
	}
	if (log_debug() && (!m.dumped5 || !m.dumped20))
		timer_at(now + ((m.dumped5 ? 20000 : 5000) - up) * 1000000LL);
	if (!display_ready() && now >= m.next_drm_try) {
		if (display_init()) {
			m.next_drm_try = now + 200000000LL;
			if (up > 15000 && ui_top() != &fatal_page)
				fatal("No display", "The DRM device did not come up.");
		}
	}
	if (now >= m.next_rescan) {
		input_rescan();
		audio_init();
		m.next_rescan = now + 2000000000LL;
	}
	timer_at(m.next_rescan);
	timer_at(battery_poll(now));
	/* Drawn in the menus' headers: redrawn when that changes. */
	battery_label(battery_get(), label, sizeof(label));
	snprintf(bat, sizeof(bat), "%s%s", label, battery_low() ? " low" : "");
	if (strcmp(bat, m.bat_shown)) {
		snprintf(m.bat_shown, sizeof(m.bat_shown), "%s", bat);
		ui_redraw();
	}
	if (!m.led_green && ui_top() != &fatal_page && display_frames() > 0) {
		log_msg("up: first frame shown at %lld ms", (long long)up);
		log_sync();
		led_set_red(0);
		m.led_green = 1;
	}
	if (m.led_green && ui_top() != &fatal_page &&
	    ui_top() != &poweroff_page) {
		/* Low battery: a 250 ms red flash every 2 s. */
		int red = battery_low() && now / 1000000 % 2000 < 250;

		if (red != m.led_red) {
			led_set_red(red);
			m.led_red = red;
		}
		if (battery_low())
			timer_at(now - now % 2000000000LL +
				 (red ? 250000000LL : 2000000000LL));
	}
	if (ui_top() == &fatal_page && now >= m.next_blink) {
		m.blink ^= 1;
		led_set_red(m.blink);
		m.next_blink = now + 500000000LL;
	}
	if (ui_top() == &fatal_page)
		timer_at(m.next_blink);
	if (m.poweroff_at && now >= m.poweroff_at)
		do_poweroff();
	if (m.poweroff_at)
		timer_at(m.poweroff_at);
}

/*
 * poll()'s timeout in ms in the menus when nothing was drawn (no flip to
 * wait for): until the page's next time (ui_due) or the timers', at most
 * IDLE_MAX_MS, rounded up so it does not wake just before.
 */
static int idle_timeout(int64_t now)
{
	int64_t t = ui_due();

	if (m.next_timer < t)
		t = m.next_timer;
	if (t <= now)
		return 0;
	if (t - now >= IDLE_MAX_MS * 1000000LL)
		return IDLE_MAX_MS;
	return (int)((t - now + 999999) / 1000000);
}

static void run(void)
{
	for (;;) {
		struct pollfd pfd[MAX_POLL];
		int64_t now = now_ns(), t0;
		int n = 0, timeout, drew = 0, r;

		if (m.sleep_req) {
			m.sleep_req = 0;
			do_sleep();
			continue;
		}
		timers(now);
		menu_stats_tick(now);
		if (display_can_draw())
			drew = frame(now);

		if (display_ready()) {
			pfd[n].fd = display_fd();
			pfd[n].events = POLLIN;
			pfd[n++].revents = 0;
		}
		n += input_pollfds(pfd + n, MAX_POLL - n);
		timeout = display_poll_timeout(now_ns());
		if (timeout > 100)
			timeout = 100;
		/* A menu with nothing new: no flip coming, sleep. The game
		 * always flips. */
		if (!drew && display_can_draw() && !ui_top()->game)
			timeout = idle_timeout(now_ns());
		t0 = now_ns();
		r = poll(pfd, n, timeout);
		if (!ui_top()->game)
			m.ms.slept += now_ns() - t0;
		if (r <= 0)
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
 * simulated codec running 0.1% fast, and the save path. FE_SLEEP_AT=N
 * also sleeps after frame N, through a fake /sys/power that cannot
 * suspend, so the fallback light sleep and the wake path run (the light
 * sleep wakes itself with a simulated power key press). FE_RESET_AT=N
 * opens the in-game menu after frame N and confirms Restart game, with
 * key presses through the page stack (ui_frame). The game starts and
 * ends like on the device (start_game, end_game and the settings saved),
 * so the resume state is loaded and written like on a start and a quit.
 */
static int headless(const char *data, const char *rom, long frames)
{
	static uint16_t native[320 * 480];
	static char power[320];
	const char *sleep_env = getenv("FE_SLEEP_AT"), *mode = getenv("FE_AUDIO");
	const char *reset_env = getenv("FE_RESET_AT");
	long sleep_at = sleep_env ? strtol(sleep_env, NULL, 0) : -1;
	long reset_at = reset_env ? strtol(reset_env, NULL, 0) : -1;
	const struct audio_stats *as = &m.last_as;
	char path[300];
	const uint16_t *fr;
	int fw = 0, fh = 0, fp = 0, st;
	unsigned nrun, nskip;
	int64_t t;
	pid_t pid;

	snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", data);
	sys_make_dirs();
	snprintf(path, sizeof(path), "%s/log/frontend.txt", fe_data_dir);
	log_open_file(path);
	/* Settings from DATA, so effects can be exercised too. */
	settings_load();
	/* FE_AUDIO=simple: the simple path with a realistic 50 ppm codec
	 * drift (it has no rate control); otherwise adaptive with 0.1%. */
	settings.audio = mode && !strcmp(mode, "simple") ? AUDIO_SIMPLE :
			 AUDIO_ADAPTIVE;
	audio_init_null(settings.audio == AUDIO_SIMPLE ? 0.00005 : 0.001);
	/* Simulated cpufreq: real work times, fake 60 Hz clock. */
	snprintf(path, sizeof(path), "%s/fake-cpufreq", fe_data_dir);
	cpuctl_fake_sysfs(path);
	cpuctl_init(path);
	cpuctl_set_mode(settings.cpu);
	m.saved = settings;
	m.cv.px = m.canvas_px;
	m.nodisplay_hz = HEADLESS_HZ;
	ui_set_handler(on_ui);
	start_game(rom);
	if (ui_top() != &game_page) {
		log_msg("headless: %s did not start", rom);
		return 1;
	}
	if (sleep_at >= 0) {
		snprintf(power, sizeof(power), "%s/fake-power", fe_data_dir);
		sleep_fake_sysfs(power);
		m.power_dir = power;
		m.sleep_test = 1;
	}
	for (long i = 0; i < frames; i++) {
		/* Tap A for 10 frames every 2 s of emulated time. */
		int64_t t0 = now_ns(), t2;
		int skipped;

		/* Fake time from the start: 60 frames per second. */
		t = m.game_start_ns + i * HEADLESS_FRAME_NS;
		skipped = emulate(i % 120 < 10 ? JOY_BIT(A) : 0);
		fr = core_frame(&fw, &fh, &fp);
		t2 = now_ns();
		if (fr)
			blit_rotate(native, 320, 320, 480, 270, fr, fp, fw, fh, 2,
				    fx_current());
		perf_add(&m.p_blit, now_ns() - t2);
		cpuctl_game_frame(now_ns() - t0, 0, skipped,
				  t + HEADLESS_FRAME_NS);
		core_save_tick(t);
		while ((pid = sys_reap(&st)) > 0)
			core_child_exited(pid, st);
		if (i == sleep_at) {
			/* A short press, as the input layer reports it. */
			on_power_key(1);
			on_power_key(0);
			if (m.sleep_req) {
				m.sleep_req = 0;
				do_sleep();
			}
			/* A stray release after the wake starts nothing. */
			on_power_key(0);
			log_msg("headless: after sleep, %s", m.sleep_req ?
				"FAILED: sleep requested again" : "no new sleep request");
		}
		if (i == reset_at) {
			/* Through the pages: SELECT+START, then Restart game
			 * (down, A, A), each press released for a frame. */
			static const uint32_t keys[] = {
				JOY_BIT(SELECT) | JOY_BIT(START), JOY_BIT(DOWN),
				JOY_BIT(A), JOY_BIT(A),
			};

			for (size_t k = 0; k < 2 * ARRAY_SIZE(keys) &&
			     (!k || ui_top() != &game_page); k++)
				ui_frame(&m.cv, k % 2 ? 0 : keys[k / 2],
					 now_ns());
			log_msg("headless: restart at frame %ld %s", i,
				ui_top() == &game_page ? "done" : "FAILED");
		}
		if (i % 600 == 599) {
			perf_report();
			/* The first 20 s are warmup for the rate control. */
			if (i < 1200)
				m.xrun_total = 0;
		}
	}
	if (m.perf_frames)
		perf_report();
	core_frame_counts(&nrun, &nskip);
	log_msg("headless: %ld frames, frame %dx%d pitch %d, final drc %.4f, "
		"fill %d..%d, underruns after warmup %u, skipped %u of %u",
		frames, fw, fh, fp, as->adj, as->fill_min, as->fill_max,
		m.xrun_total, nskip, nrun);
	/* Like a quit, without going back to the ROM list. */
	end_game("headless end");
	save_settings();
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
		if (argc == 5 && !strcmp(argv[1], "--sdgrow"))
			return sdgrow_image(argv[2], argv[3], argv[4]);
		if (argc == 4 && !strcmp(argv[1], "--core-switch"))
			return selftest_core_switch(argv[2],
						    strtol(argv[3], NULL, 0));
		if (argc == 3 && !strcmp(argv[1], "--data")) {
			snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", argv[2]);
		} else if (argc != 1) {
			fprintf(stderr, "usage: %s [--selftest DIR | --headless DATA "
				"ROM FRAMES | --core-switch DATA FRAMES | --data DIR | "
				"--sdgrow IMG DIR HOOK]\n",
				argv[0]);
			return 1;
		}
	}
	startup();
	run();
}
