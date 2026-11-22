#include "osd_layout.h"

#include <stdio.h>
#include <string.h>

#define STALE_MS 3000

/*  MODE            REC          T 03:12
 *  ARMED                         CPU 52C
 *                   +
 *                           1152x648 60fps
 *  1234 mAh                 UL -48dBm 98%
 *  16.4V 12.3A 78%                LQ 99%      */
void osd_draw_view(osd_t *o, const osd_view_t *v)
{
	if (!o || osd_begin(o))
		return;
	const telem_t *t = v->t;
	int w = osd_width(o), h = osd_height(o), lh = osd_line_height(o);
	int m = w / 40, top = m, bot = h - m - lh;
	char s[64];
	int fc = t->last_ms && v->now_ms - t->last_ms < STALE_MS;

	if (!fc) {
		osd_text(o, m, top, "NO FC DATA");
	} else {
		telem_mode_name(t, s, sizeof s);
		osd_text(o, m, top, s);
		if (t->have_hb)
			osd_text(o, m, top + lh, t->armed ? "ARMED" : "DISARMED");
	}
	if (v->rec && v->rec[0])
		osd_text(o, w / 2 - (int)strlen(v->rec) * (osd_char_width(o) - 2) / 2, top, v->rec);

	uint64_t fl = telem_flight_ms(t, v->now_ms) / 1000;
	snprintf(s, sizeof s, "T %02llu:%02llu", (unsigned long long)(fl / 60), (unsigned long long)(fl % 60));
	osd_text_right(o, w - m, top, s);

	if (v->temp_ok) {
		snprintf(s, sizeof s, "CPU %dC", v->temp_c);
		osd_text_right(o, w - m, top + lh, s);
	}

	if (fc && t->have_sys) {
		int n = snprintf(s, sizeof s, "%.1fV", t->volts);
		if (t->amps >= 0)
			n += snprintf(s + n, sizeof s - n, " %.1fA", t->amps);
		if (t->remaining >= 0)
			snprintf(s + n, sizeof s - n, " %d%%", t->remaining);
		osd_text(o, m, bot, s);
		if (t->mah >= 0) {
			snprintf(s, sizeof s, "%d mAh", t->mah);
			osd_text(o, m, bot - lh, s);
		}
	}

	if (v->vid_w > 0) {
		if (v->fps_x10 >= 0)
			snprintf(s, sizeof s, "%dx%d %dfps", v->vid_w, v->vid_h, (v->fps_x10 + 5) / 10);
		else
			snprintf(s, sizeof s, "%dx%d --fps", v->vid_w, v->vid_h);
		osd_text_right(o, w - m, bot - 2 * lh, s);
	}

	if (v->ul_ok)
		snprintf(s, sizeof s, "UL %ddBm %d%%", v->ul_rssi, v->ul_lq);
	else
		snprintf(s, sizeof s, "UL --");
	osd_text_right(o, w - m, bot - lh, s);

	if (fc && t->rc_rssi >= 0 && v->now_ms - t->rc_ms < STALE_MS)
		snprintf(s, sizeof s, "LQ %d%%", (t->rc_rssi * 100 + 127) / 254);
	else
		snprintf(s, sizeof s, "LQ --");
	osd_text_right(o, w - m, bot, s);

	osd_cross(o, w / 2, h / 2, h / 40);
	osd_commit(o);
}
