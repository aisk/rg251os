/*
 * frontend: the RG351P GBA frontend, running as PID 1 (/sbin/init).
 *
 * One static program: mounts, logging, KMS output, evdev input, ALSA
 * audio and the statically linked libretro cores (mGBA and gpSP, picked
 * per game, see core.c). The main loop is driven by page flip events:
 * whenever the previous frame is on screen, one retro_run is done, the
 * frame is scaled 2x and rotated straight into the back buffer and
 * flipped, so emulation runs at the panel's 60.02 Hz and audio is
 * resampled to match (see audio.c).
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
 *   green steady    asleep (light sleep; in suspend to RAM whatever the
 *                   hardware does with the LED pin unpowered)
 * Before PID 1 starts the kernel shows its heartbeat trigger (see dts).
 *
 * Logs on the GBA partition: log/boot-NNNN/ per boot (the last 20 are
 * kept, log/LATEST.txt names the newest, see sys_log_init) with
 * frontend.txt (this program) and dmesg-poweroff.txt, plus with debug
 * logging dmesg-5s.txt, dmesg-20s.txt, dmesg-game<N>-30s.txt,
 * dmesg-game<N>-end.txt (also after a fatal error) and
 * dmesg-sleep<N>.txt; and log/perf.csv across boots (per game and CPU
 * frequency timing, power and audio statistics, appended at the end of
 * every game, see cpuctl.c).
 *
 * CPU frequency: schedutil throughout; in game cpuctl.c sets a floor from
 * the frame times (CPU mode setting: adaptive, max or a fixed OPP).
 *
 * Clock (clock.c): the kernel sets the system clock from the RK817 RTC at
 * boot, startup logs both ("clock: ..."); Date & time in the ROM list's
 * settings sets the two. No time zones: what the user enters is UTC.
 *
 * Keys: SELECT+START in game opens the in-game menu (the RG351P has no
 * menu/function button; SELECT is also the hotkey enabler in AmberELEC
 * and ROCKNIX). SELECT in the ROM list opens the settings. See menu.c for
 * the menus.
 *
 * Power key (input.c has the timing): a short press sleeps, in the ROM
 * list, the menus or in game; the next press wakes. Holding it 2 s saves
 * and powers off. Sleep: the game is paused and its save and resume state
 * written, the PCM closed, backlight and panel (DPMS) off, then suspend
 * to RAM (sleep.c). If the kernel does not suspend, a light sleep
 * instead: the same, plus the CPU capped at its lowest OPP, blocked in
 * poll() on the input devices until the power key goes down. On wake
 * everything is restored, the audio rate control and frame pacing start
 * over and the waking press is not taken as a new one. Every step is
 * logged ("sleep #N: ...").

 *
 * Resume states (core.c): a game that ends by Quit to ROM list, Power off
 * (in-game menu, settings or the power key held 2 s, all through
 * do_poweroff) or Restart with another core writes its save and then a
 * state, <data>/saves/<rom>.<core>.state, before the core is unloaded;
 * so does sleep. The next start of that ROM with that core resumes from
 * it. Restart game removes it; a fatal error writes none.
 *
 * First boot: before the GBA partition is mounted, it is grown to fill
 * the SD card (sdgrow.c), with a message on screen; later boots only
 * check that there is nothing to do.
 *
 * Settings (settings.c) are saved when leaving the settings screen in the
 * ROM list, on resume, quit and power off, whenever they differ from what
 * was last loaded or saved.
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
#include "sleep.h"
#include "sysinfo.h"

#define HOTKEY (JOY_BIT(SELECT) | JOY_BIT(START))
#define PERF_INTERVAL_NS (10 * 1000000000LL)
#define MAX_POLL 24
#define HEADLESS_HZ 60.0186
#define HEADLESS_FRAME_NS 16661000LL

int selftest(const char *dir);
int selftest_core_switch(const char *data, long frames);

enum state {
	ST_ROMLIST, ST_GAME, ST_MENU, ST_SETTINGS, ST_FATAL, ST_POWEROFF,
	ST_SYSINFO, ST_DATETIME
};

static struct {
	enum state st;
	uint16_t canvas_px[480 * 320];
	struct canvas cv;
	char rom_path[512];
	char rom_name[128];
	struct rom_id rom_id;  /* the game's name and database entry */
	uint32_t suppress;     /* held buttons hidden until released */
	uint32_t prev;
	int led_green, led_red;
	int dumped5, dumped20;
	int64_t poweroff_at;
	int64_t next_rescan, next_drm_try, next_blink, next_perf;
	int blink;
	char fatal_title[64], fatal_msg[256];
	enum state settings_from;  /* ST_ROMLIST or ST_MENU */
	enum state sysinfo_from;   /* ST_SETTINGS or ST_MENU */
	/* As last loaded or saved, to save only what changed. */
	struct settings saved;
	struct rom_settings saved_rom;
	struct perf_series p_run, p_blit, p_audio;
	uint64_t perf_frames;
	unsigned missed;
	unsigned xrun_total;
	unsigned reports;
	struct audio_stats last_as;
	unsigned games;        /* started this boot, numbers the dmesg dumps */
	uint64_t game_frames;  /* frames of the current game */
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
} m = { .cv = { .w = 480, .h = 320 }, .power_dir = "/sys/power" };

/* ------------------------------------------------------- input events */

static void request_poweroff(const char *why)
{
	if (m.poweroff_at)
		return;
	log_msg("power off: %s", why);
	m.st = ST_POWEROFF;
	/* Leave time for one frame with the message. */
	m.poweroff_at = now_ns() + 150000000LL;
}

static void power_action(int act)
{
	switch (act) {
	case PWR_SHORT:
		if (m.st == ST_FATAL) {
			request_poweroff("power key on the error screen");
		} else if (m.st != ST_POWEROFF && !m.sleep_req) {
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
	m.st = ST_FATAL;
}

static void do_poweroff(void)
{
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
	double hz = display_ready() ? display_info()->refresh_hz : 60.0;
	const char *base = strrchr(path, '/');
	char err[128];

	base = base ? base + 1 : path;
	snprintf(m.rom_path, sizeof(m.rom_path), "%s", path);
	snprintf(m.rom_name, sizeof(m.rom_name), "%.127s", base);
	/* The ROM's settings first: they pick the core. */
	rom_settings_load(m.rom_name);
	m.saved_rom = rom_settings;
	identify_game(path);
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
	cpuctl_game_start(m.rom_name, hz, rom_settings.cpu_start, now_ns());
	m.st = ST_GAME;
	m.suppress = input_buttons();
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
	end_game("restart");
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
	ingame_menu_enter(m.rom_id.title);
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

/*
 * Restart game in the in-game menu: the save written, then the core's
 * reset, and back into the game like a resume (the output restarts with
 * its silence fill, frame pacing and the CPU floor pick up from the
 * pause, so nothing is caught up). The ROM, core and audio rates stay,
 * so it is the same session: no new game number, its statistics and
 * perf.csv row run on until the game ends.
 */
static void reset_game(uint32_t buttons)
{
	log_msg("restart: %s from the beginning (%s), same session",
		m.rom_name, core_labels[core_info()->core]);
	core_reset();
	resume_game(buttons);
	log_sync();
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

/* -------------------------------------------------------------- sleep */

static void perf_report(void);

static const char *const state_names[] = {
	"ROM list", "game", "in-game menu", "settings", "error screen",
	"power off", "system info", "date & time",
};

/* From the ROM list's settings or the in-game menu (the game stays
 * paused); B goes back there. */
static void open_sysinfo(uint32_t buttons, int64_t now)
{
	m.sysinfo_from = m.st;
	sysinfo_enter(state_names[m.st], now);
	m.suppress = buttons;
	m.st = ST_SYSINFO;
}

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
	char bat[96], marker[48];
	struct sleep_result r;
	struct battery b;
	int in_game = m.st == ST_GAME, light;
	int64_t t0 = boot_ms(), t1;

	m.sleeps++;
	snprintf(marker, sizeof(marker), "frontend: sleep #%u: entering", m.sleeps);
	log_msg("sleep #%u: entering from %s at %lld ms", m.sleeps,
		state_names[m.st], (long long)t0);
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
	m.suppress = input_buttons();
	m.prev = m.suppress;
	m.wake_frames = display_frames();
	m.wake_pending = display_ready();
	log_msg("sleep #%u: awake (%s), %lld ms since entering, restored in "
		"%lld ms, back to %s", m.sleeps,
		light ? "light sleep" : r.method, (long long)(boot_ms() - t0),
		(long long)(boot_ms() - t1), state_names[m.st]);
	log_sync();
}

/* ------------------------------------------------------------- frames */

/* SELECT+START (one of them newly pressed), or Esc/F1 on a keyboard. */
static int menu_hotkey(uint32_t buttons)
{
	return ((buttons & HOTKEY) == HOTKEY && (~m.prev & HOTKEY)) ||
	       ((buttons & BTN_BIT_MENU) && !(m.prev & BTN_BIT_MENU));
}

/* Every PERF_INTERVAL_NS in game and before sleeping: the audio counters,
 * and with debug logging the perf line. */
static void perf_report(void)
{
	char run[48], blit[48], aud[48], cpu[96];
	char bat[80] = "n/a", temp[24] = "n/a";
	const struct battery *b = battery_get();
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
	m.perf_frames = 0;
	m.missed = 0;
	if (++m.reports % 6 == 0 && fe_is_init)
		log_sync_async();
}

/* One retro_run (the core pushes its audio meanwhile) and the audio
 * write, into the run and audio timing series. */
static void emulate(uint32_t buttons)
{
	int64_t t0 = now_ns(), t1, push;

	core_run(buttons);
	t1 = now_ns();
	push = audio_take_time_ns();
	audio_flush();
	perf_add(&m.p_run, t1 - t0 - push);
	perf_add(&m.p_audio, audio_take_time_ns() + push);
	m.perf_frames++;
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
	int64_t t0, t2, t3;
	int fw, fh, fp, pitch, scale;
	unsigned missed;
	uint16_t *dst;

	if (menu_hotkey(buttons)) {
		open_menu(buttons);
		return;
	}
	t0 = now_ns();
	emulate(buttons & ~m.suppress & 0xffff);
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
		case UI_SYSINFO:
			open_sysinfo(buttons, now);
			break;
		case UI_QUIT:
			quit_game();
			break;
		case UI_RESTART:
			restart_game();
			break;
		case UI_RESET:
			reset_game(buttons);
			break;
		case UI_POWEROFF:
			request_poweroff("in-game menu");
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
		switch (settings_menu_update(pressed)) {
		case UI_BACK:
			close_settings(buttons);
			break;
		case UI_SYSINFO:
			open_sysinfo(buttons, now);
			break;
		case UI_DATETIME:
			/* Only in the ROM list's settings: no game behind. */
			datetime_enter();
			m.suppress = buttons;
			m.st = ST_DATETIME;
			break;
		case UI_POWEROFF:
			request_poweroff("settings");
			break;
		}
		if (m.st == ST_POWEROFF)
			break;
		fr = NULL;
		fw = fh = fp = 0;
		if (m.settings_from == ST_MENU)
			fr = core_frame(&fw, &fh, &fp);
		if (m.st == ST_SYSINFO)
			sysinfo_draw(&m.cv);
		else if (m.st == ST_DATETIME)
			datetime_draw(&m.cv);
		else if (m.st == ST_SETTINGS)
			settings_menu_draw(&m.cv, fr, fp, fw, fh);
		else if (m.st == ST_MENU)
			ingame_menu_draw(&m.cv, fr, fp, fw, fh);
		else
			romlist_menu_draw(&m.cv);
		blit_canvas();
		break;
	case ST_SYSINFO:
		pressed = ui_repeat(buttons & ~m.suppress, now);
		if (m.sysinfo_from == ST_MENU && menu_hotkey(buttons)) {
			resume_game(buttons);
			break;
		}
		if (sysinfo_update(pressed, now) == UI_BACK) {
			m.suppress = buttons;
			m.st = m.sysinfo_from;
		}
		if (m.st == ST_SYSINFO) {
			sysinfo_draw(&m.cv);
		} else if (m.st == ST_MENU) {
			fr = core_frame(&fw, &fh, &fp);
			ingame_menu_draw(&m.cv, fr, fp, fw, fh);
		} else {
			settings_menu_draw(&m.cv, NULL, 0, 0, 0);
		}
		blit_canvas();
		break;
	case ST_DATETIME:
		pressed = ui_repeat(buttons & ~m.suppress, now);
		if (datetime_update(pressed) == UI_BACK) {
			m.suppress = buttons;
			m.st = ST_SETTINGS;
			settings_menu_draw(&m.cv, NULL, 0, 0, 0);
		} else {
			datetime_draw(&m.cv);
		}
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

	power_action(pwrkey_poll(&m.pk, now));
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

		if (m.sleep_req) {
			m.sleep_req = 0;
			do_sleep();
			continue;
		}
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
 * simulated codec running 0.1% fast, and the save path. FE_SLEEP_AT=N
 * also sleeps after frame N, through a fake /sys/power that cannot
 * suspend, so the fallback light sleep and the wake path run (the light
 * sleep wakes itself with a simulated power key press). FE_RESET_AT=N
 * opens the in-game menu after frame N and confirms Restart game. The
 * resume state is loaded like on a start (core_load) and written at the
 * end like on a quit.
 */
static int headless(const char *data, const char *rom, long frames)
{
	static uint16_t native[320 * 480];
	static char power[320];
	const char *base = strrchr(rom, '/') ? strrchr(rom, '/') + 1 : rom;
	const char *sleep_env = getenv("FE_SLEEP_AT"), *mode = getenv("FE_AUDIO");
	const char *reset_env = getenv("FE_RESET_AT");
	long sleep_at = sleep_env ? strtol(sleep_env, NULL, 0) : -1;
	long reset_at = reset_env ? strtol(reset_env, NULL, 0) : -1;
	const struct audio_stats *as = &m.last_as;
	char err[128], path[300];
	const uint16_t *fr;
	int fw = 0, fh = 0, fp = 0, st, learned;
	pid_t pid;

	snprintf(fe_data_dir, sizeof(fe_data_dir), "%s", data);
	sys_make_dirs();
	snprintf(path, sizeof(path), "%s/log/frontend.txt", fe_data_dir);
	log_open_file(path);
	/* Settings from DATA, so effects can be exercised too. */
	settings_load();
	rom_settings_load(base);
	snprintf(m.rom_name, sizeof(m.rom_name), "%.127s", base);
	identify_game(rom);
	fx_update();
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
	if (core_load(rom, rom_settings.core, err, sizeof(err))) {
		log_msg("headless: %s", err);
		return 1;
	}
	audio_set_source(core_info()->sample_rate, core_info()->fps, HEADLESS_HZ);
	cpuctl_game_start(base, HEADLESS_HZ, rom_settings.cpu_start, 0);
	m.saved = settings;
	m.saved_rom = rom_settings;
	m.st = ST_GAME;
	if (sleep_at >= 0) {
		snprintf(power, sizeof(power), "%s/fake-power", fe_data_dir);
		sleep_fake_sysfs(power);
		m.power_dir = power;
		m.sleep_test = 1;
	}
	for (long i = 0; i < frames; i++) {
		/* Tap A for 10 frames every 2 s of emulated time. */
		int64_t t0 = now_ns(), t2;

		emulate(i % 120 < 10 ? JOY_BIT(A) : 0);
		fr = core_frame(&fw, &fh, &fp);
		t2 = now_ns();
		if (fr)
			blit_rotate(native, 320, 320, 480, 270, fr, fp, fw, fh, 2,
				    fx_current());
		perf_add(&m.p_blit, now_ns() - t2);
		cpuctl_game_frame(now_ns() - t0, 0, (i + 1) * HEADLESS_FRAME_NS);
		/* Fake time for the save timer: 60 frames per second. */
		core_save_tick(i * HEADLESS_FRAME_NS);
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
			/* The menu, then Restart game: down, A, A. */
			int act = UI_NONE;

			open_menu(0);
			ingame_menu_update(JOY_BIT(DOWN));
			ingame_menu_update(JOY_BIT(A));
			for (int k = 0; k < 10 && act == UI_NONE; k++)
				act = ingame_menu_update(k == 9 ? JOY_BIT(A) : 0);
			if (act == UI_RESET)
				reset_game(0);
			log_msg("headless: restart at frame %ld %s", i,
				m.st == ST_GAME ? "done" : "FAILED");
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
	audio_summary("headless");
	log_msg("headless: %ld frames, frame %dx%d pitch %d, final drc %.4f, "
		"fill %d..%d, underruns after warmup %u",
		frames, fw, fh, fp, as->adj, as->fill_min, as->fill_max,
		m.xrun_total);
	core_state_save("headless end");
	core_unload();
	learned = cpuctl_game_end();
	if (learned != rom_settings.cpu_start) {
		rom_settings.cpu_start = learned;
		rom_settings_save();
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
