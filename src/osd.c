#include "osd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rk_comm_rgn.h"
#include "rk_mpi_rgn.h"

#include "osd_font.h"

#define WHITE 0xFFFFFF
#define BLACK 0x000000

struct osd {
	RGN_HANDLE handle;
	MPP_CHN_S chn;
	int w, h;                       /* canvas size (16-aligned) */
	const osd_font_t *font;
	RGN_CANVAS_INFO_S canvas;       /* valid between osd_begin and osd_commit */
	int drawing;
};

osd_t *osd_create(int handle, int venc_chn, int width, int height)
{
	osd_t *o = calloc(1, sizeof *o);
	if (!o)
		return NULL;
	o->handle = handle;
	o->w = width & ~15;             /* region size and position: 16-aligned */
	o->h = height & ~15;
	o->font = height >= 1000 ? &osd_font_large : &osd_font_small;
	o->chn.enModId = RK_ID_VENC;
	o->chn.s32DevId = 0;
	o->chn.s32ChnId = venc_chn;

	RGN_ATTR_S a;
	memset(&a, 0, sizeof a);
	a.enType = OVERLAY_RGN;
	a.unAttr.stOverlay.enPixelFmt = RK_FMT_2BPP;
	a.unAttr.stOverlay.u32CanvasNum = 2;    /* draw into one while the encoder reads the other */
	a.unAttr.stOverlay.stSize.u32Width = o->w;
	a.unAttr.stOverlay.stSize.u32Height = o->h;
	RK_S32 r = RK_MPI_RGN_Create(handle, &a);
	if (r != RK_SUCCESS) {
		fprintf(stderr, "osd: RGN_Create(%d, %dx%d): %#x\n", handle, o->w, o->h, r);
		free(o);
		return NULL;
	}

	RGN_CHN_ATTR_S c;
	memset(&c, 0, sizeof c);
	c.bShow = RK_TRUE;
	c.enType = OVERLAY_RGN;
	c.unChnAttr.stOverlayChn.stPoint.s32X = 0;
	c.unChnAttr.stOverlayChn.stPoint.s32Y = 0;
	c.unChnAttr.stOverlayChn.u32BgAlpha = 0;
	c.unChnAttr.stOverlayChn.u32FgAlpha = 255;
	c.unChnAttr.stOverlayChn.u32Layer = (RK_U32)handle & 7;
	/* 2bpp pixel: bit 1 = opaque, bit 0 = LUT index. So 0 = transparent,
	 * 2 = LUT[0] (outline), 3 = LUT[1] (glyph). Seen on the board: with the
	 * two swapped, every stroke showed as a white outline around black. */
	c.unChnAttr.stOverlayChn.u32ColorLUT[0] = BLACK;
	c.unChnAttr.stOverlayChn.u32ColorLUT[1] = WHITE;
	r = RK_MPI_RGN_AttachToChn(handle, &o->chn, &c);
	if (r != RK_SUCCESS) {
		fprintf(stderr, "osd: RGN_AttachToChn(%d -> venc %d): %#x\n", handle, venc_chn, r);
		RK_MPI_RGN_Destroy(handle);
		free(o);
		return NULL;
	}
	fprintf(stderr, "osd: %dx%d 2bpp overlay on venc %d (font %dx%d)\n",
		o->w, o->h, venc_chn, o->font->w, o->font->h);
	return o;
}

void osd_destroy(osd_t *o)
{
	if (!o)
		return;
	RK_MPI_RGN_DetachFromChn(o->handle, &o->chn);
	RK_MPI_RGN_Destroy(o->handle);
	free(o);
}

int osd_width(const osd_t *o) { return o->w; }
int osd_height(const osd_t *o) { return o->h; }
int osd_line_height(const osd_t *o) { return o->font->h; }
int osd_char_width(const osd_t *o) { return o->font->w; }

int osd_begin(osd_t *o)
{
	RK_S32 r = RK_MPI_RGN_GetCanvasInfo(o->handle, &o->canvas);
	if (r != RK_SUCCESS || !o->canvas.u64VirAddr) {
		fprintf(stderr, "osd: GetCanvasInfo: %#x\n", r);
		return -1;
	}
	/* the other canvas still holds an older picture: start empty */
	memset((void *)(uintptr_t)o->canvas.u64VirAddr, 0,
	       (size_t)o->canvas.u32VirWidth * o->canvas.u32VirHeight / 4);
	o->drawing = 1;
	return 0;
}

int osd_commit(osd_t *o)
{
	if (!o->drawing)
		return -1;
	o->drawing = 0;
	RK_S32 r = RK_MPI_RGN_UpdateCanvas(o->handle);
	if (r != RK_SUCCESS) {
		fprintf(stderr, "osd: UpdateCanvas: %#x\n", r);
		return -1;
	}
	return 0;
}

/* 2 bits per pixel, the leftmost pixel in the HIGHEST bits (seen on the
 * board: lowest-first mirrored every 4 pixels, "NO FC DATA" became noise) */
#define PX_SHIFT(x) ((3 - ((x) & 3)) * 2)

static inline void put(osd_t *o, int x, int y, unsigned v)
{
	if (x < 0 || y < 0 || x >= o->w || y >= o->h)
		return;
	uint8_t *p = (uint8_t *)(uintptr_t)o->canvas.u64VirAddr +
		     ((size_t)y * o->canvas.u32VirWidth + x) / 4;
	int sh = PX_SHIFT(x);
	*p = (uint8_t)((*p & ~(3u << sh)) | (v << sh));
}

void osd_text(osd_t *o, int x, int y, const char *s)
{
	if (!o->drawing)
		return;
	const osd_font_t *f = o->font;
	for (; *s; s++, x += f->w - 2) {        /* cells overlap by the outline */
		int c = (unsigned char)*s;
		if (c < f->first || c > f->last)
			c = '?';
		const uint8_t *g = f->data + (size_t)(c - f->first) * f->w * f->h;
		for (int gy = 0; gy < f->h; gy++)
			for (int gx = 0; gx < f->w; gx++) {
				uint8_t v = g[gy * f->w + gx];
				if (v == 3)
					put(o, x + gx, y + gy, 3);
				else if (v == 2) {
					/* outline must not cut into the neighbour's glyph */
					int px = x + gx, py = y + gy;
					if (px >= 0 && py >= 0 && px < o->w && py < o->h) {
						uint8_t *p = (uint8_t *)(uintptr_t)o->canvas.u64VirAddr +
							     ((size_t)py * o->canvas.u32VirWidth + px) / 4;
						if (((*p >> PX_SHIFT(px)) & 3) != 3)
							put(o, px, py, 2);
					}
				}
			}
	}
}

void osd_text_right(osd_t *o, int right_x, int y, const char *s)
{
	int n = (int)strlen(s);
	osd_text(o, right_x - n * (o->font->w - 2) - 2, y, s);
}

/* black edge pixel that never covers white (crossing lines, corners) */
static inline void put_edge(osd_t *o, int x, int y)
{
	if (x < 0 || y < 0 || x >= o->w || y >= o->h)
		return;
	const uint8_t *p = (const uint8_t *)(uintptr_t)o->canvas.u64VirAddr +
			   ((size_t)y * o->canvas.u32VirWidth + x) / 4;
	if (((*p >> PX_SHIFT(x)) & 3) != 3)
		put(o, x, y, 2);
}

void osd_box(osd_t *o, int x, int y, int w, int h)
{
	if (!o->drawing || w < 8 || h < 8)
		return;
	/* clip to the canvas first: a huge box costs no more than a full one */
	int x1 = x + w - 1, y1 = y + h - 1;
	int cx0 = x < 0 ? 0 : x, cy0 = y < 0 ? 0 : y;
	int cx1 = x1 >= o->w ? o->w - 1 : x1, cy1 = y1 >= o->h ? o->h - 1 : y1;
	if (cx0 > cx1 || cy0 > cy1)
		return;
	/* white core (layers 1-2), then the black edges (layers 0 and 3) */
	for (int pass = 0; pass < 2; pass++)
		for (int l = 0; l < 4; l++) {
			int white = l == 1 || l == 2;
			if (white != (pass == 0))
				continue;
			for (int i = cx0; i <= cx1; i++) {
				if (i < x + l || i > x1 - l)
					continue;
				if (white) { put(o, i, y + l, 3); put(o, i, y1 - l, 3); }
				else { put_edge(o, i, y + l); put_edge(o, i, y1 - l); }
			}
			for (int j = cy0; j <= cy1; j++) {
				if (j < y + l || j > y1 - l)
					continue;
				if (white) { put(o, x + l, j, 3); put(o, x1 - l, j, 3); }
				else { put_edge(o, x + l, j); put_edge(o, x1 - l, j); }
			}
		}
}

void osd_cross(osd_t *o, int cx, int cy, int size)
{
	if (!o->drawing)
		return;
	for (int d = -size; d <= size; d++) {
		if (d > -4 && d < 4)
			continue;               /* gap in the middle */
		for (int t = -2; t <= 2; t++) {
			unsigned v = (t == -2 || t == 2) ? 2 : 3;   /* black edge, white core */
			put(o, cx + d, cy + t, v);
			put(o, cx + t, cy + d, v);
		}
	}
}
