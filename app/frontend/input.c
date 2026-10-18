/*
 * evdev input with hot plug through inotify on /dev/input. The internal
 * gamepad is a USB device behind a hub and shows up ~2.3s after boot, i.e.
 * a few hundred ms after we start, as three nodes: "OpenSimHardware OSH PB
 * Controller" (buttons and axes), "... Keyboard" and "... Mouse".
 *
 * During the first 30s after a device appears, every key and abs event is
 * written to the log (rate limited) so the mapping can be checked from one
 * hardware run.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

#include "fe.h"
#include "input.h"
#include "libretro.h"

#define MAX_DEVS 16
#define TRACE_NS (30 * 1000000000LL)
#define TRACE_RATE 40  /* lines per second per device */
#define STICK_DEADZONE_PCT 40

#define J(x) RETRO_DEVICE_ID_JOYPAD_##x

/*
 * The whole mapping, in one place.
 *
 * OSH PB Controller (USB 1209:3100) on the RG351P/M. hid-generic maps its
 * HID buttons 1..12 sequentially from BTN_GAMEPAD (0x130), so the kernel
 * names do not match the labels on the shell. Sources, all agreeing:
 *  - pid351 (github.com/vinnyhorgan/pid351, docs/hardware.md), the same
 *    kind of PID 1 project on an RG351P with a mainline 6.12 kernel,
 *    measured by pressing every button: A 0x130, B 0x131, X 0x132,
 *    Y 0x133, L1 0x134, R1 0x135, START 0x136, SELECT 0x137, L3 0x138,
 *    R3 0x139, L2 0x13a, R2 0x13b; d-pad on ABS_HAT0X/Y; left stick on
 *    ABS_Z/ABS_RX, inverted on both axes (0 = full right / full down).
 *  - ROCKNIX retroarch autoconfig "OpenSimHardware OSH PB Controller.cfg"
 *    (mainline kernel): a=0 b=1 x=2 y=3 l=4 r=5 start=6 select=7 l3=8
 *    r3=9 l2=10 r2=11, hat 0 for the d-pad, left stick axes 0/1 with
 *    inverted signs. RetroArch numbers buttons in key code order, so
 *    index 0 is 0x130 and so on.
 *  - SDL gamecontrollerdb (dArkOS, PortMaster): b:b0 a:b1 x:b3 y:b2
 *    leftshoulder:b4 rightshoulder:b5 start:b6 back:b7 lefttrigger:b10
 *    righttrigger:b11, dp: h0, leftx:a0~ lefty:a1~ (inverted).
 * GBA only uses A, B, L, R, Start, Select and the d-pad.
 */
static const struct key_map {
	uint16_t code;
	uint8_t id;
} pad_keys[] = {
	{ 0x130, J(A) },       /* kernel BTN_SOUTH */
	{ 0x131, J(B) },       /* kernel BTN_EAST */
	{ 0x132, J(X) },       /* kernel BTN_C */
	{ 0x133, J(Y) },       /* kernel BTN_NORTH */
	{ 0x134, J(L) },       /* kernel BTN_WEST */
	{ 0x135, J(R) },       /* kernel BTN_Z */
	{ 0x136, J(START) },   /* kernel BTN_TL */
	{ 0x137, J(SELECT) },  /* kernel BTN_TR */
	{ 0x138, J(L3) },      /* kernel BTN_TL2 */
	{ 0x139, J(R3) },      /* kernel BTN_TR2 */
	{ 0x13a, J(L2) },      /* kernel BTN_SELECT */
	{ 0x13b, J(R2) },      /* kernel BTN_START */
	/* Some pads report d-pad buttons instead of a hat. */
	{ BTN_DPAD_UP, J(UP) },
	{ BTN_DPAD_DOWN, J(DOWN) },
	{ BTN_DPAD_LEFT, J(LEFT) },
	{ BTN_DPAD_RIGHT, J(RIGHT) },
};

/* A USB keyboard for development (not applied to the pad's own nodes). */
static const struct key_map kbd_keys[] = {
	{ KEY_UP, J(UP) }, { KEY_DOWN, J(DOWN) },
	{ KEY_LEFT, J(LEFT) }, { KEY_RIGHT, J(RIGHT) },
	{ KEY_X, J(A) }, { KEY_Z, J(B) }, { KEY_A, J(L) }, { KEY_S, J(R) },
	{ KEY_ENTER, J(START) }, { KEY_RIGHTSHIFT, J(SELECT) },
	{ KEY_BACKSPACE, J(SELECT) },
};

/* Axes: values below center press neg, above center press pos. */
static const struct axis_map {
	uint16_t code;
	uint8_t neg, pos;
} pad_axes[] = {
	{ ABS_HAT0X, J(LEFT), J(RIGHT) },
	{ ABS_HAT0Y, J(UP), J(DOWN) },
	{ ABS_Z, J(RIGHT), J(LEFT) },   /* OSH left stick X, inverted */
	{ ABS_RX, J(DOWN), J(UP) },     /* OSH left stick Y, inverted */
	{ ABS_X, J(LEFT), J(RIGHT) },   /* generic pads */
	{ ABS_Y, J(UP), J(DOWN) },
};

#define PAD_VENDOR 0x1209
#define PAD_PRODUCT 0x3100

struct axis_state {
	int present, live;  /* live: seen centered once, so not a stuck axis */
	int min, max, value;
	int logged;
};

struct idev {
	int fd, num;
	char name[80];
	struct input_id id;
	int is_pad_node;   /* any node of the OSH controller */
	int64_t opened_ns;
	int64_t trace_sec;
	int trace_lines;
	unsigned trace_dropped;
	int trace_done;
	uint32_t keys;     /* buttons from keys */
	struct axis_state ax[ARRAY_SIZE(pad_axes)];
};

static struct idev devs[MAX_DEVS];
static int ndevs;
static int ino_fd = -1;

static int test_bit(const unsigned long *bits, unsigned bit)
{
	return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1;
}

static int axis_dir(const struct axis_state *s)
{
	int span = s->max - s->min, center, off, dz;

	if (span <= 0)
		return 0;
	if (span <= 2) {  /* hat: -1, 0, 1 */
		return s->value < 0 ? -1 : s->value > 0 ? 1 : 0;
	}
	center = s->min + span / 2;
	off = s->value - center;
	dz = span / 2 * STICK_DEADZONE_PCT / 100;
	return off < -dz ? -1 : off > dz ? 1 : 0;
}

static void log_caps(struct idev *dv)
{
	unsigned long evbits[(EV_MAX + 1 + 63) / 64] = { 0 };
	unsigned long keybits[(KEY_MAX + 1 + 63) / 64] = { 0 };
	unsigned long absbits[(ABS_MAX + 1 + 63) / 64] = { 0 };
	unsigned long swbits[(SW_MAX + 1 + 63) / 64] = { 0 };
	char phys[80] = "", line[400];
	int off, cnt = 0;

	ioctl(dv->fd, EVIOCGPHYS(sizeof(phys) - 1), phys);
	ioctl(dv->fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
	log_msg("input event%d: \"%s\" bus %04x vendor %04x product %04x "
		"version %04x phys %s ev 0x%lx", dv->num, dv->name, dv->id.bustype,
		dv->id.vendor, dv->id.product, dv->id.version, phys, evbits[0]);

	if (test_bit(evbits, EV_KEY)) {
		ioctl(dv->fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
		off = snprintf(line, sizeof(line), "  keys:");
		for (unsigned k = 0; k <= KEY_MAX; k++) {
			if (!test_bit(keybits, k))
				continue;
			cnt++;
			if (off < (int)sizeof(line) - 12)
				off += snprintf(line + off, sizeof(line) - off,
						" 0x%x", k);
		}
		log_verbose("%s%s (%d)", line, off >= (int)sizeof(line) - 12 ?
			    " ..." : "", cnt);
	}
	if (test_bit(evbits, EV_ABS)) {
		ioctl(dv->fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
		for (unsigned c = 0; c <= ABS_MAX; c++) {
			struct input_absinfo ai;

			if (!test_bit(absbits, c) || ioctl(dv->fd, EVIOCGABS(c), &ai))
				continue;
			log_verbose("  abs 0x%02x: value %d min %d max %d fuzz %d "
				    "flat %d", c, ai.value, ai.minimum, ai.maximum,
				    ai.fuzz, ai.flat);
			for (size_t i = 0; i < ARRAY_SIZE(pad_axes); i++) {
				struct axis_state *s = &dv->ax[i];

				if (pad_axes[i].code != c)
					continue;
				s->present = 1;
				s->min = ai.minimum;
				s->max = ai.maximum;
				s->value = ai.value;
				s->live = axis_dir(s) == 0;
			}
		}
	}
	if (test_bit(evbits, EV_SW)) {
		ioctl(dv->fd, EVIOCGBIT(EV_SW, sizeof(swbits)), swbits);
		if (test_bit(swbits, SW_HEADPHONE_INSERT)) {
			unsigned long state[(SW_MAX + 1 + 63) / 64] = { 0 };

			ioctl(dv->fd, EVIOCGSW(sizeof(state)), state);
			log_msg("input event%d: headphone switch, now %s", dv->num,
				test_bit(state, SW_HEADPHONE_INSERT) ? "inserted" :
				"not inserted");
			on_jack(test_bit(state, SW_HEADPHONE_INSERT));
		}
	}
}

static void open_dev(int num)
{
	char path[64];
	struct idev *dv;
	int fd;

	for (int i = 0; i < ndevs; i++)
		if (devs[i].num == num)
			return;
	if (ndevs >= MAX_DEVS)
		return;
	snprintf(path, sizeof(path), "/dev/input/event%d", num);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	dv = &devs[ndevs++];
	memset(dv, 0, sizeof(*dv));
	dv->fd = fd;
	dv->num = num;
	dv->opened_ns = now_ns();
	if (ioctl(fd, EVIOCGNAME(sizeof(dv->name) - 1), dv->name) < 0)
		strcpy(dv->name, "?");
	ioctl(fd, EVIOCGID, &dv->id);
	dv->is_pad_node = dv->id.vendor == PAD_VENDOR &&
			  dv->id.product == PAD_PRODUCT;
	log_caps(dv);
}

void input_rescan(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	int num;

	if (!d)
		return;
	while ((e = readdir(d)))
		if (sscanf(e->d_name, "event%d", &num) == 1)
			open_dev(num);
	closedir(d);
}

int input_init(void)
{
	ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino_fd < 0)
		log_msg("inotify_init1: %s (falling back to rescans)", strerror(errno));
	else if (inotify_add_watch(ino_fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0)
		log_msg("inotify /dev/input: %s", strerror(errno));
	input_rescan();
	return 0;
}

int input_pollfds(struct pollfd *p, int max)
{
	int n = 0;

	if (ino_fd >= 0 && n < max) {
		p[n].fd = ino_fd;
		p[n].events = POLLIN;
		p[n++].revents = 0;
	}
	for (int i = 0; i < ndevs && n < max; i++) {
		p[n].fd = devs[i].fd;
		p[n].events = POLLIN;
		p[n++].revents = 0;
	}
	return n;
}

static void trace(struct idev *dv, const struct input_event *ev, int64_t now)
{
	int64_t sec;

	if (dv->trace_done)
		return;
	if (now - dv->opened_ns > TRACE_NS) {
		log_verbose("input event%d: event trace off after 30s (%u lines "
			    "dropped by rate limit)", dv->num, dv->trace_dropped);
		dv->trace_done = 1;
		return;
	}
	sec = now / 1000000000;
	if (sec != dv->trace_sec) {
		dv->trace_sec = sec;
		dv->trace_lines = 0;
	}
	if (dv->trace_lines++ >= TRACE_RATE) {
		dv->trace_dropped++;
		return;
	}
	log_verbose("in event%d %s 0x%03x = %d", dv->num,
		    ev->type == EV_KEY ? "key" : ev->type == EV_ABS ? "abs" : "sw",
		    ev->code, ev->value);
}

static void handle_key(struct idev *dv, unsigned code, int value)
{
	const struct key_map *map = pad_keys;
	size_t n = ARRAY_SIZE(pad_keys);

	if (value == 2)  /* autorepeat */
		return;
	if (code == KEY_POWER) {
		on_power_key(value);
		return;
	}
	if (code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN) {
		if (value == 1)
			on_volume_key(code == KEY_VOLUMEUP ? 1 : -1);
		return;
	}
	if (code == KEY_ESC || code == KEY_F1) {
		if (value)
			dv->keys |= BTN_BIT_MENU;
		else
			dv->keys &= ~BTN_BIT_MENU;
		return;
	}
	if (code < BTN_MISC) {
		if (dv->is_pad_node)
			return;
		map = kbd_keys;
		n = ARRAY_SIZE(kbd_keys);
	}
	for (size_t i = 0; i < n; i++)
		if (map[i].code == code) {
			if (value)
				dv->keys |= 1u << map[i].id;
			else
				dv->keys &= ~(1u << map[i].id);
		}
}

static void handle_abs(struct idev *dv, unsigned code, int value)
{
	for (size_t i = 0; i < ARRAY_SIZE(pad_axes); i++) {
		struct axis_state *s = &dv->ax[i];

		if (pad_axes[i].code != code || !s->present)
			continue;
		s->value = value;
		if (!s->live && axis_dir(s) == 0) {
			s->live = 1;
			log_verbose("input event%d: axis 0x%02x is live", dv->num, code);
		}
	}
}

static void read_dev(int idx)
{
	struct idev *dv = &devs[idx];
	struct input_event ev[64];
	ssize_t n = read(dv->fd, ev, sizeof(ev));
	int64_t now = now_ns();

	if (n < 0 && errno == ENODEV) {
		log_msg("input event%d removed", dv->num);
		close(dv->fd);
		devs[idx] = devs[--ndevs];
		return;
	}
	for (ssize_t i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
		switch (ev[i].type) {
		case EV_KEY:
			trace(dv, &ev[i], now);
			handle_key(dv, ev[i].code, ev[i].value);
			break;
		case EV_ABS:
			trace(dv, &ev[i], now);
			handle_abs(dv, ev[i].code, ev[i].value);
			break;
		case EV_SW:
			if (ev[i].code == SW_HEADPHONE_INSERT) {
				trace(dv, &ev[i], now);
				on_jack(ev[i].value);
			}
			break;
		}
	}
}

static void read_inotify(void)
{
	char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n = read(ino_fd, buf, sizeof(buf));
	int num;

	for (ssize_t off = 0; n > 0 && off < n;) {
		struct inotify_event *e = (struct inotify_event *)(buf + off);

		if (e->len && sscanf(e->name, "event%d", &num) == 1)
			open_dev(num);
		off += sizeof(*e) + e->len;
	}
}

void input_dispatch(const struct pollfd *p, int n)
{
	int k = 0, nd = ndevs;

	if (ino_fd >= 0 && k < n) {
		if (p[k].revents & POLLIN)
			read_inotify();
		k++;
	}
	/* Backwards: read_dev() may drop entries. New devices are at the end
	 * and have no pollfd yet. */
	for (int i = nd - 1; i >= 0; i--)
		if (k + i < n && i < ndevs &&
		    (p[k + i].revents & (POLLIN | POLLERR | POLLHUP)))
			read_dev(i);
}

/* ------------------------------------------------------------ power key */

int pwrkey_event(struct pwrkey *k, int down, int64_t now)
{
	if (down) {
		if (k->down)
			return PWR_NONE;
		k->down = 1;
		k->fired = 0;
		k->down_ns = now;
		if (k->swallow && now < k->swallow_until) {
			/* The press that woke the device. */
			k->fired = 1;
			k->swallow = 0;
			return PWR_WAKE_PRESS;
		}
		k->swallow = 0;
		return PWR_NONE;
	}
	/* A release ends a swallow window too: the wake press itself was
	 * not reported. */
	k->swallow = 0;
	if (!k->down)
		return PWR_NONE;
	k->down = 0;
	if (k->fired)
		return PWR_NONE;
	k->fired = 1;
	return now - k->down_ns < PWRKEY_LONG_NS ? PWR_SHORT : PWR_LONG;
}

int pwrkey_poll(struct pwrkey *k, int64_t now)
{
	if (k->swallow && now >= k->swallow_until)
		k->swallow = 0;
	if (k->down && !k->fired && now - k->down_ns >= PWRKEY_LONG_NS) {
		k->fired = 1;
		return PWR_LONG;
	}
	return PWR_NONE;
}

void pwrkey_wake(struct pwrkey *k, int held, int64_t now)
{
	k->swallow = 0;
	k->down = 0;
	k->fired = 0;
	if (held > 0) {
		/* Seen going down: its release and hold are not new presses. */
		k->down = 1;
		k->fired = 1;
		k->down_ns = now;
	} else if (held < 0) {
		/* Suspend to RAM: the press that woke us is reported after
		 * resume, if at all. Swallow one press that comes soon. */
		k->swallow = 1;
		k->swallow_until = now + PWRKEY_WAKE_NS;
	}
}

uint32_t input_buttons(void)
{
	uint32_t b = 0;

	for (int i = 0; i < ndevs; i++) {
		struct idev *dv = &devs[i];

		b |= dv->keys;
		for (size_t a = 0; a < ARRAY_SIZE(pad_axes); a++) {
			int dir;

			if (!dv->ax[a].present || !dv->ax[a].live)
				continue;
			dir = axis_dir(&dv->ax[a]);
			if (dir < 0)
				b |= 1u << pad_axes[a].neg;
			else if (dir > 0)
				b |= 1u << pad_axes[a].pos;
		}
	}
	return b;
}
