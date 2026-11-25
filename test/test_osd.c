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
	osd_view_t v = { &t, "REC", 1, -48, 98, now, 1, 52, NULL, 0 };
	osd_draw_view(o, &v);
	/* clipping at the edges must not crash */
	osd_begin(o);
	osd_text(o, -5, osd_height(o) / 3, "clip left edge");
	osd_text(o, osd_width(o) - 40, osd_height(o) / 3 + lh, "clip right");
	/* shapes from another program (osd_ext): a box at grid 100,550 150x150 */
	static osd_ext_t ext;
	osd_ext_init(&ext);
	const char *cmd = "box 100 550 150 150\ntext 100 520 TARGET\n";
	osd_ext_parse(&ext, cmd, strlen(cmd), now);
	v.ext = &ext;
	v.ext_ttl_ms = 500;
	g_updates = 0;
	osd_draw_view(o, &v);               /* final picture for the image */

	int fails = 0, counts[4] = { 0 };
	for (int i = 0; i < g_w * g_h; i++)
		counts[(g_buf[i / 4] >> ((3 - (i & 3)) * 2)) & 3]++;
	if (counts[1]) { printf("FAIL: %d pixels with unused code 1 (left from the garbage)\n", counts[1]); fails++; }
	if (!counts[2] || !counts[3]) { printf("FAIL: nothing drawn\n"); fails++; }
	if (g_updates != 1) { printf("FAIL: %d canvas updates\n", g_updates); fails++; }
	/* the canvas must be transparent away from the drawing */
	int x = g_w / 2 + g_w / 8, y = g_h / 2 + g_h / 8;
	if ((g_buf[(y * g_w + x) / 4] >> ((3 - (x & 3)) * 2)) & 3) { printf("FAIL: not cleared\n"); fails++; }
	/* the box: black outer edge, white core, black inner edge, empty inside */
#define PX(x, y) ((g_buf[((y) * g_w + (x)) / 4] >> ((3 - ((x) & 3)) * 2)) & 3)
	int bx = 100 * g_w / 1000 + 40, by = 550 * g_h / 1000;
	if (PX(bx, by) != 2 || PX(bx, by + 1) != 3 || PX(bx, by + 2) != 3 || PX(bx, by + 3) != 2 ||
	    PX(bx, by + 10) != 0) {
		printf("FAIL: box edge %d %d %d %d inside %d\n", PX(bx, by), PX(bx, by + 1),
		       PX(bx, by + 2), PX(bx, by + 3), PX(bx, by + 10));
		fails++;
	}
	int cx = 100 * g_w / 1000 + 1, cy = 550 * g_h / 1000 + 1;     /* corner stays white */
	if (PX(cx, cy) != 3) { printf("FAIL: box corner %d\n", PX(cx, cy)); fails++; }
	/* expired: the next redraw has no box */
	v.now_ms = now + 501;
	osd_draw_view(o, &v);
	if (PX(bx, by + 1) != 0) { printf("FAIL: box still there after the ttl\n"); fails++; }
	v.now_ms = now;
	osd_draw_view(o, &v);               /* picture with the box again */
	g_updates = 1;

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
