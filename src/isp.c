/* Minimal AIQ bring-up, following media/samples/example/common/isp3.x/
 * sample_comm_isp.c (SAMPLE_COMM_ISP_Init/Run/Stop/SetFrameRate).
 * That sample is Copyright 2020 Fuzhou Rockchip Electronics Co., Ltd.,
 * BSD-style license; see THIRD_PARTY_NOTICES.md.
 *
 * Plus the runtime image controls (isp_apply): every control is either a
 * value or "iq" = the IQ file's own value, read at start as the baseline. */
#include "isp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rk_aiq_user_api2_sysctl.h"
#include "rk_aiq_user_api2_ae.h"
#include "rk_aiq_user_api2_imgproc.h"

static rk_aiq_sys_ctx_t *g_ctx;
static char g_sensor[64];

/* the IQ file's values, read right after start */
static struct {
	int have_time, have_gain, have_flicker, have_exp, have_wb, have_sharp, have_cp;
	paRange_t time, gain;
	expPwrLineFreq_t flicker;
	Uapi_ExpSwAttrV2_t exp;
	opMode_t wb;
	unsigned int sharp, bright, contrast, sat, hue;
} g_base;

static isp_settings_t g_applied;
static int g_have_applied;

#define CHK(call, what, ...) do { \
	XCamReturn r_ = (call); \
	fprintf(stderr, "isp: " what "%s\n", ##__VA_ARGS__, r_ == XCAM_RETURN_NO_ERROR ? "" : " FAILED"); \
	if (r_ != XCAM_RETURN_NO_ERROR) fprintf(stderr, "isp:   (rkaiq error %d)\n", (int)r_); \
} while (0)

static void read_baseline(void)
{
	memset(&g_base, 0, sizeof g_base);
	g_base.have_time = !rk_aiq_uapi2_getExpTimeRange(g_ctx, &g_base.time);
	g_base.have_gain = !rk_aiq_uapi2_getExpGainRange(g_ctx, &g_base.gain);
	g_base.have_flicker = !rk_aiq_uapi2_getExpPwrLineFreqMode(g_ctx, &g_base.flicker);
	g_base.have_exp = !rk_aiq_user_api2_ae_getExpSwAttr(g_ctx, &g_base.exp);
	g_base.have_wb = !rk_aiq_uapi2_getWBMode(g_ctx, &g_base.wb);
	g_base.have_sharp = !rk_aiq_uapi2_getSharpness(g_ctx, &g_base.sharp);
	g_base.have_cp = !rk_aiq_uapi2_getBrightness(g_ctx, &g_base.bright) &&
			 !rk_aiq_uapi2_getContrast(g_ctx, &g_base.contrast) &&
			 !rk_aiq_uapi2_getSaturation(g_ctx, &g_base.sat) &&
			 !rk_aiq_uapi2_getHue(g_ctx, &g_base.hue);
	/* logged so units and IQ defaults can be checked on the board */
	if (g_base.have_time)
		fprintf(stderr, "isp: IQ exposure time %.5f..%.5f s\n", g_base.time.min, g_base.time.max);
	if (g_base.have_gain)
		fprintf(stderr, "isp: IQ gain %.2f..%.2f x\n", g_base.gain.min, g_base.gain.max);
	if (g_base.have_exp)
		fprintf(stderr, "isp: IQ AE delay %u/%u frames, damp bright->dark %.2f under %.2f\n",
			g_base.exp.stAuto.BlackDelay, g_base.exp.stAuto.WhiteDelay,
			g_base.exp.stAuto.stAeSpeed.DampBright2Dark, g_base.exp.stAuto.stAeSpeed.DampUnder);
	if (g_base.have_cp)
		fprintf(stderr, "isp: IQ brightness %u contrast %u saturation %u hue %u, sharpness %u\n",
			g_base.bright, g_base.contrast, g_base.sat, g_base.hue, g_base.sharp);
}

int isp_start(const char *iq_dir, int fps, int mirror, int flip)
{
	rk_aiq_static_info_t info;
	rk_aiq_working_mode_t mode = RK_AIQ_WORKING_MODE_NORMAL;

	setenv("HDR_MODE", "0", 1);

	if (rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(0, &info)) {
		fprintf(stderr, "isp: no sensor found\n");
		return -1;
	}
	snprintf(g_sensor, sizeof g_sensor, "%s", info.sensor_info.sensor_name);
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

	read_baseline();
	g_have_applied = 0;
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

const char *isp_sensor_name(void) { return g_sensor; }

/* ---------------------------------------------------------------- controls */

static int changed(int cur, int prev) { return !g_have_applied ? cur != ISP_IQ : cur != prev; }

static void apply_cproc(const char *name, int v, int prev, unsigned int base,
			XCamReturn (*set)(const rk_aiq_sys_ctx_t *, unsigned int))
{
	if (!changed(v, prev))
		return;
	unsigned int level = v == ISP_IQ ? base : (unsigned int)v;
	if (v == ISP_IQ && !g_base.have_cp) {
		fprintf(stderr, "isp: %s back to iq needs a restart (no IQ value read)\n", name);
		return;
	}
	CHK(set(g_ctx, level), "%s %u%s", name, level, v == ISP_IQ ? " (iq)" : "");
}

/* AE metering: the 15x15 zone weights, row 0 = top of the picture (mirror /
 * flip happen in the sensor, before the ISP statistics). The bottom presets
 * weight the ground over the sky so a backlit horizon doesn't turn the
 * ground black; the sky clips instead. */
static void metering_grid(int m, uint8_t *w, const uint8_t *iq)
{
	static const uint8_t bottom[15] = { 1, 1, 1, 1, 1, 2, 2, 3, 4, 5, 6, 6, 6, 6, 6 };
	static const uint8_t strong[15] = { 1, 1, 1, 1, 1, 1, 2, 4, 6, 8, 10, 12, 12, 12, 12 };
	for (int i = 0; i < 15 * 15; i++)
		w[i] = m == ISP_IQ ? iq[i]
		     : m == METER_AVERAGE ? 1
		     : m == METER_BOTTOM ? bottom[i / 15] : strong[i / 15];
}

static void apply_exposure(const isp_settings_t *s, const isp_settings_t *p)
{
	if (changed(s->exp_max_ms, p->exp_max_ms) && g_base.have_time) {
		paRange_t t = g_base.time;
		if (s->exp_max_ms != ISP_IQ) {
			t.max = s->exp_max_ms / 1000.0f;
			if (t.min > t.max)
				t.min = t.max;
		}
		CHK(rk_aiq_uapi2_setExpTimeRange(g_ctx, &t), "exposure time %.5f..%.5f s%s",
		    t.min, t.max, s->exp_max_ms == ISP_IQ ? " (iq)" : "");
	}
	if (changed(s->gain_max, p->gain_max) && g_base.have_gain) {
		paRange_t g = g_base.gain;
		if (s->gain_max != ISP_IQ) {
			g.max = (float)s->gain_max;
			if (g.min > g.max)
				g.min = g.max;
		}
		CHK(rk_aiq_uapi2_setExpGainRange(g_ctx, &g), "gain %.2f..%.2f x%s",
		    g.min, g.max, s->gain_max == ISP_IQ ? " (iq)" : "");
	}

	/* anti-flicker steps exposure in 10 / 8.3 ms units: with a shorter cap it
	 * must be off, so "iq" turns into off then */
	int fl = s->antiflicker;
	if (fl == ISP_IQ && s->exp_max_ms != ISP_IQ && s->exp_max_ms < 10)
		fl = 0;
	int pfl = p->antiflicker;
	if (pfl == ISP_IQ && p->exp_max_ms != ISP_IQ && p->exp_max_ms < 10)
		pfl = 0;
	if ((!g_have_applied && (fl != ISP_IQ)) || (g_have_applied && fl != pfl)) {
		expPwrLineFreq_t f = fl == ISP_IQ ? g_base.flicker
				   : fl == 50 ? EXP_PWR_LINE_FREQ_50HZ
				   : fl == 60 ? EXP_PWR_LINE_FREQ_60HZ : EXP_PWR_LINE_FREQ_DIS;
		if (fl != s->antiflicker)
			fprintf(stderr, "isp: exposure cap %d ms < 10 ms: anti-flicker off\n", s->exp_max_ms);
		if (fl != ISP_IQ || g_base.have_flicker)
			CHK(rk_aiq_uapi2_setExpPwrLineFreqMode(g_ctx, f), "anti-flicker %s",
			    f == EXP_PWR_LINE_FREQ_DIS ? "off" : f == EXP_PWR_LINE_FREQ_50HZ ? "50 Hz" : "60 Hz");
	}

	if ((changed(s->ae_delay, p->ae_delay) || changed(s->ae_damp, p->ae_damp) ||
	     changed(s->metering, p->metering)) && g_base.have_exp) {
		Uapi_ExpSwAttrV2_t e;
		if (rk_aiq_user_api2_ae_getExpSwAttr(g_ctx, &e) == XCAM_RETURN_NO_ERROR) {
			Uapi_AeAttrV2_t *a = &e.stAuto, *b = &g_base.exp.stAuto;
			a->BlackDelay = s->ae_delay == ISP_IQ ? b->BlackDelay : (uint8_t)s->ae_delay;
			a->WhiteDelay = s->ae_delay == ISP_IQ ? b->WhiteDelay : (uint8_t)s->ae_delay;
			a->stAeSpeed.DampBright2Dark = s->ae_damp == ISP_IQ ? b->stAeSpeed.DampBright2Dark
							   : s->ae_damp / 100.0f;
			a->stAeSpeed.DampUnder = s->ae_damp == ISP_IQ ? b->stAeSpeed.DampUnder
						     : s->ae_damp / 100.0f;
			metering_grid(s->metering, e.GridWeights.uCoeff, g_base.exp.GridWeights.uCoeff);
			CHK(rk_aiq_user_api2_ae_setExpSwAttr(g_ctx, e),
			    "AE metering %s, delay %u/%u frames, damp bright->dark %.2f under %.2f",
			    s->metering == ISP_IQ ? "iq" : s->metering == METER_AVERAGE ? "average"
			    : s->metering == METER_BOTTOM ? "bottom" : "bottom_strong",
			    a->BlackDelay, a->WhiteDelay, a->stAeSpeed.DampBright2Dark, a->stAeSpeed.DampUnder);
		}
	}

	if (changed(s->blc, p->blc) || (!g_have_applied && s->blc == 0)) {
		CHK(rk_aiq_uapi2_setBLCMode(g_ctx, s->blc > 0, AE_MEAS_AREA_AUTO), "backlight comp %s",
		    s->blc > 0 ? "on" : "off");
		if (s->blc > 0)
			CHK(rk_aiq_uapi2_setBLCStrength(g_ctx, s->blc), "backlight strength %d", s->blc);
	}
	if (changed(s->hlc, p->hlc) || (!g_have_applied && s->hlc == 0)) {
		CHK(rk_aiq_uapi2_setHLCMode(g_ctx, s->hlc > 0), "highlight comp %s", s->hlc > 0 ? "on" : "off");
		if (s->hlc > 0)
			CHK(rk_aiq_uapi2_setHLCStrength(g_ctx, s->hlc), "highlight strength %d", s->hlc);
	}
}

static void apply_nr(const isp_settings_t *s, const isp_settings_t *p)
{
	if (!changed(s->nr_spatial, p->nr_spatial) && !changed(s->nr_temporal, p->nr_temporal) &&
	    g_have_applied)
		return;
	if (s->nr_spatial == ISP_IQ && s->nr_temporal == ISP_IQ) {
		if (g_have_applied)
			CHK(rk_aiq_uapi2_setNRMode(g_ctx, OP_AUTO), "noise reduction: iq (auto)");
		return;
	}
	/* manual strengths only work in manual NR mode; a control left at
	 * "iq" then gets the middle strength */
	CHK(rk_aiq_uapi2_setNRMode(g_ctx, OP_MANUAL), "noise reduction: manual");
	int sp = s->nr_spatial == ISP_IQ ? 50 : s->nr_spatial;
	int tp = s->nr_temporal == ISP_IQ ? 50 : s->nr_temporal;
	CHK(rk_aiq_uapi2_setMSpaNRStrth(g_ctx, sp > 0, sp > 0 ? (unsigned)sp : 0), "spatial NR %s%d",
	    sp > 0 ? "" : "off ", sp);
	CHK(rk_aiq_uapi2_setMTNRStrth(g_ctx, tp > 0, tp > 0 ? (unsigned)tp : 0), "temporal NR %s%d",
	    tp > 0 ? "" : "off ", tp);
}

void isp_apply(const isp_settings_t *s)
{
	if (!g_ctx)
		return;
	const isp_settings_t *p = &g_applied;

	apply_cproc("brightness", s->brightness, p->brightness, g_base.bright, rk_aiq_uapi2_setBrightness);
	apply_cproc("contrast", s->contrast, p->contrast, g_base.contrast, rk_aiq_uapi2_setContrast);
	apply_cproc("saturation", s->saturation, p->saturation, g_base.sat, rk_aiq_uapi2_setSaturation);
	apply_cproc("hue", s->hue, p->hue, g_base.hue, rk_aiq_uapi2_setHue);

	if (changed(s->sharpness, p->sharpness)) {
		if (s->sharpness == ISP_IQ && !g_base.have_sharp)
			fprintf(stderr, "isp: sharpness back to iq needs a restart\n");
		else {
			unsigned int v = s->sharpness == ISP_IQ ? g_base.sharp : (unsigned int)s->sharpness;
			CHK(rk_aiq_uapi2_setSharpness(g_ctx, v), "sharpness %u%s", v,
			    s->sharpness == ISP_IQ ? " (iq)" : "");
		}
	}

	apply_exposure(s, p);
	apply_nr(s, p);

	if (changed(s->dehaze, p->dehaze)) {
		if (s->dehaze == ISP_IQ)
			fprintf(stderr, "isp: dehaze back to iq takes effect after 'fpv restart video'\n");
		else {
			CHK(rk_aiq_uapi2_setDehazeEnable(g_ctx, s->dehaze > 0), "dehaze %s",
			    s->dehaze > 0 ? "on" : "off");
			if (s->dehaze > 0)
				CHK(rk_aiq_uapi2_setMDehazeStrth(g_ctx, (unsigned)s->dehaze), "dehaze strength %d",
				    s->dehaze);
		}
	}

	if (g_have_applied ? s->wb != p->wb : s->wb != WB_AUTO) {
		static const rk_aiq_wb_scene_t scene[] = {
			RK_AIQ_WBCT_DAYLIGHT, RK_AIQ_WBCT_DAYLIGHT, RK_AIQ_WBCT_CLOUDY_DAYLIGHT,
			RK_AIQ_WBCT_SHADE, RK_AIQ_WBCT_TWILIGHT, RK_AIQ_WBCT_FLUORESCENT,
			RK_AIQ_WBCT_INCANDESCENT,
		};
		if (s->wb == WB_AUTO)
			CHK(rk_aiq_uapi2_setWBMode(g_ctx, g_base.have_wb ? g_base.wb : OP_AUTO),
			    "white balance auto");
		else {
			CHK(rk_aiq_uapi2_setWBMode(g_ctx, OP_MANUAL), "white balance manual");
			CHK(rk_aiq_uapi2_setMWBScene(g_ctx, scene[s->wb]), "white balance scene %d", s->wb);
		}
	}

	g_applied = *s;
	g_have_applied = 1;
}
