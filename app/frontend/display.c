/*
 * DRM/KMS output with raw ioctls (no libdrm). Verified on the device: one
 * DSI connector, 320x480@60.02, RG16 scanout, page flip events on vblank.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include "display.h"
#include "fe.h"

/*
 * Software rotation from logical landscape to the native portrait buffer,
 * in degrees counter clockwise, matching the DT "rotation" property of the
 * panel (270 on RG351P/M, same as fbcon=rotate:3). Verified on the device.
 */
#define ROTATION 270
#define FLIP_TIMEOUT_MS 200

struct fb {
	uint32_t handle, pitch, fb_id;
	uint64_t size;
	uint8_t *map;
};

static struct {
	int fd;
	uint32_t conn_id, crtc_id;
	struct drm_mode_modeinfo mode;
	struct fb fb[2];
	int back;
	int flip_pending;
	int64_t flip_sent_ns;
	int use_setcrtc;
	uint32_t last_seq;
	int have_seq;
	uint32_t missed;
	uint64_t frames;
	int verbose;
	struct display_info info;
} d = { .fd = -1, .verbose = 1 };

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

static void destroy_fb(int fd, struct fb *fb)
{
	if (fb->map && fb->map != MAP_FAILED)
		munmap(fb->map, fb->size);
	if (fb->fb_id)
		xioctl(fd, DRM_IOCTL_MODE_RMFB, &fb->fb_id);
	if (fb->handle) {
		struct drm_mode_destroy_dumb dd = { .handle = fb->handle };
		xioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
	}
	memset(fb, 0, sizeof(*fb));
}

static int create_fb(int fd, struct fb *fb, uint32_t w, uint32_t h)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 16 };
	struct drm_mode_fb_cmd2 f = { .width = w, .height = h,
				      .pixel_format = DRM_FORMAT_RGB565 };
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

int display_init(void)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t *conns = NULL, *crtcs = NULL, *encs = NULL;
	struct drm_mode_modeinfo *modes = NULL;
	struct drm_mode_get_connector conn;
	struct drm_mode_crtc crtc;
	int fd;

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		if (d.verbose)
			log_msg("open /dev/dri/card0: %s", strerror(errno));
		d.verbose = 0;
		return -1;
	}
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

	for (uint32_t i = 0; i < res.count_connectors && !d.conn_id; i++) {
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
		enc.encoder_id = conn.encoder_id;
		if (conn.encoder_id && !xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) &&
		    enc.crtc_id) {
			d.crtc_id = enc.crtc_id;
		} else {
			for (uint32_t e = 0; e < conn.count_encoders && !d.crtc_id; e++) {
				memset(&enc, 0, sizeof(enc));
				enc.encoder_id = encs[e];
				if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc))
					continue;
				for (uint32_t c = 0; c < res.count_crtcs; c++)
					if (enc.possible_crtcs & (1u << c)) {
						d.crtc_id = crtcs[c];
						break;
					}
			}
		}
		if (d.crtc_id) {
			d.conn_id = conn.connector_id;
			d.mode = modes[mi];
		}
next:
		free(modes);
		free(encs);
		modes = NULL;
		encs = NULL;
	}
	if (!d.conn_id) {
		if (d.verbose)
			log_msg("display: no connected connector with a CRTC");
		goto out;
	}

	if (create_fb(fd, &d.fb[0], d.mode.hdisplay, d.mode.vdisplay) ||
	    create_fb(fd, &d.fb[1], d.mode.hdisplay, d.mode.vdisplay)) {
		log_msg("display: dumb buffer/addfb2 RG16 failed: %s", strerror(errno));
		goto fail_fb;
	}
	memset(&crtc, 0, sizeof(crtc));
	crtc.set_connectors_ptr = (uintptr_t)&d.conn_id;
	crtc.count_connectors = 1;
	crtc.crtc_id = d.crtc_id;
	crtc.fb_id = d.fb[0].fb_id;
	crtc.mode_valid = 1;
	crtc.mode = d.mode;
	if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc)) {
		log_msg("display: setcrtc failed: %s", strerror(errno));
		goto fail_fb;
	}

	d.info.native_w = d.mode.hdisplay;
	d.info.native_h = d.mode.vdisplay;
	d.info.rotation = d.mode.hdisplay < d.mode.vdisplay ? ROTATION : 0;
	if (d.info.rotation) {
		d.info.logical_w = d.mode.vdisplay;
		d.info.logical_h = d.mode.hdisplay;
	} else {
		d.info.logical_w = d.mode.hdisplay;
		d.info.logical_h = d.mode.vdisplay;
	}
	d.info.refresh_hz = d.mode.htotal && d.mode.vtotal ?
		d.mode.clock * 1000.0 / ((double)d.mode.htotal * d.mode.vtotal) :
		60.0;
	d.fd = fd;
	d.back = 1;
	log_msg("display: connector %u crtc %u mode %s %ux%u, %.4f Hz "
		"(clock %u kHz, htotal %u, vtotal %u), pitch %u, rotation %d",
		d.conn_id, d.crtc_id, d.mode.name, d.mode.hdisplay,
		d.mode.vdisplay, d.info.refresh_hz, d.mode.clock, d.mode.htotal,
		d.mode.vtotal, d.fb[0].pitch, d.info.rotation);
	goto out;

fail_fb:
	destroy_fb(fd, &d.fb[0]);
	destroy_fb(fd, &d.fb[1]);
out:
	free(conns);
	free(crtcs);
	if (d.fd < 0) {
		close(fd);
		d.conn_id = d.crtc_id = 0;
		d.verbose = 0;
		return -1;
	}
	return 0;
}

int display_ready(void)
{
	return d.fd >= 0;
}

int display_fd(void)
{
	return d.fd;
}

const struct display_info *display_info(void)
{
	return &d.info;
}

int display_can_draw(void)
{
	return d.fd >= 0 && !d.flip_pending;
}

uint16_t *display_backbuf(int *pitch_px)
{
	*pitch_px = d.fb[d.back].pitch / 2;
	return (uint16_t *)d.fb[d.back].map;
}

void display_present(void)
{
	struct fb *fb = &d.fb[d.back];

	if (!d.use_setcrtc) {
		struct drm_mode_crtc_page_flip pf = {
			.crtc_id = d.crtc_id,
			.fb_id = fb->fb_id,
			.flags = DRM_MODE_PAGE_FLIP_EVENT,
		};
		if (xioctl(d.fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) {
			d.flip_pending = 1;
			d.flip_sent_ns = now_ns();
			return;
		}
		d.use_setcrtc = 1;
		log_msg("display: page flip failed: %s, using SETCRTC",
			strerror(errno));
	}

	struct drm_mode_crtc crtc = {
		.set_connectors_ptr = (uintptr_t)&d.conn_id,
		.count_connectors = 1,
		.crtc_id = d.crtc_id,
		.fb_id = fb->fb_id,
		.mode_valid = 1,
		.mode = d.mode,
	};
	xioctl(d.fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
	d.back ^= 1;
	d.frames++;
}

static void on_flip(const struct drm_event_vblank *ev)
{
	if (d.have_seq && ev->sequence - d.last_seq > 1)
		d.missed += ev->sequence - d.last_seq - 1;
	d.last_seq = ev->sequence;
	d.have_seq = 1;
	if (d.frames == 0)
		log_msg("display: first frame on screen");
	d.flip_pending = 0;
	d.back ^= 1;
	d.frames++;
}

void display_handle_events(void)
{
	char buf[1024];
	ssize_t n = read(d.fd, buf, sizeof(buf));

	for (ssize_t off = 0; n > 0 && off + (ssize_t)sizeof(struct drm_event) <= n;) {
		struct drm_event *e = (struct drm_event *)(buf + off);

		if (e->length < sizeof(*e))
			break;
		if (e->type == DRM_EVENT_FLIP_COMPLETE)
			on_flip((struct drm_event_vblank *)e);
		off += e->length;
	}
}

int display_poll_timeout(int64_t now)
{
	static unsigned timeouts;

	if (d.fd < 0)
		return 200;
	if (d.use_setcrtc)
		return 16;
	if (!d.flip_pending)
		return 0;
	if (now - d.flip_sent_ns > FLIP_TIMEOUT_MS * 1000000LL) {
		/* Lost event: move on rather than freeze. */
		if (timeouts++ < 10)
			log_msg("display: flip event timeout");
		d.flip_pending = 0;
		d.have_seq = 0;
		d.back ^= 1;
		return 0;
	}
	return FLIP_TIMEOUT_MS;
}

/* Connector property by name: its id (0 if none) and current value. */
static uint32_t conn_prop(const char *name, uint64_t *value)
{
	struct drm_mode_obj_get_properties op = {
		.obj_id = d.conn_id,
		.obj_type = DRM_MODE_OBJECT_CONNECTOR,
	};
	uint32_t ids[32];
	uint64_t vals[32];

	if (xioctl(d.fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
		return 0;
	if (op.count_props > ARRAY_SIZE(ids))
		op.count_props = ARRAY_SIZE(ids);
	op.props_ptr = (uintptr_t)ids;
	op.prop_values_ptr = (uintptr_t)vals;
	if (xioctl(d.fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
		return 0;
	for (uint32_t i = 0; i < op.count_props && i < ARRAY_SIZE(ids); i++) {
		struct drm_mode_get_property gp = { .prop_id = ids[i] };

		if (xioctl(d.fd, DRM_IOCTL_MODE_GETPROPERTY, &gp))
			continue;
		if (!strcmp(gp.name, name)) {
			*value = vals[i];
			return ids[i];
		}
	}
	return 0;
}

/* Wait (bounded) for a pending flip, so none is in flight while off. */
static void wait_flip(int ms)
{
	int64_t end = now_ns() + ms * 1000000LL;

	while (d.flip_pending && now_ns() < end) {
		struct pollfd p = { .fd = d.fd, .events = POLLIN };

		if (poll(&p, 1, 20) > 0)
			display_handle_events();
	}
	if (d.flip_pending) {
		log_msg("display: flip still pending after %d ms", ms);
		d.flip_pending = 0;
		d.back ^= 1;
	}
}

int display_set_power(int on)
{
	struct drm_mode_obj_set_property sp = {
		.value = on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF,
		.obj_id = d.conn_id,
		.obj_type = DRM_MODE_OBJECT_CONNECTOR,
	};
	uint64_t cur = 0;
	int r;

	if (d.fd < 0)
		return -1;
	wait_flip(100);
	sp.prop_id = conn_prop("DPMS", &cur);
	if (!sp.prop_id) {
		log_msg("display: no DPMS property");
		return -1;
	}
	r = xioctl(d.fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &sp);
	if (r)
		log_msg("display: DPMS %s: %s", on ? "on" : "off", strerror(errno));
	else
		log_msg("display: DPMS %s (was %llu)", on ? "on" : "off",
			(unsigned long long)cur);
	display_resync();
	return r;
}

void display_resync(void)
{
	d.have_seq = 0;
	d.missed = 0;
}

uint32_t display_take_missed(void)
{
	uint32_t m = d.missed;

	d.missed = 0;
	return m;
}

uint64_t display_frames(void)
{
	return d.frames;
}
