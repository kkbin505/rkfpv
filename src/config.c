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
	c->pkt_size = 1400;

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
	INT_KEY("stats", stats_interval, 0, 3600);
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
		"rkfpv: rtp -> %s:%d pkt_size=%d\n",
		c->width, c->height, c->fps, c->vi_chn,
		c->iq_dir[0] ? c->iq_dir : "(none)", c->mirror, c->flip,
		c->rc == RC_CBR ? "cbr" : "vbr", c->bitrate_kbps,
		c->max_bitrate_kbps, c->gop, c->stat_time, c->slice_bytes,
		c->intra_refresh, c->dst_ip, c->dst_port, c->pkt_size);
}
