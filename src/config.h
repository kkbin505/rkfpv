#ifndef RKFPV_CONFIG_H
#define RKFPV_CONFIG_H

#include <stddef.h>

typedef enum { RC_CBR = 0, RC_VBR = 1 } rc_mode_t;

typedef struct {
	/* capture */
	int width;
	int height;
	int fps;
	int sensor_w;           /* sensor mode to ask for; 0 = the driver's default */
	int sensor_h;
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
