#ifndef RKFPV_ISP_H
#define RKFPV_ISP_H

#include "config.h"

/* Starts Rockchip AIQ (3A + image tuning) for camera 0 with the IQ files in
 * iq_dir. Must run before the VI channel is enabled. Reads the IQ file's
 * baseline values so isp_apply() can restore them for settings set to "iq".
 * sensor_w/h pick the sensor mode (e.g. 1152x648 = the binned high frame rate
 * mode); 0 keeps the driver's current one. */
int isp_start(const char *iq_dir, int fps, int mirror, int flip,
	      int sensor_w, int sensor_h);
void isp_stop(void);
/* Current auto-exposure result: exposure time in ms, total gain (1.0 = ISO 50).
 * Returns -1 when AIQ is not running. */
int isp_exposure(float *ms, float *gain);

/* Applies the ISP runtime controls (at start and on SIGHUP). Only settings
 * that changed since the last call are sent to rkaiq; each call is logged
 * with its return code. */
void isp_apply(const isp_settings_t *s);

/* Sensor name rkaiq reported, "" before isp_start */
const char *isp_sensor_name(void);

#endif
