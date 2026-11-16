#!/bin/sh
# Assembles project/cfg/BoardConfig_IPC/overlay/overlay-rkfpv in the Luckfox
# SDK from rkfpv build outputs, and adds it to the Mini SD board config.
# Run after `make` here and after luckfox/build-deps.sh.
#
#   DRONE_KEY=/path/to/drone.key luckfox/mkoverlay.sh [SDK]
#
# The wfb-ng key is never kept in this repo. Default: ../openipc/keys/drone.key
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK=${1:-${SDK:-$(cd "$HERE/.." && pwd)/luckfox-pico}}
BOARD=$SDK/project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk
OV=$SDK/project/cfg/BoardConfig_IPC/overlay/overlay-rkfpv
WFB=$HERE/luckfox/out/wfb
KO=$HERE/luckfox/out/88XXau.ko
KEY=${DRONE_KEY:-$(cd "$HERE/.." && pwd)/openipc/keys/drone.key}

need() { [ -e "$1" ] || { echo "missing: $1" >&2; exit 1; }; }
need "$HERE/build/rkfpv"
need "$HERE/build/mavbridge"
need "$KEY"
need "$WFB/wfb_tx"
need "$WFB/wfb_rx"
need "$KO"

rm -rf "$OV"
install -d "$OV/usr/bin" "$OV/etc/init.d" "$OV/oem/usr/ko"

install -m 755 "$HERE/build/rkfpv" "$HERE/build/mavbridge" "$OV/usr/bin/"
install -m 755 "$HERE/files/wfb.sh" "$OV/usr/bin/"
install -m 755 "$WFB/wfb_tx" "$WFB/wfb_rx" "$OV/usr/bin/"
[ -e "$WFB/wfb_tx_cmd" ] && install -m 755 "$WFB/wfb_tx_cmd" "$OV/usr/bin/"
install -m 755 "$HERE/files/S21appinit" "$HERE/files/S99fpv" "$OV/etc/init.d/"
install -m 644 "$HERE/files/rkfpv.conf" "$HERE/files/wfb.yaml" "$OV/etc/"
install -m 600 "$KEY" "$OV/etc/drone.key"
install -m 644 "$KO" "$OV/oem/usr/ko/"

# hook the overlay into the board config once
if ! grep -q 'overlay-rkfpv' "$BOARD"; then
	sed -i 's/^export RK_POST_OVERLAY="\(.*\)"/export RK_POST_OVERLAY="\1 overlay-rkfpv"/' "$BOARD"
fi
grep '^export RK_POST_OVERLAY' "$BOARD"
echo "overlay ready: $OV"
find "$OV" -type f | sed "s|$OV||" | sort
