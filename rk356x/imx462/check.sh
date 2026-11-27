#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Bring-up check for the IMX462 on a Radxa ZERO 3W/3E. Run on the board after
# install.sh and a reboot:  sudo ./check.sh   (writes imx462-check.log)
# Each step prints PASS / FAIL; send imx462-check.log when asking for help.

LOG=imx462-check.log
FRAMES=120
exec > >(tee "$LOG") 2>&1

step() { echo; echo "== $*"; }
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; }

step "system"
uname -a
cat /etc/os-release 2>/dev/null | head -2
grep -o "radxa-zero3-imx462[^ ]*" /boot/extlinux/extlinux.conf 2>/dev/null | head -1 \
	&& pass "overlay in extlinux.conf" || fail "overlay not in extlinux.conf (install.sh / rsetup)"

step "driver loaded and probed"
lsmod | grep -q "^imx462" && pass "module loaded" || fail "imx462 module not loaded"
dmesg | grep -iE "imx462|csi2|dphy|rkisp|rkcif" | tail -40
if dmesg | grep -q "imx462.*lanes.*RAW10"; then
	pass "sensor probed"
elif dmesg | grep -q "imx462.*no answer"; then
	fail "no I2C answer: cable direction, power enable (GPIO3_C6), or not an IMX462 at 0x1a"
else
	fail "no probe message from imx462"
fi

step "I2C bus 2 (0x1a: UU = driver owns it)"
if command -v i2cdetect >/dev/null; then
	i2cdetect -y 2 2>&1 | sed -n '1,9p'
else
	echo "no i2cdetect (apt install i2c-tools)"
fi

step "media graph"
MDEV=
for m in /dev/media*; do
	media-ctl -d "$m" -p 2>/dev/null | grep -q imx462 && MDEV=$m
done
if [ -n "$MDEV" ]; then
	pass "imx462 in $MDEV"
	media-ctl -d "$MDEV" -p 2>/dev/null | grep -E "entity|imx462|dphy|rkisp|->|<-" | head -40
else
	fail "imx462 not in any media graph (overlay / dphy / rkisp links)"
fi

step "video nodes"
v4l2-ctl --list-devices 2>/dev/null
VDEV=
for v in /sys/class/video4linux/video*; do
	[ "$(cat "$v/name" 2>/dev/null)" = rkisp_mainpath ] && VDEV=/dev/$(basename "$v") && break
done
[ -n "$VDEV" ] && pass "rkisp_mainpath = $VDEV" || fail "no rkisp_mainpath node"

step "IQ file and 3A"
ls -la /etc/iqfiles/ 2>/dev/null | grep -iE "imx462|imx327|imx290" || echo "(no imx462/imx327/imx290 IQ file)"
[ -f /etc/iqfiles/imx462_WS-IMX462-A_default.json ] && pass "IQ file present" \
	|| fail "no /etc/iqfiles/imx462_WS-IMX462-A_default.json (picture will be unprocessed)"
pgrep -a rkaiq || echo "(rkaiq_3A_server not running)"

if [ -n "$VDEV" ]; then
	step "capture $FRAMES frames 1920x1080 NV12 from $VDEV"
	OUT=/tmp/imx462.nv12
	if v4l2-ctl -d "$VDEV" --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
		--stream-mmap=4 --stream-count=$FRAMES --stream-to=$OUT --verbose 2>&1 | tail -6; then
		SIZE=$(stat -c %s $OUT 2>/dev/null || echo 0)
		WANT=$((1920 * 1080 * 3 / 2 * FRAMES))
		[ "$SIZE" -ge "$WANT" ] && pass "$FRAMES frames ($SIZE bytes) in $OUT" \
			|| fail "got $SIZE of $WANT bytes"
		echo "look at one frame on a PC: ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i imx462.nv12 -frames 1 f.png"
	else
		fail "capture failed (see dmesg for csi / rkisp errors)"
		dmesg | tail -20
	fi
fi

echo
echo "log written to $LOG"
