#ifndef RKFPV_CONFIG_H
#define RKFPV_CONFIG_H

#include <stddef.h>

typedef enum { RC_CBR = 0, RC_VBR = 1 } rc_mode_t;

typedef struct {
	/* capture */
	int width;
	int height;
	int fps;
	int vi_chn;             /* 0: rkisp_mainpath */
	char iq_dir[128];       /* empty: skip AIQ (colors will be wrong) */
	int mirror;
	int flip;

	/* encoder */
	rc_mode_t rc;
	int bitrate_kbps;       /* target (CBR) / average (VBR) */
	int max_bitrate_kbps;   /* VBR only; 0 = bitrate * 3/2 */
	int gop;                /* frames between IDR */
	int stat_time;          /* RC statistics window, seconds */
	int slice_bytes;        /* 0 = one slice per frame */
	int intra_refresh;      /* 0 = off, else rows refreshed per frame */

	/* output */
	char dst_ip[64];
	int dst_port;
	int pkt_size;           /* max RTP packet size (mlink - 100 in OpenIPC) */
	int pt;                 /* RTP payload type: 97 = H.265 as majestic sends it */

	/* DVR (TF card) */
	int dvr;                /* record from start */
	char dvr_dir[128];
	int dvr_segment;        /* seconds per file */
	int dvr_min_free;       /* MB kept free */
	int dvr_overwrite;      /* delete oldest files when full */
	int dvr_ts;             /* 1: .ts, 0: fragmented .mp4 */
	int dvr_buffer_kb;      /* ring between stream loop and card writer */
	/* separate DVR stream (VI main path -> its own encoder); width 0 =
	 * record the link stream instead */
	int dvr_width, dvr_height;
	rc_mode_t dvr_rc;
	int dvr_bitrate_kbps;
	int dvr_gop;

	int stats_interval;     /* seconds, 0 = off */
} rkfpv_config_t;

void config_defaults(rkfpv_config_t *c);

/* Loads key=value pairs from path. Missing file is not an error (returns 0).
 * Returns -1 on a bad value, with a message on stderr. */
int config_load_file(rkfpv_config_t *c, const char *path);

/* Applies one key=value. Returns 0 if ok, -1 if unknown key or bad value. */
int config_set(rkfpv_config_t *c, const char *key, const char *val);

void config_print(const rkfpv_config_t *c);

#endif
