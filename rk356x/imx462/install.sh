#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Build and install the IMX462 driver and overlay on a Radxa ZERO 3W/3E
# running Radxa OS (Rockchip BSP kernel). Run on the board:
#   sudo ./install.sh                 module + overlay, then reboot
#   sudo ./install.sh --iq FILE.json  also install FILE as the IQ file
#   sudo ./install.sh --uninstall     remove module and overlay
set -e

cd "$(dirname "$0")"
KVER=$(uname -r)
KDIR=/lib/modules/$KVER/build
OVERLAY=radxa-zero3-imx462
DTBO_DIR=/boot/dtbo
IQ_NAME=imx462_WS-IMX462-A_default.json
IQ_DIR=/etc/iqfiles
MOD_DIR=/lib/modules/$KVER/extra

die() { echo "install.sh: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo)"

update_boot() {
	if command -v u-boot-update >/dev/null 2>&1; then
		u-boot-update
	else
		echo "warning: no u-boot-update; enable the overlay with rsetup (Overlays) instead"
	fi
}

if [ "$1" = "--uninstall" ]; then
	rm -f "$MOD_DIR/imx462.ko" "$DTBO_DIR/$OVERLAY.dtbo"
	depmod -a
	update_boot
	echo "removed; reboot to finish"
	exit 0
fi

IQ_SRC=
if [ "$1" = "--iq" ]; then
	[ -f "$2" ] || die "--iq needs an existing .json file"
	IQ_SRC=$2
fi

[ -d "$KDIR" ] || die "no kernel headers in $KDIR: apt install linux-headers-$KVER (or Radxa's linux-headers package for this kernel)"
command -v dtc >/dev/null 2>&1 || die "no dtc: apt install device-tree-compiler"

echo "== building for $KVER"
make KDIR="$KDIR" module dtbo

echo "== installing module to $MOD_DIR"
mkdir -p "$MOD_DIR"
install -m 644 imx462.ko "$MOD_DIR/"
depmod -a

echo "== installing overlay to $DTBO_DIR"
mkdir -p "$DTBO_DIR"
install -m 644 "$OVERLAY.dtbo" "$DTBO_DIR/"
# only one camera can own csi2_dphy0: list other enabled camera overlays
others=$(ls "$DTBO_DIR"/*camera*.dtbo "$DTBO_DIR"/*imx*.dtbo "$DTBO_DIR"/*ov5647*.dtbo 2>/dev/null | grep -v "$OVERLAY" || true)
if [ -n "$others" ]; then
	echo "warning: other camera overlays are enabled; disable them in rsetup:"
	echo "$others" | sed 's/^/    /'
fi
update_boot
if grep -rqs "$OVERLAY" /boot/extlinux/extlinux.conf; then
	echo "overlay listed in /boot/extlinux/extlinux.conf"
else
	echo "warning: $OVERLAY not in /boot/extlinux/extlinux.conf; enable it in rsetup (Overlays)"
fi

if [ -n "$IQ_SRC" ]; then
	mkdir -p "$IQ_DIR"
	install -m 644 "$IQ_SRC" "$IQ_DIR/$IQ_NAME"
	echo "IQ file installed as $IQ_DIR/$IQ_NAME"
elif [ ! -f "$IQ_DIR/$IQ_NAME" ]; then
	echo "note: no $IQ_DIR/$IQ_NAME yet: rkaiq will not run 3A (dark or green picture)."
	echo "      see README: copy an imx327/imx462 ISP21 json, then ./install.sh --iq FILE"
fi

echo "done: reboot, then run ./check.sh"
