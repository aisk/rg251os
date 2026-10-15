/*
 * hwtest: hardware smoke test for the RG351P, running as PID 1.
 *
 * Brings up the display through raw DRM/KMS ioctls (dumb buffers, double
 * buffering, page flips paced by vblank events), draws a diagnostic screen
 * in landscape coordinates rotated in software, lists input devices and
 * their last events, and powers off when the power key is pressed.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <linux/input.h>

#include "font8x8.h"

/*
 * Software rotation from logical landscape to the native portrait buffer,
 * in degrees counter clockwise, matching the DT "rotation" property of the
 * panel (270 on RG351P/M, same as fbcon=rotate:3). If the picture comes out
 * upside down, change this to 90.
 */
#define ROTATION 270

#define MAX_INPUTS 16
#define RESCAN_MS 1000
#define FLIP_TIMEOUT_MS 500
#define RATE_WINDOW 60

struct fb {
	uint32_t handle, pitch, fb_id;
	uint64_t size;
	uint8_t *map;
};

struct display {
	int fd;
	uint32_t conn_id, crtc_id;
	struct drm_mode_modeinfo mode;
	uint32_t format;
	struct fb fb[2];
	int back;              /* index of the buffer we draw into */
	int rotation;          /* effective rotation, 0 if the mode is landscape */
	int lw, lh;            /* logical size */
	uint32_t *shadow;      /* logical XRGB8888 canvas */
	int flip_pending;
	int64_t flip_sent_ns;
	int use_setcrtc;       /* page flip failed, fall back to SETCRTC */
	int flip_errno;
};

struct input_dev {
	int fd;
	int num;
	char name[64];
};

static struct display disp = { .fd = -1 };
static struct input_dev inputs[MAX_INPUTS];
static int ninputs;
static int kmsg_fd = -1;
static int is_init;
static int drm_verbose = 1;  /* cleared after the first failed DRM probe */

/* Statistics shown on screen. */
static uint64_t frames;
static uint64_t missed_vblanks;
static uint64_t flip_timeouts;
static double flip_hz, vblank_hz;
static int64_t t_init_ms = -1, t_drm_ms = -1, t_first_frame_ms = -1;
static struct {
	int dev;
	unsigned code;
	int value;
	int valid;
} last_key, last_abs;
static int64_t poweroff_at_ns = -1;

/* ---------------------------------------------------------------- utils */

static int64_t now_ns(clockid_t clk)
{
	struct timespec ts;

	clock_gettime(clk, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void logmsg(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	n = snprintf(buf, sizeof(buf), "<6>hwtest: ");
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(buf) - 1)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	if (kmsg_fd >= 0 && write(kmsg_fd, buf, n) == n)
		return;
	/* Not PID 1 or no /dev/kmsg: strip the level prefix. */
	if (write(STDERR_FILENO, buf + 3, n - 3) < 0)
		return;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

static void on_sigchld(int sig)
{
	int saved = errno;

	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
	errno = saved;
}

/* ----------------------------------------------------------- PID 1 setup */

/*
 * Debug aids for bring-up without a serial console. The status LED shows
 * how far boot got, and the kernel log is copied to the GBA partition so
 * it can be read on a PC even when the panel stays dark.
 */
static void write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, val, strlen(val)) < 0)
		logmsg("write %s: %s", path, strerror(errno));
	close(fd);
}

static void status_led(const char *brightness)
{
	DIR *d = opendir("/sys/class/leds");
	struct dirent *e;
	char path[300];

	if (!d)
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/leds/%s/trigger", e->d_name);
		write_file(path, "none");
		snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", e->d_name);
		write_file(path, brightness);
	}
	closedir(d);
}

static void dump_klog(const char *path)
{
	int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
	char *buf;
	int n, fd;

	if (size <= 0)
		size = 1 << 17;
	buf = malloc(size);
	if (!buf)
		return;
	n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, size);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0 && n > 0) {
		if (write(fd, buf, n) != n)
			logmsg("write %s: %s", path, strerror(errno));
		fsync(fd);
	}
	if (fd >= 0)
		close(fd);
	free(buf);
}

/* Child of PID 1: save dmesg at 5s and 20s, then turn the LED green. */
static void spawn_log_dumper(void)
{
	static const char *const dev = "/dev/mmcblk0p3", *const mnt = "/mnt/data";

	if (fork() != 0)
		return;
	sleep(5);
	if (mount(dev, mnt, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) {
		logmsg("mount %s: %s", dev, strerror(errno));
		_exit(1);
	}
	mkdir("/mnt/data/log", 0755);
	dump_klog("/mnt/data/log/dmesg-5s.txt");
	sleep(15);
	dump_klog("/mnt/data/log/dmesg-20s.txt");
	umount(mnt);
	sync();
	status_led("0");
	_exit(0);
}

static void early_init(void)
{
	struct sigaction sa = { .sa_handler = on_sigchld, .sa_flags = SA_RESTART };
	int fd;

	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);

	if (!is_init)
		return;

	mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	/* Normally already mounted by CONFIG_DEVTMPFS_MOUNT (EBUSY). */
	mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");

	/* Make sure fds 0..2 are valid so no device fd ends up as stderr. */
	do
		fd = open("/dev/console", O_RDWR | O_NOCTTY);
	while (fd >= 0 && fd < 2);
	if (fd > 2)
		close(fd);
	while ((fd = open("/dev/null", O_RDWR)) >= 0 && fd < 2)
		;
	if (fd > 2)
		close(fd);

	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

	status_led("1");
	spawn_log_dumper();
}

/* -------------------------------------------------------------- drawing */

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
	int x1 = x + w, y1 = y + h;

	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x1 > disp.lw)
		x1 = disp.lw;
	if (y1 > disp.lh)
		y1 = disp.lh;
	for (int j = y; j < y1; j++) {
		uint32_t *p = disp.shadow + (size_t)j * disp.lw;
		for (int i = x; i < x1; i++)
			p[i] = c;
	}
}

static void draw_char(int x, int y, int scale, uint32_t c, unsigned char ch)
{
	const uint8_t *g;

	if (ch < FONT_FIRST || ch > FONT_LAST)
		ch = '?';
	g = font8x8[ch - FONT_FIRST];
	for (int r = 0; r < 8; r++)
		for (int b = 0; b < 8; b++)
			if (g[r] & (1u << b))
				fill_rect(x + b * scale, y + r * scale,
					  scale, scale, c);
}

static void text(int x, int y, int scale, uint32_t c, const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	for (const char *s = buf; *s; s++, x += 8 * scale)
		draw_char(x, y, scale, c, (unsigned char)*s);
}

static inline uint16_t to_rgb565(uint32_t c)
{
	return ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
}

/* Copy the logical canvas into the native buffer, rotating as needed. */
static void blit(struct fb *fb)
{
	int nw = disp.mode.hdisplay, nh = disp.mode.vdisplay;

	for (int ny = 0; ny < nh; ny++) {
		const uint32_t *src;
		ptrdiff_t step;

		/* Map native (0, ny) to a logical pixel and the step along nx. */
		switch (disp.rotation) {
		case 270: /* native left column is the user's top edge */
			src = disp.shadow + (disp.lw - 1 - ny);
			step = disp.lw;
			break;
		case 90: /* native right column is the user's top edge */
			src = disp.shadow + (size_t)(disp.lh - 1) * disp.lw + ny;
			step = -disp.lw;
			break;
		default:
			src = disp.shadow + (size_t)ny * disp.lw;
			step = 1;
			break;
		}

		if (disp.format == DRM_FORMAT_RGB565) {
			uint16_t *dst = (uint16_t *)(fb->map + (size_t)ny * fb->pitch);
			for (int nx = 0; nx < nw; nx++, src += step)
				dst[nx] = to_rgb565(*src);
		} else {
			uint32_t *dst = (uint32_t *)(fb->map + (size_t)ny * fb->pitch);
			for (int nx = 0; nx < nw; nx++, src += step)
				dst[nx] = *src;
		}
	}
}

static void render(void)
{
	static const uint32_t bars[] = {
		0xff0000, 0x00ff00, 0x0000ff, 0xffffff,
		0x00ffff, 0xff00ff, 0xffff00, 0x808080,
	};
	static const char *bar_names = "RGBWCMYg";
	int W = disp.lw, H = disp.lh;
	uint32_t fg = 0xffffff, dim = 0xa0a0a0, hi = 0xffd040;
	int64_t up = now_ns(CLOCK_BOOTTIME) / 1000000;
	static struct utsname u;
	int y;

	fill_rect(0, 0, W, H, 0x101828);

	text(8, 6, 2, hi, "RG351P HWTEST");
	text(W - 8 - 8 * 2 * 8, 6, 2, fg, "%8llu", (unsigned long long)frames);

	y = 30;
	if (!u.release[0])
		uname(&u);
	text(8, y, 1, dim, "kernel %s  pid %d", u.release, (int)getpid());
	y += 11;
	text(8, y, 1, fg, "mode %ux%u@%u  fmt %s  rot %d",
	     disp.mode.hdisplay, disp.mode.vdisplay, disp.mode.vrefresh,
	     disp.format == DRM_FORMAT_RGB565 ? "RGB565" : "XRGB8888",
	     disp.rotation);
	y += 11;
	text(8, y, 1, fg, "flip %.2f Hz  vblank %.2f Hz  missed %llu",
	     flip_hz, vblank_hz, (unsigned long long)missed_vblanks);
	y += 11;
	if (disp.use_setcrtc)
		text(8, y, 1, 0xff4040, "PAGE_FLIP failed: %s (SETCRTC fallback)",
		     strerror(disp.flip_errno));
	else
		text(8, y, 1, flip_timeouts ? 0xff4040 : dim,
		     "flip timeouts %llu", (unsigned long long)flip_timeouts);
	y += 11;
	text(8, y, 1, hi, "boottime ms: init %lld  drm %lld  1st frame %lld",
	     (long long)t_init_ms, (long long)t_drm_ms,
	     (long long)t_first_frame_ms);
	y += 11;
	text(8, y, 1, fg, "uptime %lld.%03lld s",
	     (long long)(up / 1000), (long long)(up % 1000));

	/* Color bars: checks channel order and the pixel format. */
	y = 102;
	for (int i = 0; i < 8; i++) {
		fill_rect(8 + i * 40, y, 40, 20, bars[i]);
		draw_char(8 + i * 40 + 16, y + 6, 1, i == 3 || i == 4 || i == 6 ?
			  0x000000 : 0xffffff, (unsigned char)bar_names[i]);
	}
	/* Gray ramp: banding shows RGB565 quantization. */
	for (int i = 0; i < 320; i++) {
		uint32_t v = (uint32_t)(i * 255 / 319);
		fill_rect(8 + i, y + 20, 1, 8, v << 16 | v << 8 | v);
	}

	/* Input devices and last events. */
	y = 138;
	text(8, y, 1, hi, "input devices: %d", ninputs);
	y += 11;
	for (int i = 0; i < ninputs && i < 6; i++, y += 10)
		text(16, y, 1, fg, "event%d %.48s", inputs[i].num, inputs[i].name);
	y = 218;
	if (last_key.valid)
		text(8, y, 1, fg, "last key: event%d code 0x%03x (%u) val %d",
		     last_key.dev, last_key.code, last_key.code, last_key.value);
	else
		text(8, y, 1, dim, "last key: -");
	y += 11;
	if (last_abs.valid)
		text(8, y, 1, fg, "last abs: event%d code 0x%02x (%u) val %d",
		     last_abs.dev, last_abs.code, last_abs.code, last_abs.value);
	else
		text(8, y, 1, dim, "last abs: -");
	y += 11;
	if (poweroff_at_ns >= 0)
		text(8, y, 2, 0xff4040, "POWERING OFF");
	else
		text(8, y, 1, dim, "press POWER to power off");

	/* Vertical bar sweeping right: tearing shows as a broken edge. */
	fill_rect(0, 270, W, 44, 0x000000);
	fill_rect((int)(frames * 4 % (uint64_t)(W + 16)) - 16, 270, 16, 44,
		  0xffffff);
	/* Horizontal bar moving down in the right column. */
	fill_rect(W - 48, 30, 40, 232, 0x000000);
	fill_rect(W - 48, 30 + (int)(frames * 2 % 224), 40, 8, 0x40ff40);

	/* Orientation markers: yellow at the user's top-left, red bottom-right. */
	fill_rect(0, 0, 12, 4, hi);
	fill_rect(0, 0, 4, 12, hi);
	fill_rect(W - 4, H - 12, 4, 12, 0xff4040);
	fill_rect(W - 12, H - 4, 12, 4, 0xff4040);
}

/* -------------------------------------------------------------- display */

static void destroy_fb(int fd, struct fb *fb)
{
	if (fb->map && fb->map != MAP_FAILED)
		munmap(fb->map, fb->size);
	if (fb->fb_id)
		xioctl(fd, DRM_IOCTL_MODE_RMFB, &fb->fb_id);
	if (fb->handle) {
		struct drm_mode_destroy_dumb d = { .handle = fb->handle };
		xioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	memset(fb, 0, sizeof(*fb));
}

static int create_fb(int fd, struct fb *fb, uint32_t w, uint32_t h,
		     uint32_t format)
{
	struct drm_mode_create_dumb c = {
		.width = w, .height = h,
		.bpp = format == DRM_FORMAT_RGB565 ? 16 : 32,
	};
	struct drm_mode_fb_cmd2 f = { .width = w, .height = h,
				      .pixel_format = format };
	struct drm_mode_map_dumb m = { 0 };

	memset(fb, 0, sizeof(*fb));
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c))
		return -1;
	fb->handle = c.handle;
	fb->pitch = c.pitch;
	fb->size = c.size;

	f.handles[0] = c.handle;
	f.pitches[0] = c.pitch;
	if (xioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f))
		goto fail;
	fb->fb_id = f.fb_id;

	m.handle = c.handle;
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
		goto fail;
	fb->map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       m.offset);
	if (fb->map == MAP_FAILED)
		goto fail;
	memset(fb->map, 0, c.size);
	return 0;
fail:
	destroy_fb(fd, fb);
	return -1;
}

/* Fetch a connector with its mode and encoder arrays, retrying if they grow. */
static int get_connector(int fd, uint32_t id, struct drm_mode_get_connector *c,
			 struct drm_mode_modeinfo **modes, uint32_t **encs)
{
	for (int tries = 0; tries < 5; tries++) {
		uint32_t nm, ne;

		memset(c, 0, sizeof(*c));
		c->connector_id = id;
		if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, c))
			return -1;
		nm = c->count_modes;
		ne = c->count_encoders;
		*modes = calloc(nm + 1, sizeof(**modes));
		*encs = calloc(ne + 1, sizeof(**encs));
		c->count_props = 0;
		c->props_ptr = c->prop_values_ptr = 0;
		c->modes_ptr = (uintptr_t)*modes;
		c->encoders_ptr = (uintptr_t)*encs;
		if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, c) == 0 &&
		    c->count_modes <= nm && c->count_encoders <= ne)
			return 0;
		free(*modes);
		free(*encs);
	}
	return -1;
}

static int display_init(void)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t *conns = NULL, *crtcs = NULL, *encs = NULL;
	struct drm_mode_modeinfo *modes = NULL;
	struct drm_mode_get_connector conn;
	static const uint32_t formats[] = { DRM_FORMAT_RGB565, DRM_FORMAT_XRGB8888 };
	int fd, ok = 0;

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		if (drm_verbose)
			logmsg("open /dev/dri/card0: %s", strerror(errno));
		drm_verbose = 0;
		return -1;
	}
	if (t_drm_ms < 0)
		t_drm_ms = now_ns(CLOCK_BOOTTIME) / 1000000;

	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		goto out;
	conns = calloc(res.count_connectors + 1, sizeof(*conns));
	crtcs = calloc(res.count_crtcs + 1, sizeof(*crtcs));
	res.count_fbs = res.count_encoders = 0;
	res.fb_id_ptr = res.encoder_id_ptr = 0;
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		goto out;

	/* First connected connector with at least one mode. */
	for (uint32_t i = 0; i < res.count_connectors && !disp.conn_id; i++) {
		struct drm_mode_get_encoder enc = { 0 };
		int mi = 0;

		if (get_connector(fd, conns[i], &conn, &modes, &encs))
			continue;
		if (conn.connection != 1 || conn.count_modes == 0)
			goto next;
		for (uint32_t m = 0; m < conn.count_modes; m++)
			if (modes[m].type & DRM_MODE_TYPE_PREFERRED) {
				mi = m;
				break;
			}

		/* Current encoder first, else any encoder with a usable CRTC. */
		enc.encoder_id = conn.encoder_id;
		if (conn.encoder_id && !xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) &&
		    enc.crtc_id) {
			disp.crtc_id = enc.crtc_id;
		} else {
			for (uint32_t e = 0; e < conn.count_encoders && !disp.crtc_id; e++) {
				memset(&enc, 0, sizeof(enc));
				enc.encoder_id = encs[e];
				if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc))
					continue;
				for (uint32_t c = 0; c < res.count_crtcs; c++)
					if (enc.possible_crtcs & (1u << c)) {
						disp.crtc_id = crtcs[c];
						break;
					}
			}
		}
		if (disp.crtc_id) {
			disp.conn_id = conn.connector_id;
			disp.mode = modes[mi];
		}
next:
		free(modes);
		free(encs);
		modes = NULL;
		encs = NULL;
	}
	if (!disp.conn_id) {
		if (drm_verbose)
			logmsg("no connected connector with a CRTC");
		goto out;
	}
	logmsg("connector %u crtc %u mode %s %ux%u@%u", disp.conn_id,
	       disp.crtc_id, disp.mode.name, disp.mode.hdisplay,
	       disp.mode.vdisplay, disp.mode.vrefresh);

	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]) && !ok; i++) {
		uint32_t w = disp.mode.hdisplay, h = disp.mode.vdisplay;
		struct drm_mode_crtc crtc = {
			.set_connectors_ptr = (uintptr_t)&disp.conn_id,
			.count_connectors = 1,
			.crtc_id = disp.crtc_id,
			.mode_valid = 1,
			.mode = disp.mode,
		};

		if (create_fb(fd, &disp.fb[0], w, h, formats[i]) ||
		    create_fb(fd, &disp.fb[1], w, h, formats[i])) {
			if (drm_verbose)
				logmsg("format %.4s: dumb/addfb2 failed: %s",
			       (const char *)&formats[i], strerror(errno));
			goto fail_fmt;
		}
		crtc.fb_id = disp.fb[0].fb_id;
		if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc)) {
			if (drm_verbose)
				logmsg("format %.4s: setcrtc failed: %s",
			       (const char *)&formats[i], strerror(errno));
			goto fail_fmt;
		}
		disp.format = formats[i];
		ok = 1;
		break;
fail_fmt:
		destroy_fb(fd, &disp.fb[0]);
		destroy_fb(fd, &disp.fb[1]);
	}
	if (!ok)
		goto out;

	disp.rotation = disp.mode.hdisplay < disp.mode.vdisplay ? ROTATION : 0;
	if (disp.rotation == 90 || disp.rotation == 270) {
		disp.lw = disp.mode.vdisplay;
		disp.lh = disp.mode.hdisplay;
	} else {
		disp.lw = disp.mode.hdisplay;
		disp.lh = disp.mode.vdisplay;
	}
	disp.shadow = calloc((size_t)disp.lw * disp.lh, sizeof(uint32_t));
	disp.fd = fd;
	disp.back = 1;
	logmsg("display up, format %.4s, logical %dx%d rot %d",
	       (const char *)&disp.format, disp.lw, disp.lh, disp.rotation);
out:
	free(conns);
	free(crtcs);
	if (disp.fd < 0) {
		close(fd);
		disp.conn_id = disp.crtc_id = 0;
		drm_verbose = 0;
		return -1;
	}
	return 0;
}

/* Draw the next frame into the back buffer and queue it. */
static void present(void)
{
	struct fb *fb = &disp.fb[disp.back];

	render();
	blit(fb);

	if (!disp.use_setcrtc) {
		struct drm_mode_crtc_page_flip pf = {
			.crtc_id = disp.crtc_id,
			.fb_id = fb->fb_id,
			.flags = DRM_MODE_PAGE_FLIP_EVENT,
		};
		if (xioctl(disp.fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) {
			disp.flip_pending = 1;
			disp.flip_sent_ns = now_ns(CLOCK_MONOTONIC);
			return;
		}
		disp.flip_errno = errno;
		disp.use_setcrtc = 1;
		logmsg("page flip failed: %s, using SETCRTC", strerror(errno));
	}

	struct drm_mode_crtc crtc = {
		.set_connectors_ptr = (uintptr_t)&disp.conn_id,
		.count_connectors = 1,
		.crtc_id = disp.crtc_id,
		.fb_id = fb->fb_id,
		.mode_valid = 1,
		.mode = disp.mode,
	};
	xioctl(disp.fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
	if (t_first_frame_ms < 0)
		t_first_frame_ms = now_ns(CLOCK_BOOTTIME) / 1000000;
	disp.back ^= 1;
	frames++;
}

static void on_flip(const struct drm_event_vblank *ev)
{
	static int64_t t0;
	static uint32_t seq0, last_seq;
	static uint64_t frames0;
	int64_t t = (int64_t)ev->tv_sec * 1000000000 + (int64_t)ev->tv_usec * 1000;

	if (frames > 0 && ev->sequence - last_seq > 1)
		missed_vblanks += ev->sequence - last_seq - 1;
	last_seq = ev->sequence;

	if (frames == 0 || frames - frames0 >= RATE_WINDOW) {
		if (frames > 0 && t > t0) {
			flip_hz = (frames - frames0) * 1e9 / (double)(t - t0);
			vblank_hz = (ev->sequence - seq0) * 1e9 / (double)(t - t0);
		}
		t0 = t;
		seq0 = ev->sequence;
		frames0 = frames;
	}

	/* The first drawn frame is now on screen. */
	if (t_first_frame_ms < 0) {
		t_first_frame_ms = now_ns(CLOCK_BOOTTIME) / 1000000;
		logmsg("first frame on screen at %lld ms",
		       (long long)t_first_frame_ms);
	}
	disp.flip_pending = 0;
	disp.back ^= 1;
	frames++;
}

static void drm_read_events(void)
{
	char buf[1024];
	ssize_t n = read(disp.fd, buf, sizeof(buf));

	for (ssize_t off = 0; n > 0 && off + (ssize_t)sizeof(struct drm_event) <= n;) {
		struct drm_event *e = (struct drm_event *)(buf + off);

		if (e->length < sizeof(*e))
			break;
		if (e->type == DRM_EVENT_FLIP_COMPLETE)
			on_flip((struct drm_event_vblank *)e);
		off += e->length;
	}
}

/* ---------------------------------------------------------------- input */

static void input_rescan(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d)) && ninputs < MAX_INPUTS) {
		char path[64];
		int num, fd, known = 0;

		if (sscanf(de->d_name, "event%d", &num) != 1)
			continue;
		for (int i = 0; i < ninputs; i++)
			known |= inputs[i].num == num;
		if (known)
			continue;
		snprintf(path, sizeof(path), "/dev/input/event%d", num);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		inputs[ninputs].fd = fd;
		inputs[ninputs].num = num;
		if (ioctl(fd, EVIOCGNAME(sizeof(inputs[0].name) - 1),
			  inputs[ninputs].name) < 0)
			strcpy(inputs[ninputs].name, "?");
		logmsg("input event%d: %s (at %lld ms)", num,
		       inputs[ninputs].name,
		       (long long)(now_ns(CLOCK_BOOTTIME) / 1000000));
		ninputs++;
	}
	closedir(d);
}

static void input_read(int idx)
{
	struct input_dev *dev = &inputs[idx];
	struct input_event ev[32];
	ssize_t n = read(dev->fd, ev, sizeof(ev));

	if (n < 0 && errno == ENODEV) {
		logmsg("input event%d removed", dev->num);
		close(dev->fd);
		inputs[idx] = inputs[--ninputs];
		memset(&inputs[ninputs], 0, sizeof(inputs[0]));
		return;
	}
	for (ssize_t i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
		if (ev[i].type == EV_KEY) {
			last_key.dev = dev->num;
			last_key.code = ev[i].code;
			last_key.value = ev[i].value;
			last_key.valid = 1;
			if (ev[i].code == KEY_POWER && ev[i].value == 1 &&
			    poweroff_at_ns < 0) {
				logmsg("power key pressed, powering off");
				/* Leave time for one frame with the message. */
				poweroff_at_ns = now_ns(CLOCK_MONOTONIC) + 300000000;
			}
		} else if (ev[i].type == EV_ABS) {
			last_abs.dev = dev->num;
			last_abs.code = ev[i].code;
			last_abs.value = ev[i].value;
			last_abs.valid = 1;
		}
	}
}

static void power_off(void)
{
	sync();
	if (!is_init)
		exit(0);
	reboot(RB_POWER_OFF);
	/* Still here: reboot failed or no pm_power_off. Keep running. */
	logmsg("power off failed: %s", strerror(errno));
	poweroff_at_ns = -1;
}

/* ----------------------------------------------------------------- main */

int main(void)
{
	int64_t next_rescan = 0, next_drm_try = 0;

	is_init = getpid() == 1;
	t_init_ms = now_ns(CLOCK_BOOTTIME) / 1000000;
	early_init();
	logmsg("started at %lld ms (CLOCK_BOOTTIME)", (long long)t_init_ms);

	for (;;) {
		struct pollfd pfd[MAX_INPUTS + 1];
		int64_t now = now_ns(CLOCK_MONOTONIC);
		int nfd = 0, timeout;

		if (poweroff_at_ns >= 0 && now >= poweroff_at_ns)
			power_off();

		if (now >= next_rescan) {
			input_rescan();
			next_rescan = now + RESCAN_MS * 1000000LL;
		}

		/* The panel driver may probe late; keep retrying. */
		if (disp.fd < 0 && now >= next_drm_try) {
			if (display_init())
				next_drm_try = now + 200000000;
		}

		if (disp.fd >= 0) {
			if (disp.flip_pending &&
			    now - disp.flip_sent_ns > FLIP_TIMEOUT_MS * 1000000LL) {
				/* Lost event: count it and move on. */
				flip_timeouts++;
				disp.flip_pending = 0;
				disp.back ^= 1;
			}
			if (!disp.flip_pending)
				present();
		}

		if (disp.fd >= 0) {
			pfd[nfd].fd = disp.fd;
			pfd[nfd].events = POLLIN;
			nfd++;
		}
		for (int i = 0; i < ninputs; i++) {
			pfd[nfd].fd = inputs[i].fd;
			pfd[nfd].events = POLLIN;
			nfd++;
		}

		timeout = (int)((next_rescan - now) / 1000000);
		if (disp.fd < 0 && timeout > 200)
			timeout = 200;
		if (disp.use_setcrtc && timeout > 16)
			timeout = 16;
		if (disp.flip_pending && timeout > FLIP_TIMEOUT_MS)
			timeout = FLIP_TIMEOUT_MS;
		if (poweroff_at_ns >= 0 && timeout > 50)
			timeout = 50;
		if (timeout < 0)
			timeout = 0;

		if (poll(pfd, nfd, timeout) <= 0)
			continue;

		int k = 0;
		if (disp.fd >= 0) {
			if (pfd[k].revents & POLLIN)
				drm_read_events();
			k++;
		}
		/* Walk backwards: input_read() may remove entries. */
		for (int i = ninputs - 1; i >= 0; i--)
			if (pfd[k + i].revents & (POLLIN | POLLERR | POLLHUP))
				input_read(i);
	}
}
