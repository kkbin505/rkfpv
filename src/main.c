/*
 * rkfpv: camera -> AIQ -> VI -> VENC(H.265) -> RTP/UDP, for wfb-ng.
 *
 * Feed wfb_tx with "-u <port>" (UDP input) instead of majestic's
 * "-U rtp_local".
 *
 *   rkfpv [-c /etc/rkfpv.conf] [-s key=value]...
 *
 * SIGHUP re-reads the config file and applies bitrate / max_bitrate / gop /
 * rc / stat_time and the dvr_* limits live. Other keys need a restart.
 * SIGUSR1 starts DVR recording, SIGUSR2 stops it; state in /tmp/dvr.status.
 *
 * Two pipelines when a DVR partition is mounted and dvr_size is set:
 *   ISP main path (VI chn 0, e.g. 1920x1080) -> VENC 1 -> DVR (TF card)
 *   ISP self path (VI chn 1, e.g. 1280x720)  -> VENC 0 -> RTP -> wfb-ng
 * If the DVR pipeline cannot be created, the link stream is recorded.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rk_comm_venc.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

#include "config.h"
#include "dvr.h"
#include "isp.h"
#include "osd.h"
#include "osd_layout.h"
#include "telem.h"
#include "preview.h"
#include "rtp_h265.h"

#define VENC_CHN 0              /* link stream -> RTP */
#define DVR_VENC 1              /* DVR stream: own size and bitrate */
#define DVR_VI_CHN 0            /* ISP main path (full-size capable) */
#define LINK_VI_CHN_DUAL 1      /* ISP self path, when the DVR has the main path */
#define MAX_PACKS 8             /* rockit hands out one pack per frame; room for more */
#define VI_DEV 0
#define VI_PIPE 0

static volatile sig_atomic_t g_quit;
static volatile sig_atomic_t g_reload;
static volatile sig_atomic_t g_dvr_cmd;   /* 1 start, 2 stop */

extern const dvr_writer dvr_rkmuxer;
#define DVR_STATUS "/tmp/dvr.status"
#define ISP_STATUS "/tmp/isp.status"
#define IQ_STOCK "/oem/usr/share/iqfiles"

/* Where the stream loop is, for the SIGSEGV report below. */
static volatile int g_stage;          /* 1 GetStream, 2 send, 3 Release */
static volatile unsigned long g_nframes, g_last_len, g_last_off, g_last_cnt;

static void put_ulong(char *b, int *n, const char *k, unsigned long v)
{
	char d[24];
	int i = 0;
	while (*k) b[(*n)++] = *k++;
	do { d[i++] = '0' + v % 10; v /= 10; } while (v);
	while (i) b[(*n)++] = d[--i];
}

/* async-signal-safe crash report: which loop stage, how many frames */
static void on_segv(int sig, siginfo_t *si, void *uc)
{
	char b[256];
	int n = 0;
	(void)uc;
	put_ulong(b, &n, "rkfpv: CRASH sig=", (unsigned long)sig);
	put_ulong(b, &n, " addr=", (unsigned long)si->si_addr);
	put_ulong(b, &n, " stage=", (unsigned long)g_stage);
	put_ulong(b, &n, " frames=", g_nframes);
	put_ulong(b, &n, " len=", g_last_len);
	put_ulong(b, &n, " off=", g_last_off);
	put_ulong(b, &n, " packs=", g_last_cnt);
	b[n++] = '\n';
	write(2, b, n);
	_exit(128 + sig);
}

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		g_reload = 1;
	else if (sig == SIGUSR1)
		g_dvr_cmd = 1;
	else if (sig == SIGUSR2)
		g_dvr_cmd = 2;
	else
		g_quit = 1;
}

/* only record onto the DVR partition itself: an unmounted /mnt/dvr is a
 * plain directory on the root filesystem, which recording would fill */
static int is_mountpoint(const char *dir)
{
	char parent[160];
	struct stat a, b;
	snprintf(parent, sizeof parent, "%s/..", dir);
	if (stat(dir, &a) || stat(parent, &b) || !S_ISDIR(a.st_mode))
		return 0;
	return a.st_dev != b.st_dev;
}

static void write_dvr_status(dvr_t *d, const char *why_off)
{
	char line[300];
	if (d)
		dvr_status(d, line, sizeof line);
	else
		snprintf(line, sizeof line, "state=%s", why_off);
	FILE *f = fopen(DVR_STATUS ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "%s\n", line);
	fclose(f);
	rename(DVR_STATUS ".tmp", DVR_STATUS);
}

/* ---- IQ file with fallback ----------------------------------------------
 * A custom IQ directory (e.g. /userdata/iq) must never cost the video:
 *  - rkaiq fails to start with it -> start again with the stock IQ;
 *  - it crashed or hung rkfpv twice in a row (<dir>/.tries, cleared at the
 *    first encoded frame) -> use the stock IQ for this start. */
static char g_iq_used[160], g_iq_note[96];
static int g_iq_fallback;

static void iq_tries_path(const char *dir, char *out, size_t n)
{
	snprintf(out, n, "%s/.tries", dir);
}

static int iq_tries_read(const char *dir)
{
	char p[200];
	int n = 0;
	iq_tries_path(dir, p, sizeof p);
	FILE *f = fopen(p, "r");
	if (f) {
		if (fscanf(f, "%d", &n) != 1)
			n = 0;
		fclose(f);
	}
	return n;
}

static void iq_tries_write(const char *dir, int n)
{
	char p[200];
	iq_tries_path(dir, p, sizeof p);
	if (n <= 0) {
		unlink(p);
		return;
	}
	FILE *f = fopen(p, "w");
	if (f) {
		fprintf(f, "%d\n", n);
		fflush(f);
		fsync(fileno(f));
		fclose(f);
	}
}

static void write_isp_status(const char *configured)
{
	FILE *f = fopen(ISP_STATUS ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "sensor=%s\niq_dir=%s\nconfigured=%s\nfallback=%d\nnote=%s\n",
		isp_sensor_name(), g_iq_used, configured, g_iq_fallback, g_iq_note);
	fclose(f);
	rename(ISP_STATUS ".tmp", ISP_STATUS);
}

static int isp_start_safe(const rkfpv_config_t *c)
{
	int custom = strcmp(c->iq_dir, IQ_STOCK) != 0;
	snprintf(g_iq_used, sizeof g_iq_used, "%s", c->iq_dir);
	if (custom) {
		int tries = iq_tries_read(c->iq_dir);
		if (tries >= 2) {
			fprintf(stderr, "iq: custom IQ in %s failed %d starts in a row, using stock\n",
				c->iq_dir, tries);
			snprintf(g_iq_note, sizeof g_iq_note, "custom IQ failed %d starts in a row", tries);
			g_iq_fallback = 1;
			snprintf(g_iq_used, sizeof g_iq_used, "%s", IQ_STOCK);
		} else
			iq_tries_write(c->iq_dir, tries + 1);
	}
	if (isp_start(g_iq_used, c->fps, c->mirror, c->flip) == 0)
		return 0;
	if (!custom || g_iq_fallback)
		return -1;
	fprintf(stderr, "iq: custom failed, using stock\n");
	snprintf(g_iq_note, sizeof g_iq_note, "rkaiq could not start with the custom IQ");
	g_iq_fallback = 1;
	snprintf(g_iq_used, sizeof g_iq_used, "%s", IQ_STOCK);
	return isp_start(g_iq_used, c->fps, c->mirror, c->flip);
}

static uint64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* ---- VI ---------------------------------------------------------------- */

static int vi_dev_init(void)
{
	VI_DEV_ATTR_S dev;
	VI_DEV_BIND_PIPE_S bind;
	memset(&dev, 0, sizeof(dev));
	memset(&bind, 0, sizeof(bind));

	RK_S32 ret = RK_MPI_VI_GetDevAttr(VI_DEV, &dev);
	if (ret == RK_ERR_VI_NOT_CONFIG) {
		ret = RK_MPI_VI_SetDevAttr(VI_DEV, &dev);
		if (ret != RK_SUCCESS) {
			fprintf(stderr, "RK_MPI_VI_SetDevAttr: %#x\n", ret);
			return -1;
		}
	}
	if (RK_MPI_VI_GetDevIsEnable(VI_DEV) != RK_SUCCESS) {
		ret = RK_MPI_VI_EnableDev(VI_DEV);
		if (ret != RK_SUCCESS) {
			fprintf(stderr, "RK_MPI_VI_EnableDev: %#x\n", ret);
			return -1;
		}
		bind.u32Num = 1;
		bind.PipeId[0] = VI_PIPE;
		ret = RK_MPI_VI_SetDevBindPipe(VI_DEV, &bind);
		if (ret != RK_SUCCESS) {
			fprintf(stderr, "RK_MPI_VI_SetDevBindPipe: %#x\n", ret);
			return -1;
		}
	}
	return 0;
}

static int vi_chn_init(int chn, int width, int height)
{
	VI_CHN_ATTR_S a;
	memset(&a, 0, sizeof(a));
	a.stIspOpt.u32BufCount = 2;
	a.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
	a.stIspOpt.stMaxSize.u32Width = width;
	a.stIspOpt.stMaxSize.u32Height = height;
	a.stSize.u32Width = width;
	a.stSize.u32Height = height;
	a.enPixelFormat = RK_FMT_YUV420SP;
	a.enCompressMode = COMPRESS_MODE_NONE;
	a.u32Depth = 0;            /* bound to VENC, we never pull frames */
	a.stFrameRate.s32SrcFrameRate = -1;
	a.stFrameRate.s32DstFrameRate = -1;

	RK_S32 ret = RK_MPI_VI_SetChnAttr(VI_PIPE, chn, &a);
	if (ret == RK_SUCCESS)
		ret = RK_MPI_VI_EnableChn(VI_PIPE, chn);
	if (ret != RK_SUCCESS) {
		fprintf(stderr, "VI channel %d (%dx%d): %#x\n", chn, width, height, ret);
		return -1;
	}
	return 0;
}

/* ---- VENC -------------------------------------------------------------- */

typedef struct {
	int width, height, fps;
	rc_mode_t rc;
	int bitrate_kbps, max_bitrate_kbps, gop, stat_time, slice_bytes, intra_refresh;
} enc_params_t;

static enc_params_t link_params(const rkfpv_config_t *c)
{
	enc_params_t e = { c->width, c->height, c->fps, c->rc, c->bitrate_kbps,
			   c->max_bitrate_kbps, c->gop, c->stat_time, c->slice_bytes,
			   c->intra_refresh };
	return e;
}

/* the recording: VBR by default, 1 s GOP so a fragmented MP4 never holds
 * more than about a second of video in RAM */
static enc_params_t dvr_params(const rkfpv_config_t *c)
{
	enc_params_t e = { c->dvr_width, c->dvr_height, c->fps, c->dvr_rc, c->dvr_bitrate_kbps,
			   0, c->dvr_gop, 3, 0, 0 };
	return e;
}

static void fill_rc(VENC_RC_ATTR_S *rc, const enc_params_t *c)
{
	memset(rc, 0, sizeof(*rc));
	if (c->rc == RC_VBR) {
		rc->enRcMode = VENC_RC_MODE_H265VBR;
		VENC_H265_VBR_S *v = &rc->stH265Vbr;
		v->u32Gop = c->gop;
		v->u32SrcFrameRateNum = c->fps;
		v->u32SrcFrameRateDen = 1;
		v->fr32DstFrameRateNum = c->fps;
		v->fr32DstFrameRateDen = 1;
		v->u32BitRate = c->bitrate_kbps;
		v->u32MaxBitRate = c->max_bitrate_kbps ? c->max_bitrate_kbps
						       : c->bitrate_kbps * 3 / 2;
		v->u32MinBitRate = c->bitrate_kbps / 2;
		v->u32StatTime = c->stat_time;
	} else {
		rc->enRcMode = VENC_RC_MODE_H265CBR;
		VENC_H265_CBR_S *v = &rc->stH265Cbr;
		v->u32Gop = c->gop;
		v->u32SrcFrameRateNum = c->fps;
		v->u32SrcFrameRateDen = 1;
		v->fr32DstFrameRateNum = c->fps;
		v->fr32DstFrameRateDen = 1;
		v->u32BitRate = c->bitrate_kbps;
		v->u32StatTime = c->stat_time;
	}
}

static int venc_init(int chn, const enc_params_t *c)
{
	VENC_CHN_ATTR_S a;
	memset(&a, 0, sizeof(a));

	a.stVencAttr.enType = RK_VIDEO_ID_HEVC;
	a.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	a.stVencAttr.u32Profile = H265E_PROFILE_MAIN;
	a.stVencAttr.u32PicWidth = c->width;
	a.stVencAttr.u32PicHeight = c->height;
	a.stVencAttr.u32VirWidth = c->width;
	a.stVencAttr.u32VirHeight = c->height;
	a.stVencAttr.u32StreamBufCnt = 2;
	a.stVencAttr.u32BufSize = c->width * c->height / 2;
	a.stVencAttr.enMirror = MIRROR_NONE;
	fill_rc(&a.stRcAttr, c);
	a.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	a.stGopAttr.u32MaxLtrCount = 0;

	RK_S32 ret = RK_MPI_VENC_CreateChn(chn, &a);
	if (ret != RK_SUCCESS) {
		fprintf(stderr, "RK_MPI_VENC_CreateChn(%d, %dx%d): %#x\n", chn, c->width, c->height, ret);
		return -1;
	}

	/* reference frames share one buffer (as rkipc does): less CMA */
	VENC_CHN_REF_BUF_SHARE_S share;
	memset(&share, 0, sizeof(share));
	share.bEnable = RK_TRUE;
	RK_MPI_VENC_SetChnRefBufShareAttr(chn, &share);

	if (c->slice_bytes > 0) {
		VENC_SLICE_SPLIT_S s;
		memset(&s, 0, sizeof(s));
		s.bSplitEnable = RK_TRUE;
		s.u32SplitMode = 0;               /* by byte count */
		s.u32SplitSize = c->slice_bytes;
		ret = RK_MPI_VENC_SetSliceSplit(chn, &s);
		if (ret != RK_SUCCESS)
			fprintf(stderr, "warning: slice split: %#x\n", ret);
	}

	if (c->intra_refresh > 0) {
		VENC_INTRA_REFRESH_S ir;
		memset(&ir, 0, sizeof(ir));
		ir.bRefreshEnable = RK_TRUE;
		ir.enIntraRefreshMode = INTRA_REFRESH_ROW;
		ir.u32RefreshNum = c->intra_refresh;
		ir.u32ReqIQp = 30;
		ret = RK_MPI_VENC_SetIntraRefresh(chn, &ir);
		if (ret != RK_SUCCESS)
			fprintf(stderr, "warning: intra refresh: %#x\n", ret);
	}

	VENC_RECV_PIC_PARAM_S r;
	memset(&r, 0, sizeof(r));
	r.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(chn, &r);
	if (ret != RK_SUCCESS) {
		fprintf(stderr, "RK_MPI_VENC_StartRecvFrame: %#x\n", ret);
		return -1;
	}
	return 0;
}

/* Applies the rate-control part of a new config without restarting. */
static void venc_apply_rc(int chn, const enc_params_t *c)
{
	VENC_CHN_ATTR_S a;
	if (RK_MPI_VENC_GetChnAttr(chn, &a) != RK_SUCCESS) {
		fprintf(stderr, "reload: GetChnAttr failed\n");
		return;
	}
	fill_rc(&a.stRcAttr, c);
	RK_S32 ret = RK_MPI_VENC_SetChnAttr(chn, &a);
	if (ret != RK_SUCCESS) {
		fprintf(stderr, "reload: SetChnAttr(%d): %#x\n", chn, ret);
		return;
	}
	RK_MPI_VENC_RequestIDR(chn, RK_TRUE);
	fprintf(stderr, "reload: %s stream %s %d kbps gop=%d applied\n",
		chn == VENC_CHN ? "link" : "dvr", c->rc == RC_CBR ? "cbr" : "vbr",
		c->bitrate_kbps, c->gop);
}

/* ---- OSD (experiment) --------------------------------------------------- */

#define OSD_LINK_HANDLE 0
#define OSD_DVR_HANDLE 1

#define TELEM_PORT 14560                /* mavbridge -c copies the FC stream here */
#define UPLINK_STATUS "/tmp/wfb_uplink.status"
#define OSD_PERIOD_US 250000            /* 4 redraws per second */
#define SOC_TEMP "/sys/class/thermal/thermal_zone0/temp"   /* RV1103 tsadc, m°C */

/* SoC temperature in whole degrees C; 0 if there is no sensor. Kept open
 * and re-read with pread: one syscall, no process, once per second. */
static int read_soc_temp(int *c)
{
	static int fd = -2;
	if (fd == -2)
		fd = open(SOC_TEMP, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	char b[16];
	ssize_t n = pread(fd, b, sizeof b - 1, 0);
	if (n <= 0)
		return 0;
	b[n] = 0;
	long mc = strtol(b, NULL, 10);
	*c = (int)((mc + (mc < 0 ? -500 : 500)) / 1000);
	return mc > -40000 && mc < 150000;
}

/* air-side wfb_rx (uplink from the ground station), see wfb.sh */
static int read_uplink(int *rssi, int *lq)
{
	struct stat st;
	if (stat(UPLINK_STATUS, &st) || time(NULL) - st.st_mtime > 3)
		return 0;
	FILE *f = fopen(UPLINK_STATUS, "r");
	if (!f)
		return 0;
	char r[16] = "";
	int uniq = 0, lost = 0;
	int n = fscanf(f, "rssi=%15s uniq=%d lost=%d", r, &uniq, &lost);
	fclose(f);
	if (n != 3 || uniq + lost == 0 || !strcmp(r, "none"))
		return 0;
	*rssi = atoi(r);
	*lq = uniq * 100 / (uniq + lost);
	return 1;
}

/* one OSD redraw, timed */
static uint64_t draw_osd(osd_t *o, const telem_t *t, const char *rec, uint64_t now_ms,
			 int temp_ok, int temp_c, uint64_t (*clock_us)(void))
{
	if (!o)
		return 0;
	uint64_t t0 = clock_us();
	osd_view_t v = { t, rec, 0, 0, 0, now_ms, temp_ok, temp_c };
	v.ul_ok = read_uplink(&v.ul_rssi, &v.ul_lq);
	osd_draw_view(o, &v);
	return clock_us() - t0;
}

/* ---- DVR stream capture (dual mode) ------------------------------------ */

static volatile int g_cap_quit;
static volatile unsigned long long g_dvr_frames, g_dvr_bytes;

struct dvr_cap {
	dvr_t *dvr;
	size_t max_len;
};

/* VENC 1 -> dvr_push, in its own thread so the link loop never waits on it.
 * The encoder runs whether or not we record: pausing a bound encoder can
 * hold up the shared ISP and with it the link stream. */
static void *dvr_capture(void *arg)
{
	struct dvr_cap *c = arg;
	VENC_STREAM_S st;
	memset(&st, 0, sizeof(st));
	st.pstPack = calloc(MAX_PACKS, sizeof(VENC_PACK_S));
	if (!st.pstPack)
		return NULL;
	while (!g_quit && !g_cap_quit) {
		if (RK_MPI_VENC_GetStream(DVR_VENC, &st, 200) != RK_SUCCESS)
			continue;
		RK_U32 npack = st.u32PackCount < MAX_PACKS ? st.u32PackCount : MAX_PACKS;
		for (RK_U32 i = 0; i < npack; i++) {
			VENC_PACK_S *p = &st.pstPack[i];
			uint8_t *data = (uint8_t *)RK_MPI_MB_Handle2VirAddr(p->pMbBlk);
			size_t len = p->u32Len;
			if (!data || len == 0 || len > c->max_len)
				continue;
			dvr_push(c->dvr, data, len, p->u64PTS, p->bFrameEnd);
			g_dvr_bytes += len;
			if (p->bFrameEnd)
				g_dvr_frames++;
		}
		RK_MPI_VENC_ReleaseStream(DVR_VENC, &st);
		if (dvr_want_idr(c->dvr))
			RK_MPI_VENC_RequestIDR(DVR_VENC, RK_TRUE);
	}
	free(st.pstPack);
	return NULL;
}

/* ---- main -------------------------------------------------------------- */

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-c config] [-s key=value]...\n"
		"keys: size fps vi_chn iq_dir mirror flip rc(cbr|vbr) bitrate max_bitrate\n"
		"      gop stat_time slice_bytes intra_refresh dst port pkt_size stats\n"
		"defaults: size=1280x720 fps=30 rc=cbr bitrate=3500 gop=30 dst=127.0.0.1\n"
		"          port=5600 pkt_size=1400\n", p);
}

#define MAX_OVERRIDES 32

static int load_config(rkfpv_config_t *c, const char *path,
		       char **ov, int nov)
{
	config_defaults(c);
	int rc = config_load_file(c, path);
	for (int i = 0; i < nov; i++) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s", ov[i]);
		char *eq = strchr(buf, '=');
		if (!eq || (*eq = '\0', config_set(c, buf, eq + 1))) {
			fprintf(stderr, "bad -s '%s'\n", ov[i]);
			rc = -1;
		}
	}
	return rc;
}

int main(int argc, char **argv)
{
	const char *cfg_path = "/etc/rkfpv.conf";
	char *ov[MAX_OVERRIDES];
	int nov = 0, opt;

	while ((opt = getopt(argc, argv, "c:s:h")) != -1) {
		switch (opt) {
		case 'c': cfg_path = optarg; break;
		case 's':
			if (nov < MAX_OVERRIDES)
				ov[nov++] = optarg;
			break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}

	rkfpv_config_t cfg;
	if (load_config(&cfg, cfg_path, ov, nov))
		return 1;
	config_print(&cfg);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sv;
	memset(&sv, 0, sizeof(sv));
	sv.sa_sigaction = on_segv;
	sv.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &sv, NULL);
	sigaction(SIGBUS, &sv, NULL);

	rtp_h265_t rtp;
	if (rtp_h265_open(&rtp, cfg.dst_ip, cfg.dst_port, cfg.pkt_size)) {
		fprintf(stderr, "cannot open UDP socket to %s:%d\n", cfg.dst_ip, cfg.dst_port);
		return 1;
	}
	rtp.pt = (uint8_t)cfg.pt;
	/* only on loopback: reached over USB with adb forward, never over the air */
	preview_t pv;
	preview_open(&pv, "127.0.0.1", cfg.preview_port);

	int ret = 1, isp_up = 0, vi_up = 0, venc_up = 0, bound = 0;
	osd_t *osd_link = NULL, *osd_dvr = NULL;
	int telem_fd = -1;
	int dvr_vi_up = 0, dvr_venc_up = 0, dvr_bound = 0, dual = 0, cap_started = 0;
	pthread_t cap_thr;
	struct dvr_cap cap;
	dvr_t *dvr = NULL;
	const char *dvr_off = "off";
	/* dvr = 0: no DVR at all, nothing allocated (no second encoder, no
	 * ring buffer, no muxer). Memory on the 64 MB board is too tight to
	 * keep it around "just in case". */
	int dvr_card = cfg.dvr ? is_mountpoint(cfg.dvr_dir) : 0;
	enc_params_t lp = link_params(&cfg), dp = dvr_params(&cfg);
	MPP_CHN_S src = { .enModId = RK_ID_VI, .s32DevId = VI_DEV, .s32ChnId = cfg.vi_chn };
	MPP_CHN_S dst = { .enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = VENC_CHN };
	MPP_CHN_S dsrc = { .enModId = RK_ID_VI, .s32DevId = VI_DEV, .s32ChnId = DVR_VI_CHN };
	MPP_CHN_S ddst = { .enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = DVR_VENC };

	if (cfg.iq_dir[0]) {
		int isp_rc = isp_start_safe(&cfg);
		write_isp_status(cfg.iq_dir);
		if (isp_rc)
			goto out;
		isp_up = 1;
		isp_apply(&cfg.isp);
	} else {
		fprintf(stderr, "warning: iq_dir empty, AIQ not started (colors will be off)\n");
	}

	if (RK_MPI_SYS_Init() != RK_SUCCESS) {
		fprintf(stderr, "RK_MPI_SYS_Init failed\n");
		goto out;
	}
	if (vi_dev_init())
		goto out;

	/* DVR pipeline first: it takes the ISP main path. Any failure here
	 * (e.g. not enough CMA) falls back to recording the link stream. */
	if (dvr_card && cfg.dvr_width > 0) {
		if (vi_chn_init(DVR_VI_CHN, dp.width, dp.height) == 0) {
			dvr_vi_up = 1;
			if (venc_init(DVR_VENC, &dp) == 0) {
				dvr_venc_up = 1;
				if (RK_MPI_SYS_Bind(&dsrc, &ddst) == RK_SUCCESS) {
					dvr_bound = 1;
					dual = 1;
				}
			}
		}
		if (!dual) {
			fprintf(stderr, "dvr: %dx%d stream failed, recording the link stream instead\n",
				dp.width, dp.height);
			if (dvr_venc_up) {
				RK_MPI_VENC_StopRecvFrame(DVR_VENC);
				RK_MPI_VENC_DestroyChn(DVR_VENC);
				dvr_venc_up = 0;
			}
			if (dvr_vi_up) {
				RK_MPI_VI_DisableChn(VI_PIPE, DVR_VI_CHN);
				dvr_vi_up = 0;
			}
		} else
			fprintf(stderr, "dvr: own stream %dx%d %s %d kbps (VI %d -> VENC %d)\n",
				dp.width, dp.height, dp.rc == RC_CBR ? "cbr" : "vbr",
				dp.bitrate_kbps, DVR_VI_CHN, DVR_VENC);
	}

	/* link stream: ISP self path when the DVR has the main path */
	int link_vi = dual ? LINK_VI_CHN_DUAL : cfg.vi_chn;
	src.s32ChnId = link_vi;
	if (vi_chn_init(link_vi, lp.width, lp.height))
		goto out;
	vi_up = 1;
	if (venc_init(VENC_CHN, &lp))
		goto out;
	venc_up = 1;
	if (RK_MPI_SYS_Bind(&src, &dst) != RK_SUCCESS) {
		fprintf(stderr, "bind VI -> VENC failed\n");
		goto out;
	}
	bound = 1;

	VENC_STREAM_S st;
	memset(&st, 0, sizeof(st));
		st.pstPack = calloc(MAX_PACKS, sizeof(VENC_PACK_S));
	if (!st.pstPack)
		goto out;

	if (!cfg.dvr) {
		dvr_off = "disabled";
		fprintf(stderr, "dvr: disabled (dvr = 0)\n");
	} else if (!dvr_card) {
		dvr_off = "no-card";
		fprintf(stderr, "dvr: %s is not a mounted partition, recording disabled\n", cfg.dvr_dir);
	} else {
		dvr_config dc;
		memset(&dc, 0, sizeof dc);
		snprintf(dc.dir, sizeof dc.dir, "%s", cfg.dvr_dir);
		dc.segment_s = cfg.dvr_segment;
		dc.min_free_mb = cfg.dvr_min_free;
		dc.overwrite = cfg.dvr_overwrite;
		dc.ts = cfg.dvr_ts;
		dc.ring_bytes = (size_t)cfg.dvr_buffer_kb * 1024;
		const enc_params_t *rec = dual ? &dp : &lp;
		dc.stream = (dvr_stream_info){ rec->width, rec->height, rec->fps, rec->bitrate_kbps, cfg.dvr_ts };
		dvr = dvr_create(&dc, &dvr_rkmuxer, 1);
		if (!dvr)
			fprintf(stderr, "dvr: cannot start (out of memory?)\n");
		else {
			if (cfg.dvr)
				dvr_start(dvr);
			if (dual) {
				cap.dvr = dvr;
				cap.max_len = (size_t)dp.width * dp.height * 2;
				if (pthread_create(&cap_thr, NULL, dvr_capture, &cap) == 0)
					cap_started = 1;
				else
					fprintf(stderr, "dvr: cannot start the capture thread\n");
			}
		}
	}
	write_dvr_status(dvr, dvr_off);
	uint64_t t_dvr_status = now_us();

	if (cfg.osd) {
		osd_link = osd_create(OSD_LINK_HANDLE, VENC_CHN, cfg.width, cfg.height);
		if (dual && cfg.dvr_osd)
			osd_dvr = osd_create(OSD_DVR_HANDLE, DVR_VENC, dp.width, dp.height);
		else if (dual)
			fprintf(stderr, "osd: not in the recording (dvr_osd = 0)\n");
		else if (!cfg.dvr_osd && dvr)
			fprintf(stderr, "osd: dvr_osd = 0 needs the recording's own encoder (dvr_size not link); "
				"the recording is the radio stream, with its OSD\n");
	}
	uint64_t t_osd = now_us(), osd_worst = 0, temp_ms = 0;
	int temp_ok = 0, temp_c = 0;
	telem_t telem;
	telem_init(&telem);
	telem_fd = (osd_link || osd_dvr) ? telem_open(TELEM_PORT) : -1;
	if ((osd_link || osd_dvr) && telem_fd < 0)
		fprintf(stderr, "osd: cannot listen on udp %d for telemetry\n", TELEM_PORT);

	uint64_t t0 = now_us(), frames = 0, bytes = 0, max_frame = 0, cur_frame = 0;
	uint64_t pkts0 = 0, t_start = t0, dvr_f0 = 0, dvr_b0 = 0;
	int got_first = 0;
	fprintf(stderr, "rkfpv: pipeline up, waiting for frames\n");

	while (!g_quit) {
		if (g_reload) {
			g_reload = 0;
			rkfpv_config_t n;
			if (load_config(&n, cfg_path, ov, nov) == 0) {
				if (n.width != cfg.width || n.height != cfg.height ||
				    n.fps != cfg.fps || n.dst_port != cfg.dst_port ||
				    strcmp(n.dst_ip, cfg.dst_ip) || n.pkt_size != cfg.pkt_size)
					fprintf(stderr, "reload: size/fps/dst/pkt_size need a restart\n");
				lp = link_params(&n);
				lp.width = cfg.width;   /* size changes need a restart */
				lp.height = cfg.height;
				venc_apply_rc(VENC_CHN, &lp);
				if (dual && (n.dvr_bitrate_kbps != cfg.dvr_bitrate_kbps ||
					     n.dvr_gop != cfg.dvr_gop || n.dvr_rc != cfg.dvr_rc)) {
					cfg.dvr_bitrate_kbps = n.dvr_bitrate_kbps;
					cfg.dvr_gop = n.dvr_gop;
					cfg.dvr_rc = n.dvr_rc;
					dp = dvr_params(&cfg);
					venc_apply_rc(DVR_VENC, &dp);
				}
				if (n.iq_dir[0] && strcmp(n.iq_dir, cfg.iq_dir))
					fprintf(stderr, "reload: iq_dir needs a restart\n");
				isp_apply(&n.isp);
				cfg.isp = n.isp;
				cfg.rc = n.rc;
				cfg.bitrate_kbps = n.bitrate_kbps;
				cfg.max_bitrate_kbps = n.max_bitrate_kbps;
				cfg.gop = n.gop;
				cfg.stat_time = n.stat_time;
				dvr_reconfig(dvr, n.dvr_segment, n.dvr_min_free, n.dvr_overwrite, n.dvr_ts);
			}
		}
		if (g_dvr_cmd) {
			int c = g_dvr_cmd;
			g_dvr_cmd = 0;
			if (!dvr)
				fprintf(stderr, "dvr: %s requested, but DVR is %s\n", c == 1 ? "start" : "stop", dvr_off);
			else if (c == 1)
				dvr_start(dvr);
			else
				dvr_stop(dvr);
		}

		preview_poll(&pv);
		if (preview_take_idr_request(&pv))
			RK_MPI_VENC_RequestIDR(VENC_CHN, RK_TRUE);

		g_stage = 1;
		RK_S32 r = RK_MPI_VENC_GetStream(VENC_CHN, &st, 200);
		if (r != RK_SUCCESS)
			continue;   /* timeout or transient error; signals land here too */

		RK_U32 npack = st.u32PackCount;
		if (npack > MAX_PACKS) {
			fprintf(stderr, "rkfpv: %u packs in one frame, using %d\n", npack, MAX_PACKS);
			npack = MAX_PACKS;
		}
		g_last_cnt = st.u32PackCount;
		g_stage = 2;
		for (RK_U32 i = 0; i < npack; i++) {
			VENC_PACK_S *p = &st.pstPack[i];
			/* As the Rockchip samples and rkipc read it: the MB's virtual
			 * address is the start of the stream, u32Len its length.
			 * u32Offset is not used there and is not reliable. */
			uint8_t *data = (uint8_t *)RK_MPI_MB_Handle2VirAddr(p->pMbBlk);
			size_t len = p->u32Len;
			uint32_t ts = (uint32_t)(p->u64PTS * 9 / 100);   /* us -> 90 kHz */

			g_last_len = len;
			g_last_off = p->u32Offset;
			if (!data || len == 0 || len > (size_t)cfg.width * cfg.height * 2) {
				fprintf(stderr, "rkfpv: bad pack %u: data=%p len=%zu off=%u, skipped\n",
					i, (void *)data, len, p->u32Offset);
				continue;
			}

			rtp_h265_send(&rtp, data, len, ts, p->bFrameEnd);
			preview_push(&pv, data, len, p->bFrameEnd);
			if (!dual)
				dvr_push(dvr, data, len, p->u64PTS, p->bFrameEnd);

			bytes += len;
			cur_frame += len;
			if (p->bFrameEnd) {
				frames++;
				g_nframes++;
				if (cur_frame > max_frame)
					max_frame = cur_frame;
				cur_frame = 0;
			}
		}
		g_stage = 3;
		RK_MPI_VENC_ReleaseStream(VENC_CHN, &st);

		if (!dual && dvr_want_idr(dvr))
			RK_MPI_VENC_RequestIDR(VENC_CHN, RK_TRUE);
		if (now_us() - t_dvr_status >= 1000000u) {
			write_dvr_status(dvr, dvr_off);
			t_dvr_status = now_us();
		}

		if (!got_first) {
			got_first = 1;
			/* the custom IQ got the pipeline running: not a bad file */
			if (!g_iq_fallback && strcmp(cfg.iq_dir, IQ_STOCK))
				iq_tries_write(cfg.iq_dir, 0);
			fprintf(stderr, "rkfpv: first frame after %llu ms (%llu B)\n",
				(unsigned long long)((now_us() - t_start) / 1000),
				(unsigned long long)bytes);
		}

		if (telem_fd >= 0)
			telem_poll(&telem, telem_fd, now_us() / 1000);
		if ((osd_link || osd_dvr) && now_us() - t_osd >= OSD_PERIOD_US) {
			char ds[300] = "", rec[16] = "";
			if (dvr) {
				dvr_status(dvr, ds, sizeof ds);
				if (strstr(ds, "state=recording"))
					snprintf(rec, sizeof rec, "REC");
				else if (strstr(ds, "state=full"))
					snprintf(rec, sizeof rec, "CARD FULL");
			}
			uint64_t now_ms = now_us() / 1000;
			if (now_ms - temp_ms >= 1000) {
				temp_ok = read_soc_temp(&temp_c);
				temp_ms = now_ms;
			}
			uint64_t d1 = draw_osd(osd_link, &telem, rec, now_ms, temp_ok, temp_c, now_us);
			uint64_t d2 = draw_osd(osd_dvr, &telem, rec, now_ms, temp_ok, temp_c, now_us);
			if (d1 + d2 > osd_worst) {
				osd_worst = d1 + d2;
				fprintf(stderr, "osd: redraw took %llu us (link %llu, dvr %llu) - slowest so far\n",
					(unsigned long long)(d1 + d2), (unsigned long long)d1,
					(unsigned long long)d2);
			}
			t_osd = now_us();
		}

		uint64_t t = now_us();
		if (cfg.stats_interval && t - t0 >= (uint64_t)cfg.stats_interval * 1000000u) {
			double s = (t - t0) / 1e6;
			char ds[300];
			if (dvr)
				dvr_status(dvr, ds, sizeof ds);
			else
				snprintf(ds, sizeof ds, "state=%s", dvr_off);
			char dr[64] = "";
			if (dual) {
				unsigned long long df = g_dvr_frames, db = g_dvr_bytes;
				snprintf(dr, sizeof dr, " %.1f fps %.0f kbps", (df - dvr_f0) / s,
					 (db - dvr_b0) * 8 / s / 1000);
				dvr_f0 = df;
				dvr_b0 = db;
			}
			fprintf(stderr, "stats: %.1f fps, %.0f kbps, max frame %llu B, "
				"%.0f pkt/s, send errors %llu | preview %s | dvr%s %s\n",
				frames / s, bytes * 8 / s / 1000, (unsigned long long)max_frame,
				(rtp.pkts_sent - pkts0) / s,
				(unsigned long long)rtp.send_errors,
				pv.cfd >= 0 ? "on" : "off", dr, ds);
			t0 = t;
			frames = bytes = max_frame = 0;
			pkts0 = rtp.pkts_sent;
		}
	}
	free(st.pstPack);
	ret = 0;
	fprintf(stderr, "rkfpv: stopping (%s)\n", g_quit ? "signal" : "error");

out:
	/* each step is logged so a crash during teardown shows where it was */
	if (telem_fd >= 0)
		close(telem_fd);
	osd_destroy(osd_link);
	osd_destroy(osd_dvr);
	if (cap_started) {
		g_cap_quit = 1;
		pthread_join(cap_thr, NULL);
	}
	if (dvr) {
		fprintf(stderr, "rkfpv: dvr close\n");
		dvr_destroy(dvr);       /* finishes the current file */
		write_dvr_status(NULL, "off");
	}
	if (bound) {
		fprintf(stderr, "rkfpv: unbind\n");
		RK_MPI_SYS_UnBind(&src, &dst);
	}
	if (dvr_bound)
		RK_MPI_SYS_UnBind(&dsrc, &ddst);
	if (vi_up) {
		fprintf(stderr, "rkfpv: vi chn off\n");
		RK_MPI_VI_DisableChn(VI_PIPE, src.s32ChnId);
	}
	if (dvr_vi_up)
		RK_MPI_VI_DisableChn(VI_PIPE, DVR_VI_CHN);
	if (venc_up) {
		fprintf(stderr, "rkfpv: venc off\n");
		RK_MPI_VENC_StopRecvFrame(VENC_CHN);
		RK_MPI_VENC_DestroyChn(VENC_CHN);
	}
	if (dvr_venc_up) {
		RK_MPI_VENC_StopRecvFrame(DVR_VENC);
		RK_MPI_VENC_DestroyChn(DVR_VENC);
	}
	if (vi_up) {
		fprintf(stderr, "rkfpv: vi dev off\n");
		RK_MPI_VI_DisableDev(VI_DEV);
	}
	if (isp_up) {
		fprintf(stderr, "rkfpv: isp stop\n");
		isp_stop();
	}
	fprintf(stderr, "rkfpv: sys exit\n");
	RK_MPI_SYS_Exit();
	preview_close(&pv);
	rtp_h265_close(&rtp);
	fprintf(stderr, "rkfpv: done\n");
	return ret;
}
