#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(rkfpv_config_t *c)
{
	memset(c, 0, sizeof(*c));
	c->width = 1280;
	c->height = 720;
	c->fps = 30;
	c->vi_chn = 0;
	snprintf(c->iq_dir, sizeof(c->iq_dir), "/oem/usr/share/iqfiles");

	c->rc = RC_CBR;
	c->bitrate_kbps = 3500;
	c->max_bitrate_kbps = 0;
	c->gop = 30;
	c->stat_time = 1;
	c->slice_bytes = 0;
	c->intra_refresh = 0;

	snprintf(c->dst_ip, sizeof(c->dst_ip), "127.0.0.1");
	c->dst_port = 5600;
	c->preview_port = 5602;
	c->pkt_size = 1400;
	/* majestic sends H.264 as 96 and H.265 as 97; PixelPilot and other
	 * OpenIPC ground stations pick the decoder from it */
	c->pt = 97;

	c->dvr = 0;             /* off: the 64 MB board has no RAM to spare */
	snprintf(c->dvr_dir, sizeof(c->dvr_dir), "/mnt/dvr");
	c->dvr_segment = 600;
	c->dvr_min_free = 300;
	c->dvr_overwrite = 1;
	c->dvr_ts = 0;
	c->dvr_buffer_kb = 1024;
	c->dvr_width = 0;       /* "link": record the radio stream, no 2nd encoder */
	c->dvr_height = 0;
	c->dvr_rc = RC_VBR;     /* quality where the scene needs it; the card has room */
	c->dvr_bitrate_kbps = 8000;
	c->dvr_gop = 30;        /* 1 s: fMP4 fragment and seek granularity */
	c->dvr_osd = 1;
	c->blackbox = 0;        /* bblog's keys: rkfpv only accepts them */
	c->blackbox_baud = 1500000;

	/* iq = whatever the active IQ file sets (a custom IQ may set its own) */
	c->isp.brightness = c->isp.contrast = c->isp.saturation = c->isp.hue = ISP_IQ;
	c->isp.sharpness = ISP_IQ;
	c->isp.exp_max_ms = ISP_IQ;
	c->isp.gain_max = ISP_IQ;
	c->isp.antiflicker = ISP_IQ;
	c->isp.ae_delay = ISP_IQ;
	c->isp.ae_damp = ISP_IQ;
	/* bottom-weighted metering + highlight comp: widest usable range with the
	 * ground visible against a bright sky (AE metering DOE, 3 video modes) */
	c->isp.metering = METER_BOTTOM_STRONG;
	c->isp.blc = 0;
	c->isp.hlc = 50;
	c->isp.nr_spatial = 0;  /* off: no smearing on fast motion (user's choice) */
	c->isp.nr_temporal = 0;
	c->isp.dehaze = ISP_IQ;
	c->isp.wb = WB_AUTO;

	c->osd = 0;             /* experimental, off until tested on hardware */
	c->osd_ext_port = 0;    /* other programs draw into the OSD (osd_ext.h) */
	c->osd_ext_ttl_ms = 500;
	c->frame_tap = 0;       /* small grey copy in shared memory (frametap.h) */
	c->frame_tap_fps = 10;
	c->frame_tap_scale = 2;

	c->stats_interval = 5;
}

static int parse_int(const char *v, int lo, int hi, int *out)
{
	char *end;
	long n = strtol(v, &end, 10);
	if (end == v || *end != '\0' || n < lo || n > hi)
		return -1;
	*out = (int)n;
	return 0;
}

static int parse_size(const char *v, int *w, int *h)
{
	int a, b;
	char x;
	if (sscanf(v, "%d%c%d", &a, &x, &b) != 3 || (x != 'x' && x != 'X'))
		return -1;
	if (a < 64 || a > 4096 || b < 64 || b > 4096 || (a & 1) || (b & 1))
		return -1;
	*w = a;
	*h = b;
	return 0;
}

/* "iq" -> ISP_IQ, "off" -> 0 where allowed, else a number in lo..hi */
static int parse_isp(const char *v, int lo, int hi, int allow_off, int *out)
{
	if (!strcmp(v, "iq")) { *out = ISP_IQ; return 0; }
	if (allow_off && !strcmp(v, "off")) { *out = 0; return 0; }
	return parse_int(v, lo, hi, out);
}

static const char *const WB_NAMES[] = {
	"auto", "daylight", "cloudy", "shade", "twilight", "fluorescent", "incandescent", NULL
};

/* index = METER_* value; [0] is "iq" (ISP_IQ) */
static const char *const METER_NAMES[] = { "iq", "average", "bottom", "bottom_strong", NULL };

#define ISP_KEY(name, field, lo, hi, off) \
	if (!strcmp(key, name)) return parse_isp(val, lo, hi, off, &c->isp.field)

#define INT_KEY(name, field, lo, hi) \
	if (!strcmp(key, name)) return parse_int(val, lo, hi, &c->field)

int config_set(rkfpv_config_t *c, const char *key, const char *val)
{
	if (!strcmp(key, "size"))
		return parse_size(val, &c->width, &c->height);
	if (!strcmp(key, "rc")) {
		if (!strcmp(val, "cbr")) { c->rc = RC_CBR; return 0; }
		if (!strcmp(val, "vbr")) { c->rc = RC_VBR; return 0; }
		return -1;
	}
	if (!strcmp(key, "dst")) {
		snprintf(c->dst_ip, sizeof(c->dst_ip), "%s", val);
		return 0;
	}
	if (!strcmp(key, "isp_wb")) {
		for (int i = 0; WB_NAMES[i]; i++)
			if (!strcmp(val, WB_NAMES[i])) { c->isp.wb = i; return 0; }
		return -1;
	}
	if (!strcmp(key, "isp_metering")) {
		for (int i = 0; METER_NAMES[i]; i++)
			if (!strcmp(val, METER_NAMES[i])) { c->isp.metering = i ? i : ISP_IQ; return 0; }
		return -1;
	}
	if (!strcmp(key, "isp_antiflicker")) {
		if (!strcmp(val, "50")) { c->isp.antiflicker = 50; return 0; }
		if (!strcmp(val, "60")) { c->isp.antiflicker = 60; return 0; }
		return parse_isp(val, 0, 0, 1, &c->isp.antiflicker);
	}
	ISP_KEY("isp_brightness", brightness, 0, 255, 0);
	ISP_KEY("isp_contrast", contrast, 0, 255, 0);
	ISP_KEY("isp_saturation", saturation, 0, 255, 0);
	ISP_KEY("isp_hue", hue, 0, 255, 0);
	ISP_KEY("isp_sharpness", sharpness, 0, 100, 0);
	ISP_KEY("isp_exp_max", exp_max_ms, 1, 33, 0);
	ISP_KEY("isp_gain_max", gain_max, 1, 512, 0);
	ISP_KEY("isp_ae_delay", ae_delay, 0, 4, 0);
	ISP_KEY("isp_ae_damp", ae_damp, 0, 100, 0);
	ISP_KEY("isp_blc", blc, 0, 100, 1);
	ISP_KEY("isp_hlc", hlc, 0, 100, 1);
	ISP_KEY("isp_nr_spatial", nr_spatial, 1, 100, 1);
	ISP_KEY("isp_nr_temporal", nr_temporal, 1, 100, 1);
	ISP_KEY("isp_dehaze", dehaze, 1, 100, 1);
	if (!strcmp(key, "dvr_size")) {
		if (!strcmp(val, "link")) { c->dvr_width = c->dvr_height = 0; return 0; }
		return parse_size(val, &c->dvr_width, &c->dvr_height);
	}
	if (!strcmp(key, "dvr_rc")) {
		if (!strcmp(val, "cbr")) { c->dvr_rc = RC_CBR; return 0; }
		if (!strcmp(val, "vbr")) { c->dvr_rc = RC_VBR; return 0; }
		return -1;
	}
	if (!strcmp(key, "dvr_format")) {
		if (!strcmp(val, "mp4")) { c->dvr_ts = 0; return 0; }
		if (!strcmp(val, "ts")) { c->dvr_ts = 1; return 0; }
		return -1;
	}
	if (!strcmp(key, "dvr_dir")) {
		snprintf(c->dvr_dir, sizeof c->dvr_dir, "%s", val);
		return 0;
	}
	if (!strcmp(key, "iq_dir")) {
		snprintf(c->iq_dir, sizeof(c->iq_dir), "%s", val);
		return 0;
	}
	INT_KEY("fps", fps, 1, 120);
	INT_KEY("vi_chn", vi_chn, 0, 2);
	INT_KEY("mirror", mirror, 0, 1);
	INT_KEY("flip", flip, 0, 1);
	INT_KEY("bitrate", bitrate_kbps, 100, 50000);
	INT_KEY("max_bitrate", max_bitrate_kbps, 0, 50000);
	INT_KEY("gop", gop, 1, 65536);
	INT_KEY("stat_time", stat_time, 1, 60);
	INT_KEY("slice_bytes", slice_bytes, 0, 1 << 20);
	INT_KEY("intra_refresh", intra_refresh, 0, 64);
	INT_KEY("port", dst_port, 1, 65535);
	INT_KEY("pkt_size", pkt_size, 200, 4000);
	INT_KEY("pt", pt, 96, 127);
	INT_KEY("preview_port", preview_port, 0, 65535);
	INT_KEY("stats", stats_interval, 0, 3600);
	INT_KEY("dvr", dvr, 0, 1);
	INT_KEY("osd", osd, 0, 1);
	INT_KEY("osd_ext_port", osd_ext_port, 0, 65535);
	INT_KEY("osd_ext_ttl_ms", osd_ext_ttl_ms, 100, 10000);
	INT_KEY("frame_tap", frame_tap, 0, 1);
	INT_KEY("frame_tap_fps", frame_tap_fps, 1, 30);
	INT_KEY("frame_tap_scale", frame_tap_scale, 1, 4);
	INT_KEY("dvr_segment", dvr_segment, 10, 3600);
	INT_KEY("dvr_min_free", dvr_min_free, 10, 1000000);
	INT_KEY("dvr_overwrite", dvr_overwrite, 0, 1);
	INT_KEY("dvr_buffer_kb", dvr_buffer_kb, 256, 16384);
	INT_KEY("dvr_bitrate", dvr_bitrate_kbps, 500, 50000);
	INT_KEY("dvr_gop", dvr_gop, 1, 600);
	INT_KEY("dvr_osd", dvr_osd, 0, 1);
	INT_KEY("blackbox", blackbox, 0, 1);
	INT_KEY("blackbox_baud", blackbox_baud, 115200, 2000000);
	return -1;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

int config_load_file(rkfpv_config_t *c, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;

	char line[256];
	int lineno = 0, rc = 0;
	while (fgets(line, sizeof(line), f)) {
		lineno++;
		char *hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		char *s = trim(line);
		if (!*s)
			continue;
		char *eq = strchr(s, '=');
		if (!eq) {
			fprintf(stderr, "%s:%d: expected key=value\n", path, lineno);
			rc = -1;
			continue;
		}
		*eq = '\0';
		char *k = trim(s), *v = trim(eq + 1);
		if (config_set(c, k, v)) {
			fprintf(stderr, "%s:%d: bad setting '%s=%s'\n", path, lineno, k, v);
			rc = -1;
		}
	}
	fclose(f);
	return rc;
}

void config_print(const rkfpv_config_t *c)
{
	fprintf(stderr,
		"rkfpv: %dx%d@%d vi_chn=%d iq=%s mirror=%d flip=%d\n"
		"rkfpv: h265 %s %d kbps (max %d) gop=%d stat=%ds slice=%d ir=%d\n"
		"rkfpv: rtp -> %s:%d pkt_size=%d pt=%d preview=%d\n"
		"rkfpv: dvr=%d %s segment=%ds min_free=%dMB overwrite=%d %s buffer=%dKB\n"
		"rkfpv: dvr stream %s %dx%d %s %d kbps gop=%d\n",
		c->width, c->height, c->fps, c->vi_chn,
		c->iq_dir[0] ? c->iq_dir : "(none)", c->mirror, c->flip,
		c->rc == RC_CBR ? "cbr" : "vbr", c->bitrate_kbps,
		c->max_bitrate_kbps, c->gop, c->stat_time, c->slice_bytes,
		c->intra_refresh, c->dst_ip, c->dst_port, c->pkt_size, c->pt, c->preview_port,
		c->dvr, c->dvr_dir, c->dvr_segment, c->dvr_min_free, c->dvr_overwrite,
		c->dvr_ts ? "ts" : "mp4", c->dvr_buffer_kb,
		c->dvr_width ? "own" : "= link", c->dvr_width, c->dvr_height,
		c->dvr_rc == RC_CBR ? "cbr" : "vbr", c->dvr_bitrate_kbps, c->dvr_gop);
}
