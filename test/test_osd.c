/* Host test for src/osd.c: the RGN calls are stubbed with an in-memory 2BPP
 * canvas; the drawn frame is written as a PGM (0 transparent -> mid grey,
 * outline -> black, glyph -> white) for a visual check.
 *
 *   test_osd out.pgm [width height]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rk_comm_rgn.h"
#include "rk_mpi_rgn.h"
#include "../src/osd.h"
#include "../src/osd_layout.h"

static uint8_t *g_buf;
static int g_w, g_h, g_updates;

RK_S32 RK_MPI_RGN_Create(RGN_HANDLE h, const RGN_ATTR_S *a)
{
	(void)h;
	g_w = a->unAttr.stOverlay.stSize.u32Width;
	g_h = a->unAttr.stOverlay.stSize.u32Height;
	if (a->unAttr.stOverlay.enPixelFmt != RK_FMT_2BPP || g_w % 16 || g_h % 16)
		return -1;
	g_buf = calloc(1, (size_t)g_w * g_h / 4);
	return 0;
}
RK_S32 RK_MPI_RGN_Destroy(RGN_HANDLE h) { (void)h; free(g_buf); g_buf = NULL; return 0; }
RK_S32 RK_MPI_RGN_AttachToChn(RGN_HANDLE h, const MPP_CHN_S *c, const RGN_CHN_ATTR_S *a)
{
	(void)h; (void)c;
	const OVERLAY_CHN_ATTR_S *o = &a->unChnAttr.stOverlayChn;
	return (o->stPoint.s32X % 16 || o->stPoint.s32Y % 16) ? -1 : 0;
}
RK_S32 RK_MPI_RGN_DetachFromChn(RGN_HANDLE h, const MPP_CHN_S *c) { (void)h; (void)c; return 0; }
RK_S32 RK_MPI_RGN_GetCanvasInfo(RGN_HANDLE h, RGN_CANVAS_INFO_S *ci)
{
	(void)h;
	memset(ci, 0, sizeof *ci);
	memset(g_buf, 0x55, (size_t)g_w * g_h / 4);     /* garbage: osd_begin must clear */
	ci->u64VirAddr = (RK_U64)(uintptr_t)g_buf;
	ci->u32VirWidth = g_w;
	ci->u32VirHeight = g_h;
	return 0;
}
RK_S32 RK_MPI_RGN_UpdateCanvas(RGN_HANDLE h) { (void)h; g_updates++; return 0; }

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--default-layout")) {    /* checked against files/fpv */
		for (int i = 0; i < OSD_N; i++)
			printf("%s%s:%d:%d:%d", i ? "," : "", osd_elem_name[i], osd_default_layout[i].on,
			       osd_default_layout[i].x, osd_default_layout[i].y);
		printf("\n");
		return 0;
	}
	int w = argc > 3 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
	osd_t *o = osd_create(0, 0, w, h);
	if (!o) { printf("FAIL create\n"); return 1; }
	int lh = osd_line_height(o);
	/* sample flight: ArduCopter LOITER, armed 3:12, 4S pack */
	telem_t t;
	telem_init(&t);
	uint64_t now = 1000000;
	t.last_ms = now;
	t.have_hb = 1; t.custom_mode = 5; t.type = 2; t.autopilot = 3; t.armed = 1;
	t.armed_since_ms = now - 192000;
	t.have_sys = 1; t.volts = 15.4f; t.amps = 12.3f; t.remaining = 78; t.mah = 1234;
	t.rc_rssi = 251; t.rc_ms = now;
	osd_view_t v = { &t, "REC", 1, -48, 98, now, 1, 52, 1152, 648, 599, NULL };
	osd_draw_view(o, &v);
	/* clipping at the edges must not crash */
	osd_begin(o);
	osd_text(o, -5, osd_height(o) / 3, "clip left edge");
	osd_text(o, osd_width(o) - 40, osd_height(o) / 3 + lh, "clip right");
	g_updates = 0;
	osd_draw_view(o, &v);               /* final picture for the image */

	int fails = 0, counts[4] = { 0 };

	/* layout string */
	osd_pos_t pos[OSD_N];
	char def[512] = "";
	for (int i = 0; i < OSD_N; i++)
		snprintf(def + strlen(def), sizeof def - strlen(def), "%s%s:%d:%d:%d", i ? "," : "",
			 osd_elem_name[i], osd_default_layout[i].on, osd_default_layout[i].x, osd_default_layout[i].y);
	if (osd_layout_parse(def, pos) || memcmp(pos, osd_default_layout, sizeof pos)) { printf("FAIL: default string\n"); fails++; }
	if (osd_layout_parse("", pos) || memcmp(pos, osd_default_layout, sizeof pos)) { printf("FAIL: empty = defaults\n"); fails++; }
	if (osd_layout_parse("timer:0:10:20,future:1:5:5,lq:1:0:1000", pos) ||
	    pos[OSD_TIMER].on || pos[OSD_TIMER].x != 10 || pos[OSD_TIMER].y != 20 ||
	    pos[OSD_LQ].y != 1000 || pos[OSD_MODE].x != osd_default_layout[OSD_MODE].x) {
		printf("FAIL: partial layout / unknown name\n"); fails++;
	}
	static const char *bad[] = { "timer:2:1:1", "timer:1:1001:1", "timer:1:1", "timer:1:1:1;", "timer:1:-1:5",
				     "TIMER:1:1:1", "timer:1:1:1,,", NULL };
	for (int i = 0; bad[i]; i++) {
		memcpy(pos, osd_default_layout, sizeof pos);
		pos[0].x = 7;
		if (!osd_layout_parse(bad[i], pos) || pos[0].x != 7) { printf("FAIL: accepted '%s'\n", bad[i]); fails++; }
	}
	rkfpv_config_t cf;
	config_defaults(&cf);
	if (config_set(&cf, "osd_layout", "junk") || memcmp(cf.osd_pos, osd_default_layout, sizeof pos)) {
		printf("FAIL: bad osd_layout must not be fatal\n"); fails++;
	}

	/* hidden element draws nothing; a moved one draws where it was put */
	osd_layout_parse("mode:0:25:44,armed:0:25:81,timer:1:30:500,cross:0:500:500,lq:0:0:0,uplink:0:0:0,video:0:0:0,temp:0:0:0,rec:0:0:0", pos);
	v.pos = pos;
	osd_draw_view(o, &v);
	int top_left = 0, timer_px = 0;
	for (int y = 0; y < g_h / 10; y++)
		for (int x = 0; x < g_w / 4; x++)
			top_left += ((g_buf[(y * g_w + x) / 4] >> ((3 - (x & 3)) * 2)) & 3) == 3;
	for (int y = g_h / 2; y < g_h / 2 + lh; y++)
		for (int x = 0; x < g_w / 4; x++)
			timer_px += ((g_buf[(y * g_w + x) / 4] >> ((3 - (x & 3)) * 2)) & 3) == 3;
	if (top_left) { printf("FAIL: hidden mode still drawn (%d px)\n", top_left); fails++; }
	if (!timer_px) { printf("FAIL: moved timer not at its new place\n"); fails++; }
	v.pos = NULL;
	g_updates = 0;
	osd_draw_view(o, &v);               /* back to the default picture for the image */
	for (int i = 0; i < g_w * g_h; i++)
		counts[(g_buf[i / 4] >> ((3 - (i & 3)) * 2)) & 3]++;
	if (counts[1]) { printf("FAIL: %d pixels with unused code 1 (left from the garbage)\n", counts[1]); fails++; }
	if (!counts[2] || !counts[3]) { printf("FAIL: nothing drawn\n"); fails++; }
	if (g_updates != 1) { printf("FAIL: %d canvas updates\n", g_updates); fails++; }
	/* the canvas must be transparent away from the drawing */
	int x = g_w / 2 + g_w / 8, y = g_h / 2 + g_h / 8;
	if ((g_buf[(y * g_w + x) / 4] >> ((3 - (x & 3)) * 2)) & 3) { printf("FAIL: not cleared\n"); fails++; }

	FILE *f = fopen(argc > 1 ? argv[1] : "osd.pgm", "wb");
	fprintf(f, "P5 %d %d 255\n", g_w, g_h);
	static const uint8_t lut[4] = { 110, 255, 0, 255 };
	for (int i = 0; i < g_w * g_h; i++)
		fputc(lut[(g_buf[i / 4] >> ((3 - (i & 3)) * 2)) & 3], f);
	fclose(f);
	osd_destroy(o);
	printf("%s: transparent %d, outline %d, glyph %d px\n", fails ? "FAIL" : "ok", counts[0], counts[2], counts[3]);
	return fails ? 1 : 0;
}
