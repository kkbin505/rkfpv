/* What the OSD shows and where (no SDK calls: testable on the PC). */
#ifndef RKFPV_OSD_LAYOUT_H
#define RKFPV_OSD_LAYOUT_H

#include <stdint.h>

#include "osd.h"
#include "telem.h"

typedef struct {
	const telem_t *t;
	const char *rec;                /* "REC", "CARD FULL" or "" */
	int ul_ok, ul_rssi, ul_lq;      /* wfb uplink as received by the air unit */
	uint64_t now_ms;
	int temp_ok, temp_c;             /* SoC temperature, degrees C */
} osd_view_t;

/* one full redraw: begin, draw, commit */
void osd_draw_view(osd_t *o, const osd_view_t *v);

#endif
