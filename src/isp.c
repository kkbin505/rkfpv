/* Minimal AIQ bring-up, following media/samples/example/common/isp3.x/
 * sample_comm_isp.c (SAMPLE_COMM_ISP_Init/Run/Stop/SetFrameRate). */
#include "isp.h"

#include <stdio.h>
#include <stdlib.h>

#include "rk_aiq_user_api2_sysctl.h"
#include "rk_aiq_user_api2_ae.h"
#include "rk_aiq_user_api2_imgproc.h"

static rk_aiq_sys_ctx_t *g_ctx;

int isp_start(const char *iq_dir, int fps, int mirror, int flip)
{
	rk_aiq_static_info_t info;
	rk_aiq_working_mode_t mode = RK_AIQ_WORKING_MODE_NORMAL;

	setenv("HDR_MODE", "0", 1);

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
	if (rk_aiq_uapi2_sysctl_prepare(g_ctx, 0, 0, mode)) {
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
