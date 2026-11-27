# IMX462 camera on Radxa ZERO 3W / 3E (RK3566)

A Rockchip-style kernel driver, a device-tree overlay and two scripts. Together they bring a Raspberry Pi style **Sony IMX462** module through the RK3566 ISP (rkisp + rkaiq) on Radxa OS. The target module is the Waveshare IMX462 IR-CUT Camera (A): plain RAW sensor, 2-lane MIPI.

**Status: compile-tested only.**
- The driver builds without warnings against Rockchip's 6.1 (arm64) and 5.10 BSP kernels.
- The overlay applies to the ZERO 3W base device tree with `fdtoverlay`.
- The register values were checked against the mainline `imx290.c` IMX462 settings by a script.
- It has not run on hardware yet. Use `check.sh` and report what it says.

Everything in this folder is **GPL-2.0** (see `LICENSE`), unlike the rest of rkfpv (MIT):
- the driver derives from Rockchip's `imx327.c` and the mainline `imx290.c`;
- the overlay derives from Radxa's `radxa-zero3-rpi-camera-v2.dts`.

## Why a new driver
- **Rockchip's `imx327.c` (same sensor family) only has 4-lane tables.** Pi style modules are 2-lane: 891 Mbit/s per lane for 1080p.
- **The mainline `imx290.c` has 2-lane and IMX462 support,** but it lacks the Rockchip `rkmodule` interface that rkaiq needs.
- **`imx462.c` combines them:** it keeps Rockchip's controls and ioctls, and uses the mainline register sequence, with clock, lane and D-PHY timing tables for 2 or 4 lanes and a 37.125 or 74.25 MHz crystal.

| Mode | Link (2 lanes) | Max fps | Notes |
|---|---|---|---|
| 1920×1080 RAW10 | 445.5 MHz | 60 | full sensor |
| 1280×720 RAW10 | 297 MHz | 60 | a window of the sensor: **narrower view** than 1080p. For a wide 720p picture, use 1080p and let the ISP scale |

- **Lower frame rates** come from vblank (VMAX), which rkaiq sets.
- **`hcg=1`** (module parameter) starts in high conversion gain, which is better in the dark. rkaiq can also switch it with `RKMODULE_SET_CONVERSION_GAIN`.

## Hardware
- **Connector:** ZERO 3W/3E 22-pin, 0.5 mm, Raspberry Pi layout. A module with a 15-pin socket needs a **15→22 pin** cable. Mind the contact side.
- **Power enable:** connector pin 17 = GPIO3_C6. The overlay keeps it high, like Radxa's Pi camera overlays.
- **I2C:** bus 2 (i2c2m1), sensor at **0x1a**.
- **Crystal:** the overlay assumes **37.125 MHz** on the module. If the crystal reads 74.25, change `clock-frequency` in the `.dts` to `74250000`.
- **IR-cut filter:** not handled. If the module switches it from a GPIO on the connector, that needs an addition to the overlay.

## Install (on the board)
Use Radxa OS with the Rockchip BSP kernel. Cameras on the ZERO 3W are reported to work on 5.10.160-26.

```sh
sudo apt install linux-headers-$(uname -r) device-tree-compiler i2c-tools v4l-utils
sudo ./install.sh                  # builds against /lib/modules/$(uname -r)/build
sudo reboot
sudo ./check.sh                    # PASS/FAIL per step, writes imx462-check.log
```

- **Disable other camera overlays.** Only one camera can own `csi2_dphy0`; `install.sh` lists any others it finds.
- **Header package name:** if `linux-headers-$(uname -r)` doesn't exist, find Radxa's name for it with `apt search linux-headers`.
- **Uninstall:** `sudo ./install.sh --uninstall`.

## IQ file (picture quality)
rkaiq loads `/etc/iqfiles/<sensor>_<module>_<lens>.json`. For this overlay that is:

```
/etc/iqfiles/imx462_WS-IMX462-A_default.json
```

Without it, frames still arrive, but there is no auto exposure or white balance, and the picture looks dark or green.

1. **Look for one on the image:** `ls /etc/iqfiles | grep -iE "imx462|imx327|imx290"`.
2. **Install an IMX327 or IMX462 file for the RK356x (ISP21)** with `sudo ./install.sh --iq that.json`. It is copied under the name above. IMX327 and IMX462 share the same pixel design, so colours will be roughly right, but untuned.
3. **Don't use IQ files for other ISP versions.** RV1106 (ISP32) and RK3588 (ISP30) files are a different format.
4. **Proper tuning** needs Rockchip's RKISP2.x Tuner and a colour chart.

## If it doesn't work

| check.sh says | Usually |
|---|---|
| no I2C answer | cable reversed, 15/22 adapter wrong, power enable not high, or the module isn't at 0x1a |
| probed, but no frames | crystal frequency wrong in the overlay (37.125 vs 74.25), or wrong lane count |
| frames, dark or green | no IQ file, or rkaiq not running |
| `0x301e = 0x..` warning | harmless: that register only identifies the IMX327, and the IMX462 value isn't known yet. The value is in the log for later |

## Files
- `imx462.c`: the driver.
- `Makefile`: out-of-tree build (`KDIR=` for cross builds).
- `radxa-zero3-imx462.dts`: the overlay. It needs no includes, so plain `dtc` builds it.
- `install.sh`: build, install the module and dtbo, run `u-boot-update`, optionally install an IQ file.
- `check.sh`: bring-up checklist with a log file.
