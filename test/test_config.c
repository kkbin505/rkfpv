/* rkfpv config parser: the shipped rkfpv.conf loads, and the ISP values */
#include "config.h"
#include <stdio.h>
int main(int argc, char **argv) {
	rkfpv_config_t c; config_defaults(&c);
	int r = config_load_file(&c, argv[1]);
	isp_settings_t *i = &c.isp;
	printf("load=%d bri=%d sat=%d sharp=%d wb=%d exp=%d gain=%d flick=%d delay=%d damp=%d blc=%d hlc=%d nrs=%d nrt=%d dehaze=%d\n",
	       r, i->brightness, i->saturation, i->sharpness, i->wb, i->exp_max_ms, i->gain_max, i->antiflicker,
	       i->ae_delay, i->ae_damp, i->blc, i->hlc, i->nr_spatial, i->nr_temporal, i->dehaze);
	rkfpv_config_t d; config_defaults(&d);
	int bad = 0;
	bad |= config_set(&d, "isp_exp_max", "4") || d.isp.exp_max_ms != 4;
	bad |= config_set(&d, "isp_exp_max", "off") == 0;       /* off not allowed there */
	bad |= config_set(&d, "isp_exp_max", "34") == 0;
	bad |= config_set(&d, "isp_nr_temporal", "iq") || d.isp.nr_temporal != -1;
	bad |= config_set(&d, "isp_antiflicker", "60") || d.isp.antiflicker != 60;
	bad |= config_set(&d, "isp_antiflicker", "55") == 0;
	bad |= config_set(&d, "isp_wb", "shade") || d.isp.wb != 3;
	bad |= config_set(&d, "isp_wb", "purple") == 0;
	bad |= d.dvr_osd != 1 || d.blackbox != 0 || d.blackbox_baud != 1500000;   /* defaults */
	bad |= config_set(&d, "dvr_osd", "0") || d.dvr_osd != 0;
	bad |= config_set(&d, "dvr_osd", "2") == 0;
	bad |= d.osd_ext_port != 0 || d.osd_ext_ttl_ms != 500;             /* defaults */
	bad |= config_set(&d, "osd_ext_port", "14580") || d.osd_ext_port != 14580;
	bad |= config_set(&d, "osd_ext_port", "65536") == 0;
	bad |= config_set(&d, "osd_ext_ttl_ms", "99") == 0;
	bad |= config_set(&d, "osd_ext_ttl_ms", "10000") || d.osd_ext_ttl_ms != 10000;
	bad |= d.frame_tap != 0 || d.frame_tap_fps != 10 || d.frame_tap_scale != 2;   /* defaults */
	bad |= config_set(&d, "frame_tap_fps", "31") == 0;
	bad |= config_set(&d, "frame_tap_scale", "4") || d.frame_tap_scale != 4;
	bad |= config_set(&d, "frame_tap_scale", "0") == 0;
	bad |= config_set(&d, "blackbox", "1") || d.blackbox != 1;
	bad |= config_set(&d, "blackbox_baud", "2000000") || d.blackbox_baud != 2000000;
	bad |= config_set(&d, "blackbox_baud", "3000000") == 0;
	printf("edge cases %s\n", bad ? "FAIL" : "ok");
	return r || bad;
}
