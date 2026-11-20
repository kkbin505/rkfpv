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
	bad |= d.isp.metering != -1;                             /* default iq */
	bad |= config_set(&d, "isp_metering", "bottom_strong") || d.isp.metering != METER_BOTTOM_STRONG;
	bad |= config_set(&d, "isp_metering", "iq") || d.isp.metering != -1;
	bad |= config_set(&d, "isp_metering", "top") == 0;
	printf("edge cases %s\n", bad ? "FAIL" : "ok");
	return r || bad;
}
