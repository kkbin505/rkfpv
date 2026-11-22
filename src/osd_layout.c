#include "osd_layout.h"

#include <stdio.h>
#include <string.h>

#define STALE_MS 3000

/* Default layout (osd_default_layout in config.c); every element can be
 * moved or hidden with osd_layout (the configurator's OSD tab):
 *
 *  MODE            REC          T 03:12
 *  ARMED                         CPU 52C
 *                   +
 *                           1152x648 60fps
 *  1234 mAh                 UL -48dBm 98%
 *  16.4V 12.3A 78%                LQ 99%      */

typedef struct {
	osd_t *o;
	const osd_pos_t *pos;
	int w, h, adv, lh;
} lay_t;

/* top-left of element i's box in pixels, kept inside the picture */
static void box(const lay_t *l, int i, int *x, int *y, int *bw)
{
	*bw = osd_elem_chars[i] * l->adv + 2;
	*x = l->pos[i].x * l->w / 1000;
	*y = l->pos[i].y * l->h / 1000;
	if (*x > l->w - *bw)
		*x = l->w - *bw;
	if (*y > l->h - l->lh)
		*y = l->h - l->lh;
	if (*x < 0)
		*x = 0;
	if (*y < 0)
		*y = 0;
}

static void put(const lay_t *l, int i, const char *s)
{
	if (!l->pos[i].on || !s[0])
		return;
	int x, y, bw;
	box(l, i, &x, &y, &bw);
	int tw = (int)strlen(s) * l->adv + 2, mid = x + bw / 2;
	if (mid > 2 * l->w / 3)
		x += bw - tw;                   /* right third: right-aligned */
	else if (mid >= l->w / 3)
		x += (bw - tw) / 2;             /* middle third: centred */
	osd_text(l->o, x, y, s);
}

void osd_draw_view(osd_t *o, const osd_view_t *v)
{
	if (!o || osd_begin(o))
		return;
	lay_t l = { o, v->pos ? v->pos : osd_default_layout, osd_width(o), osd_height(o),
		    osd_char_width(o) - 2, osd_line_height(o) };
	const telem_t *t = v->t;
	char s[64];
	int fc = t->last_ms && v->now_ms - t->last_ms < STALE_MS;

	if (!fc) {
		put(&l, OSD_MODE, "NO FC DATA");
	} else {
		telem_mode_name(t, s, sizeof s);
		put(&l, OSD_MODE, s);
		if (t->have_hb)
			put(&l, OSD_ARMED, t->armed ? "ARMED" : "DISARMED");
	}
	if (v->rec)
		put(&l, OSD_REC, v->rec);

	uint64_t fl = telem_flight_ms(t, v->now_ms) / 1000;
	snprintf(s, sizeof s, "T %02llu:%02llu", (unsigned long long)(fl / 60), (unsigned long long)(fl % 60));
	put(&l, OSD_TIMER, s);

	if (v->temp_ok) {
		snprintf(s, sizeof s, "CPU %dC", v->temp_c);
		put(&l, OSD_TEMP, s);
	}

	if (fc && t->have_sys) {
		int n = snprintf(s, sizeof s, "%.1fV", t->volts);
		if (t->amps >= 0)
			n += snprintf(s + n, sizeof s - n, " %.1fA", t->amps);
		if (t->remaining >= 0)
			snprintf(s + n, sizeof s - n, " %d%%", t->remaining);
		put(&l, OSD_BATTERY, s);
		if (t->mah >= 0) {
			snprintf(s, sizeof s, "%d mAh", t->mah);
			put(&l, OSD_MAH, s);
		}
	}

	if (v->vid_w > 0) {
		if (v->fps_x10 >= 0)
			snprintf(s, sizeof s, "%dx%d %dfps", v->vid_w, v->vid_h, (v->fps_x10 + 5) / 10);
		else
			snprintf(s, sizeof s, "%dx%d --fps", v->vid_w, v->vid_h);
		put(&l, OSD_VIDEO, s);
	}

	if (v->ul_ok)
		snprintf(s, sizeof s, "UL %ddBm %d%%", v->ul_rssi, v->ul_lq);
	else
		snprintf(s, sizeof s, "UL --");
	put(&l, OSD_UPLINK, s);

	if (fc && t->rc_rssi >= 0 && v->now_ms - t->rc_ms < STALE_MS)
		snprintf(s, sizeof s, "LQ %d%%", (t->rc_rssi * 100 + 127) / 254);
	else
		snprintf(s, sizeof s, "LQ --");
	put(&l, OSD_LQ, s);

	if (l.pos[OSD_CROSS].on)
		osd_cross(o, l.pos[OSD_CROSS].x * l.w / 1000, l.pos[OSD_CROSS].y * l.h / 1000, l.h / 40);
	osd_commit(o);
}
