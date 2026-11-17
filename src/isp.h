#ifndef RKFPV_ISP_H
#define RKFPV_ISP_H

#include "config.h"

/* Starts Rockchip AIQ (3A + image tuning) for camera 0 with the IQ files in
 * iq_dir. Must run before the VI channel is enabled. Reads the IQ file's
 * baseline values so isp_apply() can restore them for settings set to "iq". */
int isp_start(const char *iq_dir, int fps, int mirror, int flip);
void isp_stop(void);

/* Applies the ISP runtime controls (at start and on SIGHUP). Only settings
 * that changed since the last call are sent to rkaiq; each call is logged
 * with its return code. */
void isp_apply(const isp_settings_t *s);

/* Sensor name rkaiq reported, "" before isp_start */
const char *isp_sensor_name(void);

#endif
