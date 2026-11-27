// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX462 sensor driver for the Rockchip camera stack (rkcif / rkisp,
 * rkaiq), 2- or 4-lane MIPI CSI-2, RAW10, linear mode only.
 *
 * Structure, controls and rkmodule ioctls follow Rockchip's imx327.c
 * (Copyright (C) 2020 Rockchip Electronics Co., Ltd.), so rkaiq can drive it.
 * The register sequence follows the mainline imx290.c, IMX462 variant
 * (Copyright (C) 2019 FRAMOS GmbH, Copyright (C) 2019 Linaro Ltd.;
 * IMX462 settings by Raspberry Pi Ltd.). Rockchip's imx327.c only carries
 * 4-lane tables; Raspberry Pi style modules are 2-lane.
 *
 * Builds against the Rockchip 5.10 and 6.1 BSP kernels.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/rk-camera-module.h>
#include <linux/rk-preisp.h>	/* GAIN_MODE_LCG / HCG */
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x00)
#define IMX462_NAME			"imx462"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
#define PAD_STATE			struct v4l2_subdev_state
#define FH_PAD(fh)			((fh)->state)
#else
#define PAD_STATE			struct v4l2_subdev_pad_config
#define FH_PAD(fh)			((fh)->pad)
#endif

/* registers */
#define REG_STANDBY			0x3000
#define REG_REGHOLD			0x3001
#define REG_XMSTA			0x3002
#define REG_SW_RESET			0x3003
#define REG_CTRL_07			0x3007	/* window mode, flips */
#define  WINMODE_1080P			(0 << 4)
#define  WINMODE_720P			(1 << 4)
#define  VREVERSE			BIT(0)
#define  HREVERSE			BIT(1)
#define REG_FR_FDG_SEL			0x3009
#define  FRSEL_60FPS			BIT(0)
#define  FDG_HCG			BIT(4)
#define REG_BLKLEVEL_L			0x300a
#define REG_BLKLEVEL_H			0x300b
#define REG_GAIN			0x3014
#define REG_VMAX_L			0x3018	/* 18 bit, little endian */
#define REG_HMAX_L			0x301c	/* 16 bit */
#define REG_CHIP_ID			0x301e
#define REG_SHS1_L			0x3020	/* 18 bit */
#define REG_PGCTRL			0x308c
#define  PGCTRL_REGEN			BIT(0)
#define  PGCTRL_THRU			BIT(1)
#define  PGCTRL_MODE(n)			((n) << 4)
#define REG_INCKSEL1			0x305c
#define REG_INCKSEL2			0x305d
#define REG_INCKSEL3			0x305e
#define REG_INCKSEL4			0x305f
#define REG_INCKSEL5			0x315e
#define REG_INCKSEL6			0x3164
#define REG_REPETITION			0x3405
#define REG_PHY_LANE_NUM		0x3407
#define REG_CSI_LANE_MODE		0x3443
#define REG_EXTCK_FREQ			0x3444	/* 16 bit */
#define REG_TCLKPOST			0x3446	/* 0x3446..0x3455: 16 bit each */
#define REG_INCKSEL7			0x3480

#define CHIP_ID_IMX327			0xb2	/* what imx327.c expects at 0x301e */
#define BLACK_LEVEL			0x3c	/* 10 bit: 60 */
#define PIXEL_RATE			148500000 /* HMAX counts at this rate */
#define VMAX_MAX			0x3ffff
#define EXPOSURE_MIN			1
#define EXPOSURE_OFFSET			2	/* exposure <= VMAX - 2 */
#define GAIN_MAX			0xee	/* 0.3 dB steps; > 98 is digital */
#define LINK_FREQ_1080P_2L		445500000
#define LINK_FREQ_720P_2L		297000000
#define LINK_FREQ_1080P_4L		222750000
#define LINK_FREQ_720P_4L		148500000

#define REG_NULL			0xffff
#define REG_DELAY			0xfffe

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

static bool hcg;
module_param(hcg, bool, 0644);
MODULE_PARM_DESC(hcg, "start in high conversion gain (better in the dark)");

static const char * const imx462_supply_names[] = {
	"avdd",		/* analog */
	"dovdd",	/* I/O */
	"dvdd",		/* core */
};
#define NUM_SUPPLIES ARRAY_SIZE(imx462_supply_names)

struct regval {
	u16 addr;
	u8 val;
};

enum { CLK_37_125, CLK_74_25, CLK_NUM };

struct inck_cfg {
	u8 incksel[6];		/* INCKSEL1..INCKSEL6 */
};

/* MIPI D-PHY timing, by link frequency (mainline imx290.c) */
struct csi_cfg {
	s64 link_freq;
	u8 repetition;
	u16 t[8];	/* tclkpost thszero thsprepare tclktrail thstrail
			 * tclkzero tclkprepare tlpx */
};

struct imx462_mode {
	u32 width;
	u32 height;
	u32 hts;		/* HMAX at PIXEL_RATE */
	u32 vts;		/* VMAX, gives max_fps */
	struct v4l2_fract max_fps;
	u32 exp_def;
	u8 winmode;
	u8 wincv_ob;		/* WINWV_OB */
	u8 opb_size_v;
	struct inck_cfg inck[CLK_NUM];
	s64 link_freq_2l;
	s64 link_freq_4l;
};

struct imx462 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	int			clk_idx;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[NUM_SUPPLIES];
	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*link_freq;
	struct v4l2_ctrl	*h_flip;
	struct v4l2_ctrl	*v_flip;
	struct v4l2_ctrl	*test_pattern;
	struct mutex		mutex;

	struct v4l2_fwnode_endpoint bus_cfg;
	u32			nlanes;
	bool			streaming;
	bool			power_on;
	bool			hcg;
	const struct imx462_mode *cur_mode;
	u32			cur_vts;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
};

#define to_imx462(sd) container_of(sd, struct imx462, subdev)

/* common settings, mainline imx290.c: global + IMX462 model settings */
static const struct regval imx462_common_regs[] = {
	{0x303a, 12},		/* WINWV_OB */
	{0x3040, 0x00},		/* WINPH */
	{0x3041, 0x00},
	{0x303c, 0x00},		/* WINPV */
	{0x303d, 0x00},
	{0x3042, 0x9c},		/* WINWH 1948 */
	{0x3043, 0x07},
	{0x303e, 0x49},		/* WINWV 1097 */
	{0x303f, 0x04},
	{0x304b, 0x0a},		/* XSOUTSEL: XVS, XHS out */
	{0x3012, 0x64},
	{0x3013, 0x00},
	/* IMX462 */
	{0x300f, 0x00},
	{0x3010, 0x21},
	{0x3011, 0x02},
	{0x3016, 0x09},
	{0x3070, 0x02},
	{0x3071, 0x11},
	{0x309b, 0x10},
	{0x309c, 0x22},
	{0x30a2, 0x02},
	{0x30a6, 0x20},
	{0x30a8, 0x20},
	{0x30aa, 0x20},
	{0x30ac, 0x20},
	{0x30b0, 0x43},
	{0x3119, 0x9e},
	{0x311c, 0x1e},
	{0x311e, 0x08},
	{0x3128, 0x05},
	{0x313d, 0x83},
	{0x3150, 0x03},
	{0x317e, 0x00},
	{0x32b8, 0x50},
	{0x32b9, 0x10},
	{0x32ba, 0x00},
	{0x32bb, 0x04},
	{0x32c8, 0x50},
	{0x32c9, 0x10},
	{0x32ca, 0x00},
	{0x32cb, 0x04},
	{0x332c, 0xd3},
	{0x332d, 0x10},
	{0x332e, 0x0d},
	{0x3358, 0x06},
	{0x3359, 0xe1},
	{0x335a, 0x11},
	{0x3360, 0x1e},
	{0x3361, 0x61},
	{0x3362, 0x10},
	{0x33b0, 0x50},
	{0x33b2, 0x1a},
	{0x33b3, 0x04},
	/* RAW10 */
	{0x3005, 0x00},		/* ADBIT */
	{0x3046, 0x00},		/* OUT_CTRL: 10 bit, MIPI */
	{0x3129, 0x1d},		/* ADBIT1 */
	{0x317c, 0x12},		/* ADBIT2 */
	{0x31ec, 0x37},		/* ADBIT3 */
	{0x3441, 0x0a},		/* CSI_DT_FMT RAW10 */
	{0x3442, 0x0a},
	{REG_BLKLEVEL_L, BLACK_LEVEL},
	{REG_BLKLEVEL_H, 0x00},
	{REG_NULL, 0x00},
};

static const struct csi_cfg imx462_csi_cfgs[] = {
	{ 445500000, 0x00, { 119, 103, 71, 55, 63, 255, 63, 55 } },
	{ 297000000, 0x00, { 103,  87, 47, 39, 47, 191, 47, 39 } },
	{ 222750000, 0x10, {  87,  55, 31, 31, 31, 119, 31, 23 } },
	{ 148500000, 0x10, {  79,  47, 23, 23, 23,  87, 23, 23 } },
};

/*
 * Both modes run at 60 fps (HMAX * VMAX = 2475000 at 148.5 MHz); lower
 * rates come from a longer VMAX (vblank). 720p is a window of the sensor,
 * not a scaled picture: the field of view is narrower than 1080p.
 */
static const struct imx462_mode supported_modes[] = {
	{
		.width = 1920,
		.height = 1080,
		.hts = 2200,
		.vts = 1125,
		.max_fps = { .numerator = 10000, .denominator = 600000 },
		.exp_def = 1000,
		.winmode = WINMODE_1080P,
		.wincv_ob = 12,
		.opb_size_v = 10,
		.inck = {
			[CLK_37_125] = { { 0x18, 0x03, 0x20, 0x01, 0x1a, 0x1a } },
			[CLK_74_25]  = { { 0x0c, 0x03, 0x10, 0x01, 0x1b, 0x1b } },
		},
		.link_freq_2l = LINK_FREQ_1080P_2L,
		.link_freq_4l = LINK_FREQ_1080P_4L,
	}, {
		.width = 1280,
		.height = 720,
		.hts = 3300,
		.vts = 750,
		.max_fps = { .numerator = 10000, .denominator = 600000 },
		.exp_def = 600,
		.winmode = WINMODE_720P,
		.wincv_ob = 6,
		.opb_size_v = 4,
		.inck = {
			[CLK_37_125] = { { 0x20, 0x00, 0x20, 0x01, 0x1a, 0x1a } },
			[CLK_74_25]  = { { 0x10, 0x00, 0x10, 0x01, 0x1b, 0x1b } },
		},
		.link_freq_2l = LINK_FREQ_720P_2L,
		.link_freq_4l = LINK_FREQ_720P_4L,
	},
};

static const s64 link_freq_2l[] = { LINK_FREQ_1080P_2L, LINK_FREQ_720P_2L };
static const s64 link_freq_4l[] = { LINK_FREQ_1080P_4L, LINK_FREQ_720P_4L };

static const char * const imx462_test_pattern_menu[] = {
	"Disabled",
	"Sequence Pattern 1",
	"Horizontal Color-bar Chart",
	"Vertical Color-bar Chart",
	"Sequence Pattern 2",
	"Gradation Pattern 1",
	"Gradation Pattern 2",
	"000/555h Toggle Pattern",
};

static int imx462_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };

	return i2c_master_send(client, buf, 3) == 3 ? 0 : -EIO;
}

/* little-endian multi-byte register: low byte at the lowest address */
static int imx462_write_le(struct i2c_client *client, u16 reg, u32 val,
			   int bytes)
{
	int i, ret = 0;

	for (i = 0; i < bytes && !ret; i++)
		ret = imx462_write_reg(client, reg + i, (val >> (8 * i)) & 0xff);
	return ret;
}

static int imx462_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	int i, ret = 0;

	for (i = 0; !ret && regs[i].addr != REG_NULL; i++) {
		if (regs[i].addr == REG_DELAY)
			usleep_range(regs[i].val * 1000, regs[i].val * 1000 + 1000);
		else
			ret = imx462_write_reg(client, regs[i].addr, regs[i].val);
	}
	return ret;
}

static int imx462_read_reg(struct i2c_client *client, u16 reg, u8 *val)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0, .len = 2, .buf = addr },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};

	return i2c_transfer(client->adapter, msgs, 2) == 2 ? 0 : -EIO;
}

static s64 imx462_mode_link_freq(struct imx462 *imx462,
				 const struct imx462_mode *mode)
{
	return imx462->nlanes == 4 ? mode->link_freq_4l : mode->link_freq_2l;
}

static u8 imx462_fdg_sel(struct imx462 *imx462)
{
	return FRSEL_60FPS | (imx462->hcg ? FDG_HCG : 0);
}

static void imx462_update_ranges(struct imx462 *imx462,
				 const struct imx462_mode *mode)
{
	u32 hblank = mode->hts - mode->width;
	u32 vblank = mode->vts - mode->height;

	__v4l2_ctrl_modify_range(imx462->hblank, hblank, hblank, 1, hblank);
	__v4l2_ctrl_modify_range(imx462->vblank, vblank,
				 VMAX_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(imx462->link_freq,
			   mode == &supported_modes[0] ? 0 : 1);
	imx462->cur_vts = mode->vts;
}

static int imx462_set_fmt(struct v4l2_subdev *sd, PAD_STATE *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx462 *imx462 = to_imx462(sd);
	const struct imx462_mode *mode;

	mutex_lock(&imx462->mutex);
	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);
	fmt->format.code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, state, fmt->pad) = fmt->format;
#else
		mutex_unlock(&imx462->mutex);
		return -ENOTTY;
#endif
	} else if (imx462->streaming) {
		mutex_unlock(&imx462->mutex);
		return -EBUSY;
	} else {
		imx462->cur_mode = mode;
		imx462_update_ranges(imx462, mode);
	}
	mutex_unlock(&imx462->mutex);
	return 0;
}

static int imx462_get_fmt(struct v4l2_subdev *sd, PAD_STATE *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx462 *imx462 = to_imx462(sd);
	const struct imx462_mode *mode = imx462->cur_mode;

	mutex_lock(&imx462->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, state, fmt->pad);
#else
		mutex_unlock(&imx462->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = MEDIA_BUS_FMT_SRGGB10_1X10;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&imx462->mutex);
	return 0;
}

static int imx462_enum_mbus_code(struct v4l2_subdev *sd, PAD_STATE *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int imx462_enum_frame_sizes(struct v4l2_subdev *sd, PAD_STATE *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;
	if (fse->code != MEDIA_BUS_FMT_SRGGB10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = supported_modes[fse->index].width;
	fse->min_height = fse->max_height = supported_modes[fse->index].height;
	return 0;
}

static int imx462_enum_frame_interval(struct v4l2_subdev *sd, PAD_STATE *state,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	if (fie->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;
	fie->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;
	fie->reserved[0] = NO_HDR;
	return 0;
}

static int imx462_get_selection(struct v4l2_subdev *sd, PAD_STATE *state,
				struct v4l2_subdev_selection *sel)
{
	struct imx462 *imx462 = to_imx462(sd);

	if (sel->target != V4L2_SEL_TGT_CROP_BOUNDS)
		return -EINVAL;
	/* the sensor already outputs exactly width x height */
	sel->r.left = 0;
	sel->r.top = 0;
	sel->r.width = imx462->cur_mode->width;
	sel->r.height = imx462->cur_mode->height;
	return 0;
}

static int imx462_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx462 *imx462 = to_imx462(sd);

	fi->interval = imx462->cur_mode->max_fps;
	return 0;
}

static int imx462_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	struct imx462 *imx462 = to_imx462(sd);

	config->type = V4L2_MBUS_CSI2_DPHY;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
	config->bus.mipi_csi2 = imx462->bus_cfg.bus.mipi_csi2;
#else
	/* 5.10: lane count as V4L2_MBUS_CSI2_n_LANE flag bits */
	config->flags = (1 << (imx462->nlanes - 1)) | V4L2_MBUS_CSI2_CHANNEL_0 |
			V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
#endif
	return 0;
}

static void imx462_get_module_inf(struct imx462 *imx462,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strscpy(inf->base.sensor, IMX462_NAME, sizeof(inf->base.sensor));
	strscpy(inf->base.module, imx462->module_name, sizeof(inf->base.module));
	strscpy(inf->base.lens, imx462->len_name, sizeof(inf->base.lens));
}

static int imx462_set_conversion_gain(struct imx462 *imx462, u32 cg)
{
	struct i2c_client *client = imx462->client;
	int ret;

	if (cg != GAIN_MODE_LCG && cg != GAIN_MODE_HCG)
		return -EINVAL;
	imx462->hcg = cg == GAIN_MODE_HCG;
	if (!imx462->streaming)
		return 0;
	ret = imx462_write_reg(client, REG_REGHOLD, 1);
	ret |= imx462_write_reg(client, REG_FR_FDG_SEL, imx462_fdg_sel(imx462));
	ret |= imx462_write_reg(client, REG_REGHOLD, 0);
	return ret;
}

static long imx462_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct imx462 *imx462 = to_imx462(sd);
	struct rkmodule_hdr_cfg *hdr;
	long ret = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		imx462_get_module_inf(imx462, arg);
		break;
	case RKMODULE_GET_HDR_CFG:
		hdr = arg;
		hdr->esp.mode = HDR_NORMAL_VC;
		hdr->hdr_mode = NO_HDR;
		break;
	case RKMODULE_SET_HDR_CFG:
		hdr = arg;
		if (hdr->hdr_mode != NO_HDR) {
			dev_err(&imx462->client->dev, "hdr mode %u not supported\n",
				hdr->hdr_mode);
			ret = -EINVAL;
		}
		break;
	case RKMODULE_SET_CONVERSION_GAIN:
		ret = imx462_set_conversion_gain(imx462, *(u32 *)arg);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		ret = imx462_write_reg(imx462->client, REG_STANDBY,
				       *(u32 *)arg ? 0 : 1);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long imx462_compat_ioctl32(struct v4l2_subdev *sd, unsigned int cmd,
				  unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_hdr_cfg hdr;
	u32 val;
	long ret;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf)
			return -ENOMEM;
		ret = imx462_ioctl(sd, cmd, inf);
		if (!ret && copy_to_user(up, inf, sizeof(*inf)))
			ret = -EFAULT;
		kfree(inf);
		return ret;
	case RKMODULE_GET_HDR_CFG:
		ret = imx462_ioctl(sd, cmd, &hdr);
		if (!ret && copy_to_user(up, &hdr, sizeof(hdr)))
			ret = -EFAULT;
		return ret;
	case RKMODULE_SET_HDR_CFG:
		if (copy_from_user(&hdr, up, sizeof(hdr)))
			return -EFAULT;
		return imx462_ioctl(sd, cmd, &hdr);
	case RKMODULE_SET_CONVERSION_GAIN:
	case RKMODULE_SET_QUICK_STREAM:
		if (copy_from_user(&val, up, sizeof(val)))
			return -EFAULT;
		return imx462_ioctl(sd, cmd, &val);
	default:
		return -ENOIOCTLCMD;
	}
}
#endif

static int imx462_write_csi_cfg(struct imx462 *imx462, s64 link_freq)
{
	struct i2c_client *client = imx462->client;
	const struct csi_cfg *c = NULL;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(imx462_csi_cfgs); i++)
		if (imx462_csi_cfgs[i].link_freq == link_freq)
			c = &imx462_csi_cfgs[i];
	if (!c)
		return -EINVAL;
	ret = imx462_write_reg(client, REG_REPETITION, c->repetition);
	for (i = 0; i < 8 && !ret; i++)
		ret = imx462_write_le(client, REG_TCLKPOST + 2 * i, c->t[i], 2);
	return ret;
}

static int imx462_write_clock(struct imx462 *imx462,
			      const struct imx462_mode *mode)
{
	static const u16 inck_regs[6] = {
		REG_INCKSEL1, REG_INCKSEL2, REG_INCKSEL3,
		REG_INCKSEL4, REG_INCKSEL5, REG_INCKSEL6,
	};
	struct i2c_client *client = imx462->client;
	const struct inck_cfg *inck = &mode->inck[imx462->clk_idx];
	bool fast = imx462->clk_idx == CLK_74_25;
	int i, ret;

	ret = imx462_write_le(client, REG_EXTCK_FREQ,
			      fast ? 74250 * 256 / 1000 : 37125 * 256 / 1000, 2);
	ret |= imx462_write_reg(client, REG_INCKSEL7, fast ? 0x92 : 0x49);
	for (i = 0; i < 6 && !ret; i++)
		ret = imx462_write_reg(client, inck_regs[i], inck->incksel[i]);
	return ret;
}

static int __imx462_start_stream(struct imx462 *imx462)
{
	struct i2c_client *client = imx462->client;
	const struct imx462_mode *mode = imx462->cur_mode;
	int ret;

	/* reset, then stay in standby with the master clock stopped */
	ret = imx462_write_reg(client, REG_SW_RESET, 1);
	usleep_range(16000, 17000);
	ret |= imx462_write_reg(client, REG_STANDBY, 1);
	ret |= imx462_write_reg(client, REG_XMSTA, 1);
	if (ret)
		return ret;

	ret = imx462_write_array(client, imx462_common_regs);
	ret = ret ?: imx462_write_clock(imx462, mode);
	ret = ret ?: imx462_write_reg(client, REG_PHY_LANE_NUM, imx462->nlanes - 1);
	ret = ret ?: imx462_write_reg(client, REG_CSI_LANE_MODE, imx462->nlanes - 1);
	ret = ret ?: imx462_write_reg(client, REG_FR_FDG_SEL, imx462_fdg_sel(imx462));
	ret = ret ?: imx462_write_csi_cfg(imx462, imx462_mode_link_freq(imx462, mode));
	/* mode: window, output size, line length */
	ret = ret ?: imx462_write_reg(client, 0x303a, mode->wincv_ob);
	ret = ret ?: imx462_write_reg(client, 0x3414, mode->opb_size_v);
	ret = ret ?: imx462_write_le(client, 0x3472, mode->width, 2);
	ret = ret ?: imx462_write_le(client, 0x3418, mode->height, 2);
	ret = ret ?: imx462_write_le(client, REG_HMAX_L, mode->hts, 2);
	if (ret)
		return ret;
	usleep_range(10000, 11000);

	/* VMAX, exposure, gain, flips (CTRL_07 with the window mode) */
	ret = __v4l2_ctrl_handler_setup(&imx462->ctrl_handler);
	if (ret)
		return ret;

	ret = imx462_write_reg(client, REG_STANDBY, 0);
	msleep(30);
	return ret ?: imx462_write_reg(client, REG_XMSTA, 0);
}

static int __imx462_stop_stream(struct imx462 *imx462)
{
	int ret = imx462_write_reg(imx462->client, REG_STANDBY, 1);

	return ret ?: imx462_write_reg(imx462->client, REG_XMSTA, 1);
}

static int imx462_s_stream(struct v4l2_subdev *sd, int on)
{
	struct imx462 *imx462 = to_imx462(sd);
	struct i2c_client *client = imx462->client;
	int ret = 0;

	mutex_lock(&imx462->mutex);
	on = !!on;
	if (on == imx462->streaming)
		goto unlock;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock;
		}
		ret = __imx462_start_stream(imx462);
		if (ret) {
			v4l2_err(sd, "start stream failed while writing registers\n");
			pm_runtime_put(&client->dev);
			goto unlock;
		}
	} else {
		__imx462_stop_stream(imx462);
		pm_runtime_put(&client->dev);
	}
	imx462->streaming = on;
unlock:
	mutex_unlock(&imx462->mutex);
	return ret;
}

static int imx462_s_power(struct v4l2_subdev *sd, int on)
{
	struct imx462 *imx462 = to_imx462(sd);
	struct i2c_client *client = imx462->client;
	int ret = 0;

	mutex_lock(&imx462->mutex);
	if (imx462->power_on == !!on)
		goto unlock;
	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock;
		}
		ret = 0;
		imx462->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		imx462->power_on = false;
	}
unlock:
	mutex_unlock(&imx462->mutex);
	return ret;
}

static int __imx462_power_on(struct imx462 *imx462)
{
	struct device *dev = &imx462->client->dev;
	int ret;

	if (!IS_ERR_OR_NULL(imx462->pins_default) &&
	    pinctrl_select_state(imx462->pinctrl, imx462->pins_default) < 0)
		dev_err(dev, "could not set pins\n");

	ret = clk_prepare_enable(imx462->xvclk);
	if (ret < 0) {
		dev_err(dev, "failed to enable xvclk\n");
		return ret;
	}
	ret = regulator_bulk_enable(NUM_SUPPLIES, imx462->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable regulators\n");
		clk_disable_unprepare(imx462->xvclk);
		return ret;
	}
	/* Rockchip convention: reset-gpios 1 = running, pwdn-gpios 1 = on */
	if (!IS_ERR(imx462->reset_gpio))
		gpiod_set_value_cansleep(imx462->reset_gpio, 0);
	usleep_range(500, 1000);
	if (!IS_ERR(imx462->reset_gpio))
		gpiod_set_value_cansleep(imx462->reset_gpio, 1);
	if (!IS_ERR(imx462->pwdn_gpio))
		gpiod_set_value_cansleep(imx462->pwdn_gpio, 1);
	/* > 8192 INCK cycles before the first I2C access, plus margin */
	usleep_range(20000, 25000);
	return 0;
}

static void __imx462_power_off(struct imx462 *imx462)
{
	if (!IS_ERR(imx462->pwdn_gpio))
		gpiod_set_value_cansleep(imx462->pwdn_gpio, 0);
	clk_disable_unprepare(imx462->xvclk);
	if (!IS_ERR(imx462->reset_gpio))
		gpiod_set_value_cansleep(imx462->reset_gpio, 0);
	if (!IS_ERR_OR_NULL(imx462->pins_sleep))
		pinctrl_select_state(imx462->pinctrl, imx462->pins_sleep);
	regulator_bulk_disable(NUM_SUPPLIES, imx462->supplies);
}

static int imx462_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));

	return __imx462_power_on(to_imx462(sd));
}

static int imx462_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));

	__imx462_power_off(to_imx462(sd));
	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int imx462_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx462 *imx462 = to_imx462(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_get_try_format(sd, FH_PAD(fh), 0);

	mutex_lock(&imx462->mutex);
	try_fmt->width = supported_modes[0].width;
	try_fmt->height = supported_modes[0].height;
	try_fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	try_fmt->field = V4L2_FIELD_NONE;
	mutex_unlock(&imx462->mutex);
	return 0;
}

static const struct v4l2_subdev_internal_ops imx462_internal_ops = {
	.open = imx462_open,
};
#endif

static const struct dev_pm_ops imx462_pm_ops = {
	SET_RUNTIME_PM_OPS(imx462_runtime_suspend, imx462_runtime_resume, NULL)
};

static const struct v4l2_subdev_core_ops imx462_core_ops = {
	.s_power = imx462_s_power,
	.ioctl = imx462_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = imx462_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops imx462_video_ops = {
	.s_stream = imx462_s_stream,
	.g_frame_interval = imx462_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx462_pad_ops = {
	.enum_mbus_code = imx462_enum_mbus_code,
	.enum_frame_size = imx462_enum_frame_sizes,
	.enum_frame_interval = imx462_enum_frame_interval,
	.get_fmt = imx462_get_fmt,
	.set_fmt = imx462_set_fmt,
	.get_selection = imx462_get_selection,
	.get_mbus_config = imx462_g_mbus_config,
};

static const struct v4l2_subdev_ops imx462_subdev_ops = {
	.core = &imx462_core_ops,
	.video = &imx462_video_ops,
	.pad = &imx462_pad_ops,
};

static int imx462_write_shs1(struct imx462 *imx462, u32 exposure)
{
	return imx462_write_le(imx462->client, REG_SHS1_L,
			       imx462->cur_vts - exposure - 1, 3);
}

static int imx462_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx462 *imx462 = container_of(ctrl->handler, struct imx462,
					     ctrl_handler);
	struct i2c_client *client = imx462->client;
	int ret = 0;
	u8 reg;

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* exposure must stay below the new frame length */
		s64 max = imx462->cur_mode->height + ctrl->val - EXPOSURE_OFFSET;

		imx462->cur_vts = imx462->cur_mode->height + ctrl->val;
		__v4l2_ctrl_modify_range(imx462->exposure,
					 imx462->exposure->minimum, max,
					 imx462->exposure->step,
					 min_t(s64, imx462->exposure->default_value, max));
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = imx462_write_shs1(imx462, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx462_write_reg(client, REG_GAIN, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		/* SHS1 counts from the end of the frame: rewrite it with VMAX */
		ret = imx462_write_reg(client, REG_REGHOLD, 1);
		ret |= imx462_write_le(client, REG_VMAX_L, imx462->cur_vts, 3);
		ret |= imx462_write_shs1(imx462, imx462->exposure->val);
		ret |= imx462_write_reg(client, REG_REGHOLD, 0);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		reg = imx462->cur_mode->winmode;
		if (imx462->h_flip->val)
			reg |= HREVERSE;
		if (imx462->v_flip->val)
			reg |= VREVERSE;
		ret = imx462_write_reg(client, REG_CTRL_07, reg);
		break;
	case V4L2_CID_TEST_PATTERN:
		if (ctrl->val) {
			ret = imx462_write_reg(client, REG_BLKLEVEL_L, 0);
			ret |= imx462_write_reg(client, REG_PGCTRL,
						PGCTRL_REGEN | PGCTRL_THRU |
						PGCTRL_MODE(ctrl->val));
		} else {
			ret = imx462_write_reg(client, REG_PGCTRL, 0);
			ret |= imx462_write_reg(client, REG_BLKLEVEL_L, BLACK_LEVEL);
		}
		break;
	default:
		break;
	}

	pm_runtime_put(&client->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx462_ctrl_ops = {
	.s_ctrl = imx462_set_ctrl,
};

static int imx462_initialize_controls(struct imx462 *imx462)
{
	const struct imx462_mode *mode = imx462->cur_mode;
	struct v4l2_ctrl_handler *handler = &imx462->ctrl_handler;
	u32 hblank = mode->hts - mode->width;
	u32 vblank = mode->vts - mode->height;
	int ret;

	ret = v4l2_ctrl_handler_init(handler, 10);
	if (ret)
		return ret;
	handler->lock = &imx462->mutex;

	imx462->link_freq = v4l2_ctrl_new_int_menu(handler, NULL,
		V4L2_CID_LINK_FREQ, 1, 0,
		imx462->nlanes == 4 ? link_freq_4l : link_freq_2l);
	if (imx462->link_freq)
		imx462->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx462->pixel_rate = v4l2_ctrl_new_std(handler, NULL,
		V4L2_CID_PIXEL_RATE, 0, PIXEL_RATE, 1, PIXEL_RATE);

	imx462->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
					   hblank, hblank, 1, hblank);
	if (imx462->hblank)
		imx462->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx462->cur_vts = mode->vts;
	imx462->vblank = v4l2_ctrl_new_std(handler, &imx462_ctrl_ops,
		V4L2_CID_VBLANK, vblank, VMAX_MAX - mode->height, 1, vblank);

	imx462->exposure = v4l2_ctrl_new_std(handler, &imx462_ctrl_ops,
		V4L2_CID_EXPOSURE, EXPOSURE_MIN, mode->vts - EXPOSURE_OFFSET,
		1, mode->exp_def);

	imx462->anal_gain = v4l2_ctrl_new_std(handler, &imx462_ctrl_ops,
		V4L2_CID_ANALOGUE_GAIN, 0, GAIN_MAX, 1, 0);

	imx462->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
		&imx462_ctrl_ops, V4L2_CID_TEST_PATTERN,
		ARRAY_SIZE(imx462_test_pattern_menu) - 1, 0, 0,
		imx462_test_pattern_menu);

	imx462->h_flip = v4l2_ctrl_new_std(handler, &imx462_ctrl_ops,
					   V4L2_CID_HFLIP, 0, 1, 1, 0);
	imx462->v_flip = v4l2_ctrl_new_std(handler, &imx462_ctrl_ops,
					   V4L2_CID_VFLIP, 0, 1, 1, 0);

	if (handler->error) {
		ret = handler->error;
		dev_err(&imx462->client->dev, "failed to init controls (%d)\n", ret);
		v4l2_ctrl_handler_free(handler);
		return ret;
	}
	imx462->subdev.ctrl_handler = handler;
	return 0;
}

/*
 * The IMX290 family has no documented ID register. Rockchip's imx327.c
 * reads 0xb2 at 0x301e; the IMX462 value is not known yet, so a different
 * value only gives a warning. A failed read (no I2C answer) is an error.
 */
static int imx462_check_sensor_id(struct imx462 *imx462)
{
	struct device *dev = &imx462->client->dev;
	u8 id = 0;
	int ret = imx462_read_reg(imx462->client, REG_CHIP_ID, &id);

	if (ret) {
		dev_err(dev, "no answer at I2C address 0x%02x (%d)\n",
			imx462->client->addr, ret);
		return ret;
	}
	if (id != CHIP_ID_IMX327)
		dev_warn(dev, "register 0x301e = 0x%02x (imx327: 0x%02x), continuing\n",
			 id, CHIP_ID_IMX327);
	else
		dev_info(dev, "sensor answers, 0x301e = 0x%02x\n", id);
	return 0;
}

static int imx462_parse_clock(struct imx462 *imx462)
{
	struct device *dev = &imx462->client->dev;
	unsigned long rate;

	imx462->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx462->xvclk)) {
		dev_err(dev, "failed to get xvclk\n");
		return PTR_ERR(imx462->xvclk);
	}
	/* a SoC clock may be settable; a fixed-clock (module crystal) is not */
	rate = clk_get_rate(imx462->xvclk);
	if (rate != 37125000 && rate != 74250000) {
		clk_set_rate(imx462->xvclk, 37125000);
		rate = clk_get_rate(imx462->xvclk);
	}
	if (rate == 37125000) {
		imx462->clk_idx = CLK_37_125;
	} else if (rate == 74250000) {
		imx462->clk_idx = CLK_74_25;
	} else {
		dev_err(dev, "xvclk %lu Hz: needs 37125000 or 74250000\n", rate);
		return -EINVAL;
	}
	dev_info(dev, "xvclk %lu Hz\n", rate);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int imx462_probe(struct i2c_client *client)
#else
static int imx462_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#endif
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct device_node *endpoint;
	struct imx462 *imx462;
	struct v4l2_subdev *sd;
	char facing[2];
	unsigned int i;
	int ret;

	dev_info(dev, "driver version: %02x.%02x.%02x\n", DRIVER_VERSION >> 16,
		 (DRIVER_VERSION & 0xff00) >> 8, DRIVER_VERSION & 0xff);

	imx462 = devm_kzalloc(dev, sizeof(*imx462), GFP_KERNEL);
	if (!imx462)
		return -ENOMEM;
	imx462->client = client;
	imx462->hcg = hcg;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &imx462->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &imx462->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &imx462->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &imx462->len_name);
	if (ret) {
		dev_err(dev, "could not get module information\n");
		return -EINVAL;
	}

	endpoint = of_graph_get_next_endpoint(node, NULL);
	if (!endpoint) {
		dev_err(dev, "failed to get endpoint\n");
		return -EINVAL;
	}
	imx462->bus_cfg.bus_type = V4L2_MBUS_CSI2_DPHY;
	ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(endpoint),
					 &imx462->bus_cfg);
	of_node_put(endpoint);
	if (ret) {
		dev_err(dev, "failed to parse endpoint (%d)\n", ret);
		return ret;
	}
	imx462->nlanes = imx462->bus_cfg.bus.mipi_csi2.num_data_lanes;
	if (imx462->nlanes != 2 && imx462->nlanes != 4) {
		dev_err(dev, "data-lanes: %u lanes, needs 2 or 4\n", imx462->nlanes);
		return -EINVAL;
	}
	imx462->cur_mode = &supported_modes[0];

	ret = imx462_parse_clock(imx462);
	if (ret)
		return ret;

	imx462->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(imx462->reset_gpio))
		dev_info(dev, "no reset-gpios\n");
	imx462->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(imx462->pwdn_gpio))
		dev_info(dev, "no pwdn-gpios\n");

	for (i = 0; i < NUM_SUPPLIES; i++)
		imx462->supplies[i].supply = imx462_supply_names[i];
	ret = devm_regulator_bulk_get(dev, NUM_SUPPLIES, imx462->supplies);
	if (ret) {
		dev_err(dev, "failed to get regulators\n");
		return ret;
	}

	imx462->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(imx462->pinctrl)) {
		imx462->pins_default = pinctrl_lookup_state(imx462->pinctrl,
					OF_CAMERA_PINCTRL_STATE_DEFAULT);
		imx462->pins_sleep = pinctrl_lookup_state(imx462->pinctrl,
					OF_CAMERA_PINCTRL_STATE_SLEEP);
	}

	mutex_init(&imx462->mutex);
	sd = &imx462->subdev;
	v4l2_i2c_subdev_init(sd, client, &imx462_subdev_ops);
	ret = imx462_initialize_controls(imx462);
	if (ret)
		goto err_destroy_mutex;

	ret = __imx462_power_on(imx462);
	if (ret)
		goto err_free_handler;
	ret = imx462_check_sensor_id(imx462);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &imx462_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	imx462->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &imx462->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	facing[0] = strcmp(imx462->module_facing, "back") == 0 ? 'b' : 'f';
	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 imx462->module_index, facing, IMX462_NAME, dev_name(sd->dev));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	ret = v4l2_async_register_subdev_sensor(sd);
#else
	ret = v4l2_async_register_subdev_sensor_common(sd);
#endif
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);
	dev_info(dev, "%u lanes, %ux%u RAW10, link %lld Hz\n", imx462->nlanes,
		 imx462->cur_mode->width, imx462->cur_mode->height,
		 imx462_mode_link_freq(imx462, imx462->cur_mode));
	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__imx462_power_off(imx462);
err_free_handler:
	v4l2_ctrl_handler_free(&imx462->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&imx462->mutex);
	return ret;
}

static void __imx462_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx462 *imx462 = to_imx462(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&imx462->ctrl_handler);
	mutex_destroy(&imx462->mutex);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__imx462_power_off(imx462);
	pm_runtime_set_suspended(&client->dev);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void imx462_remove(struct i2c_client *client)
{
	__imx462_remove(client);
}
#else
static int imx462_remove(struct i2c_client *client)
{
	__imx462_remove(client);
	return 0;
}
#endif

static const struct of_device_id imx462_of_match[] = {
	{ .compatible = "sony,imx462" },
	{ },
};
MODULE_DEVICE_TABLE(of, imx462_of_match);

static const struct i2c_device_id imx462_match_id[] = {
	{ "sony,imx462", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, imx462_match_id);

static struct i2c_driver imx462_i2c_driver = {
	.driver = {
		.name = IMX462_NAME,
		.pm = &imx462_pm_ops,
		.of_match_table = imx462_of_match,
	},
	.probe = imx462_probe,
	.remove = imx462_remove,
	.id_table = imx462_match_id,
};

module_i2c_driver(imx462_i2c_driver);

MODULE_DESCRIPTION("Sony IMX462 sensor driver for Rockchip ISP");
MODULE_LICENSE("GPL v2");
