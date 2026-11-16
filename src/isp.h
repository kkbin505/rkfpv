#ifndef RKFPV_ISP_H
#define RKFPV_ISP_H

/* Starts Rockchip AIQ (3A + image tuning) for camera 0 with the IQ files in
 * iq_dir. Must run before the VI channel is enabled. */
int isp_start(const char *iq_dir, int fps, int mirror, int flip);
void isp_stop(void);

#endif
