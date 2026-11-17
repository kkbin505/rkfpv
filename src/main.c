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
 */
#include <errno.h>
#include <getopt.h>
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
#include "rtp_h265.h"

#define VENC_CHN 0
#define VI_DEV 0
#define VI_PIPE 0

static volatile sig_atomic_t g_quit;
static volatile sig_atomic_t g_reload;
static volatile sig_atomic_t g_dvr_cmd;   /* 1 start, 2 stop */

extern const dvr_writer dvr_rkmuxer;
#define DVR_STATUS "/tmp/dvr.status"

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

static int vi_chn_init(const rkfpv_config_t *c)
{
	VI_CHN_ATTR_S a;
	memset(&a, 0, sizeof(a));
	a.stIspOpt.u32BufCount = 2;
	a.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
	a.stSize.u32Width = c->width;
	a.stSize.u32Height = c->height;
	a.enPixelFormat = RK_FMT_YUV420SP;
	a.enCompressMode = COMPRESS_MODE_NONE;
	a.u32Depth = 0;            /* bound to VENC, we never pull frames */
	a.stFrameRate.s32SrcFrameRate = -1;
	a.stFrameRate.s32DstFrameRate = -1;

	RK_S32 ret = RK_MPI_VI_SetChnAttr(VI_PIPE, c->vi_chn, &a);
	if (ret == RK_SUCCESS)
		ret = RK_MPI_VI_EnableChn(VI_PIPE, c->vi_chn);
	if (ret != RK_SUCCESS) {
		fprintf(stderr, "VI channel %d: %#x\n", c->vi_chn, ret);
		return -1;
	}
	return 0;
}

/* ---- VENC -------------------------------------------------------------- */

static void fill_rc(VENC_RC_ATTR_S *rc, const rkfpv_config_t *c)
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

static int venc_init(const rkfpv_config_t *c)
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

	RK_S32 ret = RK_MPI_VENC_CreateChn(VENC_CHN, &a);
	if (ret != RK_SUCCESS) {
		fprintf(stderr, "RK_MPI_VENC_CreateChn: %#x\n", ret);
		return -1;
	}

	if (c->slice_bytes > 0) {
		VENC_SLICE_SPLIT_S s;
		memset(&s, 0, sizeof(s));
		s.bSplitEnable = RK_TRUE;
		s.u32SplitMode = 0;               /* by byte count */
		s.u32SplitSize = c->slice_bytes;
		ret = RK_MPI_VENC_SetSliceSplit(VENC_CHN, &s);
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
		ret = RK_MPI_VENC_SetIntraRefresh(VENC_CHN, &ir);
		if (ret != RK_SUCCESS)
			fprintf(stderr, "warning: intra refresh: %#x\n", ret);
	}

	VENC_RECV_PIC_PARAM_S r;
	memset(&r, 0, sizeof(r));
	r.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN, &r);
	if (ret != RK_SUCCESS) {
		fprintf(stderr, "RK_MPI_VENC_StartRecvFrame: %#x\n", ret);
		return -1;
	}
	return 0;
}

/* Applies the rate-control part of a new config without restarting. */
static void venc_apply_rc(const rkfpv_config_t *c)
{
	VENC_CHN_ATTR_S a;
	if (RK_MPI_VENC_GetChnAttr(VENC_CHN, &a) != RK_SUCCESS) {
		fprintf(stderr, "reload: GetChnAttr failed\n");
		return;
	}
	fill_rc(&a.stRcAttr, c);
	RK_S32 ret = RK_MPI_VENC_SetChnAttr(VENC_CHN, &a);
	if (ret != RK_SUCCESS) {
		fprintf(stderr, "reload: SetChnAttr: %#x\n", ret);
		return;
	}
	RK_MPI_VENC_RequestIDR(VENC_CHN, RK_TRUE);
	fprintf(stderr, "reload: %s %d kbps gop=%d applied\n",
		c->rc == RC_CBR ? "cbr" : "vbr", c->bitrate_kbps, c->gop);
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

	int ret = 1, isp_up = 0, vi_up = 0, venc_up = 0, bound = 0;
	dvr_t *dvr = NULL;
	const char *dvr_off = "off";
	MPP_CHN_S src = { .enModId = RK_ID_VI, .s32DevId = VI_DEV, .s32ChnId = cfg.vi_chn };
	MPP_CHN_S dst = { .enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = VENC_CHN };

	if (cfg.iq_dir[0]) {
		if (isp_start(cfg.iq_dir, cfg.fps, cfg.mirror, cfg.flip))
			goto out;
		isp_up = 1;
	} else {
		fprintf(stderr, "warning: iq_dir empty, AIQ not started (colors will be off)\n");
	}

	if (RK_MPI_SYS_Init() != RK_SUCCESS) {
		fprintf(stderr, "RK_MPI_SYS_Init failed\n");
		goto out;
	}
	if (vi_dev_init() || vi_chn_init(&cfg))
		goto out;
	vi_up = 1;
	if (venc_init(&cfg))
		goto out;
	venc_up = 1;
	if (RK_MPI_SYS_Bind(&src, &dst) != RK_SUCCESS) {
		fprintf(stderr, "bind VI -> VENC failed\n");
		goto out;
	}
	bound = 1;

	VENC_STREAM_S st;
	memset(&st, 0, sizeof(st));
	/* room for several packs even though rockit hands out one per frame */
#define MAX_PACKS 8
	st.pstPack = calloc(MAX_PACKS, sizeof(VENC_PACK_S));
	if (!st.pstPack)
		goto out;

	if (!is_mountpoint(cfg.dvr_dir)) {
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
		dc.stream = (dvr_stream_info){ cfg.width, cfg.height, cfg.fps, cfg.bitrate_kbps, cfg.dvr_ts };
		dvr = dvr_create(&dc, &dvr_rkmuxer, 1);
		if (!dvr)
			fprintf(stderr, "dvr: cannot start (out of memory?)\n");
		else if (cfg.dvr)
			dvr_start(dvr);
	}
	write_dvr_status(dvr, dvr_off);
	uint64_t t_dvr_status = now_us();

	uint64_t t0 = now_us(), frames = 0, bytes = 0, max_frame = 0, cur_frame = 0;
	uint64_t pkts0 = 0, t_start = t0;
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
				venc_apply_rc(&n);
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

		if (dvr_want_idr(dvr))
			RK_MPI_VENC_RequestIDR(VENC_CHN, RK_TRUE);
		if (now_us() - t_dvr_status >= 1000000u) {
			write_dvr_status(dvr, dvr_off);
			t_dvr_status = now_us();
		}

		if (!got_first) {
			got_first = 1;
			fprintf(stderr, "rkfpv: first frame after %llu ms (%llu B)\n",
				(unsigned long long)((now_us() - t_start) / 1000),
				(unsigned long long)bytes);
		}

		uint64_t t = now_us();
		if (cfg.stats_interval && t - t0 >= (uint64_t)cfg.stats_interval * 1000000u) {
			double s = (t - t0) / 1e6;
			char ds[300];
			if (dvr)
				dvr_status(dvr, ds, sizeof ds);
			else
				snprintf(ds, sizeof ds, "state=%s", dvr_off);
			fprintf(stderr, "stats: %.1f fps, %.0f kbps, max frame %llu B, "
				"%.0f pkt/s, send errors %llu | dvr %s\n",
				frames / s, bytes * 8 / s / 1000, (unsigned long long)max_frame,
				(rtp.pkts_sent - pkts0) / s,
				(unsigned long long)rtp.send_errors, ds);
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
	if (dvr) {
		fprintf(stderr, "rkfpv: dvr close\n");
		dvr_destroy(dvr);       /* finishes the current file */
		write_dvr_status(NULL, "off");
	}
	if (bound) {
		fprintf(stderr, "rkfpv: unbind\n");
		RK_MPI_SYS_UnBind(&src, &dst);
	}
	if (vi_up) {
		fprintf(stderr, "rkfpv: vi chn off\n");
		RK_MPI_VI_DisableChn(VI_PIPE, cfg.vi_chn);
	}
	if (venc_up) {
		fprintf(stderr, "rkfpv: venc off\n");
		RK_MPI_VENC_StopRecvFrame(VENC_CHN);
		RK_MPI_VENC_DestroyChn(VENC_CHN);
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
	rtp_h265_close(&rtp);
	fprintf(stderr, "rkfpv: done\n");
	return ret;
}
