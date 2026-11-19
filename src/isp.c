/* Minimal AIQ bring-up, following media/samples/example/common/isp3.x/
 * sample_comm_isp.c (SAMPLE_COMM_ISP_Init/Run/Stop/SetFrameRate).
 * That sample is Copyright 2020 Fuzhou Rockchip Electronics Co., Ltd.,
 * BSD-style license; see THIRD_PARTY_NOTICES.md. */
#include "isp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/v4l2-subdev.h>

#include "rk_aiq_user_api2_sysctl.h"
#include "rk_aiq_user_api2_ae.h"
#include "rk_aiq_user_api2_imgproc.h"

static rk_aiq_sys_ctx_t *g_ctx;

/* rk_aiq_uapi2_sysctl_prepare() only checks the size it is given, it never
 * switches the sensor mode: set it on the sensor subdev ourselves, before
 * AIQ/VI read the format. Rockchip sensor subdevs are named
 * "m<NN>_<f|b>_<sensor> <bus>-<addr>". */
static int sensor_set_size(int w, int h)
{
	char path[64], name[64];
	for (int i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/sys/class/video4linux/v4l-subdev%d/name", i);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		int ok = fgets(name, sizeof(name), f) != NULL;
		fclose(f);
		unsigned n;
		char side;
		if (!ok || sscanf(name, "m%2u_%c_", &n, &side) != 2 || (side != 'f' && side != 'b'))
			continue;
		name[strcspn(name, "\n")] = 0;

		snprintf(path, sizeof(path), "/dev/v4l-subdev%d", i);
		int fd = open(path, O_RDWR);
		if (fd < 0) {
			perror(path);
			return -1;
		}
		struct v4l2_subdev_format fmt;
		memset(&fmt, 0, sizeof(fmt));
		fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
		fmt.pad = 0;
		int r = ioctl(fd, VIDIOC_SUBDEV_G_FMT, &fmt);
		if (!r) {
			fmt.format.width = w;
			fmt.format.height = h;
			r = ioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt);
		}
		close(fd);
		if (r) {
			perror("isp: sensor set format");
			return -1;
		}
		fprintf(stderr, "isp: sensor %s mode %ux%u (asked %dx%d)\n", name,
			fmt.format.width, fmt.format.height, w, h);
		if ((int)fmt.format.width != w || (int)fmt.format.height != h) {
			fprintf(stderr, "isp: sensor has no %dx%d mode\n", w, h);
			return -1;
		}
		return 0;
	}
	fprintf(stderr, "isp: no sensor subdev found\n");
	return -1;
}

int isp_start(const char *iq_dir, int fps, int mirror, int flip,
	      int sensor_w, int sensor_h)
{
	rk_aiq_static_info_t info;
	rk_aiq_working_mode_t mode = RK_AIQ_WORKING_MODE_NORMAL;

	setenv("HDR_MODE", "0", 1);

	if (sensor_w && sensor_h && sensor_set_size(sensor_w, sensor_h))
		return -1;

	if (rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(0, &info)) {
		fprintf(stderr, "isp: no sensor found\n");
		return -1;
	}
	fprintf(stderr, "isp: sensor %s, iq dir %s\n", info.sensor_info.sensor_name, iq_dir);

	rk_aiq_uapi2_sysctl_preInit_devBufCnt(info.sensor_info.sensor_name, "rkraw_rx", 2);

	g_ctx = rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, iq_dir, NULL, NULL);
	if (!g_ctx) {
		fprintf(stderr, "isp: rk_aiq_uapi2_sysctl_init failed\n");
		return -1;
	}
	if (rk_aiq_uapi2_sysctl_prepare(g_ctx, sensor_w, sensor_h, mode)) {
		fprintf(stderr, "isp: prepare failed\n");
		rk_aiq_uapi2_sysctl_deinit(g_ctx);
		g_ctx = NULL;
		return -1;
	}
	if (rk_aiq_uapi2_sysctl_start(g_ctx)) {
		fprintf(stderr, "isp: start failed\n");
		rk_aiq_uapi2_sysctl_deinit(g_ctx);
		g_ctx = NULL;
		return -1;
	}

	/* Fix the sensor frame rate so AE does not stretch exposure (and frame
	 * time) in low light. */
	Uapi_ExpSwAttrV2_t exp;
	if (!rk_aiq_user_api2_ae_getExpSwAttr(g_ctx, &exp)) {
		exp.stAuto.stFrmRate.isFpsFix = true;
		exp.stAuto.stFrmRate.FpsValue = fps;
		rk_aiq_user_api2_ae_setExpSwAttr(g_ctx, exp);
	}

	if (mirror || flip)
		rk_aiq_uapi2_setMirrorFlip(g_ctx, mirror, flip, 4);

	return 0;
}

void isp_stop(void)
{
	if (!g_ctx)
		return;
	rk_aiq_uapi2_sysctl_stop(g_ctx, false);
	rk_aiq_uapi2_sysctl_deinit(g_ctx);
	g_ctx = NULL;
}
