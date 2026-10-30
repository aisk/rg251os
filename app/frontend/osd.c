/*
 * The video player's on-screen display, drawn by the frontend over the
 * whole 480x320 logical screen after the frame is blitted, so with Fit
 * it uses the black bars, and the picture buffer of video_core.c is never
 * touched. Two strips of OSD_STRIP_H rows: at the top the video's name
 * (cut with "~") and on its right a note (the volume after a volume key,
 * "First video" or "Last video" after L/R at the folder's ends); at the
 * bottom a play symbol, two bars while paused or a square after the end,
 * the time as elapsed / total (m:ss, h:mm:ss for a video of an hour or
 * more) and a progress bar. Their background is the picture under them
 * at a quarter of its brightness (black over the bars), the colors those
 * of the menus.
 *
 * When it shows: for OSD_SHOW_NS after a video starts (osd_start), after
 * every control the player acted on (its event count, video_core_status:
 * A, a seek or its repeat, L/R, restart) and after a note; always while
 * paused and after the end. B toggles it: shown, it hides (also while
 * paused) until the next control; hidden, it stays until B again.
 *
 * Cost: nothing while hidden but reading the player's status. While
 * shown it is drawn again every frame, the game page flipping every
 * vblank anyway: the strips are built from the frame in a cached canvas
 * (darkened copy, then the text and shapes), and only their rows go into
 * the native buffer (blit_rows: one span per native row, writes only, the
 * scanout buffer is never read). So the time follows each second and the
 * picture under the strips stays live, paused or not. The cost per frame
 * drawn (average and maximum) is logged with the frames drawn, how often
 * it came up and the B toggles, at the end of the video and at unload.
 * B toggles are logged when they happen; the controls are logged by
 * video_core.c and main.c.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blit.h"
#include "core.h"
#include "draw.h"
#include "fe.h"
#include "input.h"
#include "menu.h"
#include "osd.h"
#include "romdb.h"

#define BAR_H 6
#define ICON_W 14
#define ICON_H 16

static uint16_t px[CANVAS_W * CANVAS_H];

static struct {
	char title[ROMDB_NAME_MAX + 2];
	char note[32];
	int64_t until, note_until;
	int pinned, hidden;   /* B: shown until B again / hidden until a control */
	unsigned events;      /* the player's count last seen */
	uint32_t prev_b;
	int was_shown;
	/* Statistics since osd_start. */
	unsigned frames, shows, toggles;
	int64_t ns, max_ns;
} o;

void osd_format_time(double t, int hours, char *buf, size_t size)
{
	long s = t > 0 ? (long)floor(t + 1e-6) : 0;

	if (hours)
		snprintf(buf, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60,
			 s % 60);
	else
		snprintf(buf, size, "%ld:%02ld", s / 60, s % 60);
}

static void show_for(int64_t now)
{
	o.hidden = 0;
	o.until = now + OSD_SHOW_NS;
}

static int visible(const struct video_status *st, int64_t now)
{
	if (o.pinned)
		return 1;
	if (o.hidden)
		return 0;
	return now < o.until || st->paused || st->ended;
}

/* A control the player acted on shows the OSD. */
static void track(const struct video_status *st, int64_t now)
{
	if (st->events != o.events) {
		o.events = st->events;
		show_for(now);
	}
}

void osd_start(const char *title, int64_t now)
{
	struct video_status st;

	video_core_status(&st);
	memset(&o, 0, sizeof(o));
	snprintf(o.title, sizeof(o.title), "%s", title);
	o.events = st.events;
	show_for(now);
}

void osd_note(const char *note, int64_t now)
{
	snprintf(o.note, sizeof(o.note), "%s", note);
	o.note_until = now + OSD_SHOW_NS;
	show_for(now);
}

int osd_shown(int64_t now)
{
	struct video_status st;

	video_core_status(&st);
	if (!st.loaded)
		return 0;
	track(&st, now);
	return visible(&st, now);
}

void osd_input(uint32_t held, int64_t now)
{
	uint32_t b = held & JOY_BIT(B);
	struct video_status st;

	if (b && !o.prev_b) {
		video_core_status(&st);
		track(&st, now);
		o.toggles++;
		if (visible(&st, now)) {
			o.pinned = 0;
			o.hidden = 1;
			o.until = 0;
		} else {
			o.pinned = 1;
			o.hidden = 0;
		}
		log_msg("video: OSD %s (B) at %.2f s", o.pinned ? "shown" :
			"hidden", st.pos);
	}
	o.prev_b = b;
}

/* Rows y0 .. y0 + h - 1 of the frame as blitted (scale, centered) at a
 * quarter of its brightness, black outside the picture. */
static void dim_rows(struct canvas *c, int y0, int h, const uint16_t *fr,
		     int fp, int fw, int fh)
{
	int k = 0, ox = 0, oy = 0;

	if (fr && fw > 0 && fh > 0) {
		k = c->w / fw < c->h / fh ? c->w / fw : c->h / fh;
		k = k < 1 ? 1 : k;
		ox = (c->w - fw * k) / 2;
		oy = (c->h - fh * k) / 2;
	}
	for (int y = y0; y < y0 + h; y++) {
		uint16_t *d = c->px + (long)y * c->w;
		int sy = y - oy;

		const uint16_t *s;

		memset(d, 0, (size_t)c->w * 2);
		if (!k || sy < 0 || sy >= fh * k)
			continue;
		s = fr + (long)(sy / k) * fp;
		for (int x = ox < 0 ? 0 : ox; x < c->w && x < ox + fw * k; x++)
			d[x] = (s[(x - ox) / k] >> 2) & 0x39e7;
	}
}

void osd_draw(struct canvas *c, const struct video_status *st,
	      const uint16_t *fr, int fp, int fw, int fh, int64_t now)
{
	const int ty = (OSD_STRIP_H - FONT_H) / 2, by = c->h - OSD_STRIP_H;
	int hours = st->duration >= 3600 || st->pos >= 3600;
	int right = c->w - 8, x, x0, x1, iy = by + (OSD_STRIP_H - ICON_H) / 2;
	char t[16], total[16];

	dim_rows(c, 0, OSD_STRIP_H, fr, fp, fw, fh);
	dim_rows(c, by, OSD_STRIP_H, fr, fp, fw, fh);

	/* Top: the name, and the note right aligned. */
	if (o.note[0] && now < o.note_until) {
		right -= draw_text_width(o.note);
		draw_text(c, right, ty, COL_FG, o.note);
		right -= FONT_W;
	}
	draw_rom_name(c, 8, ty, COL_HI, o.title, (right - 8) / FONT_W, 0);

	/* Bottom: the state, the time, the bar. */
	x = 10;
	if (st->ended) {
		draw_fill(c, x, iy + 1, ICON_W, ICON_W, COL_FG);
	} else if (st->paused) {
		draw_fill(c, x + 1, iy, 4, ICON_H, COL_FG);
		draw_fill(c, x + ICON_W - 5, iy, 4, ICON_H, COL_FG);
	} else {
		draw_triangle(c, x, iy, ICON_W, ICON_H, COL_FG);
	}
	x += ICON_W + 10;
	osd_format_time(st->pos, hours, t, sizeof(t));
	if (st->duration > 0)
		osd_format_time(st->duration, hours, total, sizeof(total));
	else
		snprintf(total, sizeof(total), "%s", hours ? "-:--:--" : "--:--");
	x = draw_text(c, x, by + ty, COL_FG, t);
	x = draw_textf(c, x, by + ty, COL_DIM, " / %s", total);
	x0 = x + 12;
	x1 = c->w - 12;
	if (x1 - x0 >= 20) {
		int w = x1 - x0, fill = 0, y = by + (OSD_STRIP_H - BAR_H) / 2;

		if (st->duration > 0)
			fill = (int)(w * fmin(st->pos / st->duration, 1.0));
		draw_fill(c, x0, y, w, BAR_H, COL_SEL);
		draw_fill(c, x0, y, fill, BAR_H, COL_HI);
	}
}

void osd_frame(uint16_t *dst, int dpitch, int nw, int nh, int rot,
	       const uint16_t *fr, int fp, int fw, int fh, int64_t now)
{
	int lw = rot == 0 ? nw : nh, lh = rot == 0 ? nh : nw;
	struct canvas c = { px, lw, lh };
	struct video_status st;
	int64_t t0, dt;
	int on;

	video_core_status(&st);
	if (!st.loaded)
		return;
	track(&st, now);
	on = visible(&st, now);
	o.shows += on && !o.was_shown;
	o.was_shown = on;
	if (!on || lw > CANVAS_W || lh > CANVAS_H)
		return;
	t0 = now_ns();
	osd_draw(&c, &st, fr, fp, fw, fh, now);
	blit_rows(dst, dpitch, nw, nh, rot, px, lw, 0, OSD_STRIP_H);
	blit_rows(dst, dpitch, nw, nh, rot, px + (long)(lh - OSD_STRIP_H) * lw,
		  lw, lh - OSD_STRIP_H, OSD_STRIP_H);
	dt = now_ns() - t0;
	o.frames++;
	o.ns += dt;
	o.max_ns = dt > o.max_ns ? dt : o.max_ns;
}

void osd_log_stats(const char *why)
{
	log_msg("video: OSD (%s): drawn on %u frames, avg %.3f ms max %.3f ms "
		"(2 strips of %d rows), came up %u times, %u B toggles", why,
		o.frames, o.frames ? o.ns / 1e6 / o.frames : 0.0, o.max_ns / 1e6,
		OSD_STRIP_H, o.shows, o.toggles);
}
