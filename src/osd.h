/*
 * OSD burned into the encoded video with the SDK's region (RGN) overlay:
 * the VENC hardware blends a 2-bit-per-pixel canvas into every frame, so
 * drawing costs CPU only when the content changes.
 *
 * One full-frame canvas per encoder channel (720p ~230 KB, 1080p ~520 KB
 * per buffer). 2BPP pixels: 0 transparent, 3 = LUT 0 (white), 2 = LUT 1
 * (black outline).
 *
 * Use: osd_begin() -> draw (osd_text, osd_cross, ...) -> osd_commit().
 */
#ifndef RKFPV_OSD_H
#define RKFPV_OSD_H

typedef struct osd osd_t;

/* handle: RGN handle (unique per region); venc_chn: encoder to attach to */
osd_t *osd_create(int handle, int venc_chn, int width, int height);
void osd_destroy(osd_t *o);

int osd_begin(osd_t *o);                /* get a cleared canvas; 0 = ok */
void osd_text(osd_t *o, int x, int y, const char *s);   /* top-left pixel */
void osd_text_right(osd_t *o, int right_x, int y, const char *s);
void osd_cross(osd_t *o, int cx, int cy, int size);
/* outline rectangle, top-left x y, outer size w x h: 2 px white between
 * 1 px black edges, like the font */
void osd_box(osd_t *o, int x, int y, int w, int h);
int osd_commit(osd_t *o);               /* show what was drawn; 0 = ok */

int osd_width(const osd_t *o);
int osd_height(const osd_t *o);
int osd_line_height(const osd_t *o);
int osd_char_width(const osd_t *o);

#endif
