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
KO=$HERE/luckfox/out/88XXau.ko	# all three drivers come from build-deps.sh
KEY=${DRONE_KEY:-$(cd "$HERE/.." && pwd)/openipc/keys/drone.key}

need() { [ -e "$1" ] || { echo "missing: $1" >&2; exit 1; }; }
TRIM_SERVICES="S49ntp S50telnet S41dhcpcd S99python S99hciinit S60micinit S99_auto_reboot S80dnsmasq"
TRIM_MODULES="rknpu snd-soc-rv1106 motor rve imx415 os04a10 sc4336 sc530ai gc2053 sc200ai sc401ai sc450ai techpoint mis5001 mia1321"
need "$HERE/build/rkfpv"
need "$HERE/build/mavbridge"
need "$KEY"
need "$WFB/wfb_tx"
need "$WFB/wfb_rx"
need "$KO"

rm -rf "$OV"
install -d "$OV/usr/bin" "$OV/etc/init.d" "$OV/oem/usr/ko"

install -m 755 "$HERE/build/rkfpv" "$HERE/build/mavbridge" "$OV/usr/bin/"
install -m 755 "$HERE/files/wfb.sh" "$HERE/files/fpv" "$OV/usr/bin/"
install -m 755 "$WFB/wfb_tx" "$WFB/wfb_rx" "$OV/usr/bin/"
[ -e "$WFB/wfb_tx_cmd" ] && install -m 755 "$WFB/wfb_tx_cmd" "$OV/usr/bin/"
install -m 755 "$HERE/files/S21appinit" "$HERE/files/S99fpv" "$OV/etc/init.d/"
install -m 644 "$HERE/files/rkfpv.conf" "$HERE/files/wfb.yaml" "$OV/etc/"
# pristine copies for `fpv reset`
install -d "$OV/etc/fpv/defaults"
install -m 644 "$HERE/files/rkfpv.conf" "$HERE/files/wfb.yaml" "$OV/etc/fpv/defaults/"
install -m 600 "$KEY" "$OV/etc/drone.key"
install -m 644 "$HERE"/luckfox/out/*.ko "$OV/oem/usr/ko/"

# ---- RAM: the Mini has 64 MB, 24 MB of it CMA for video, ~2 MB free in FPV.
# Stock services an air unit does not need (no internet, usb0 has a static
# address, no Bluetooth/mic/python). The overlay replaces each with a stub.
for s in $TRIM_SERVICES; do
	printf '#!/bin/sh\n# disabled by rkfpv to save RAM (luckfox/mkoverlay.sh TRIM_SERVICES)\nexit 0\n' > "$OV/etc/init.d/$s"
	chmod 755 "$OV/etc/init.d/$s"
done
# Kernel modules nobody uses here: NPU, audio, motor, RVE and the other
# camera sensors. Same script as the SDK's, minus those insmod lines.
INSMOD=$SDK/sysdrv/drv_ko/insmod_ko.sh
need "$INSMOD"
pat=$(echo $TRIM_MODULES | sed 's/ /\\|/g')
sed "/^__insmod \($pat\)\.ko/d" "$INSMOD" > "$OV/oem/usr/ko/insmod_ko.sh"
chmod 755 "$OV/oem/usr/ko/insmod_ko.sh"
for m in $TRIM_MODULES; do
	grep -q "^__insmod $m\.ko" "$OV/oem/usr/ko/insmod_ko.sh" && { echo "insmod trim failed: $m" >&2; exit 1; }
done
grep -q '^__insmod sc3336\.ko' "$OV/oem/usr/ko/insmod_ko.sh" || { echo "insmod trim removed sc3336" >&2; exit 1; }

# hook the overlay into the board config once
if ! grep -q 'overlay-rkfpv' "$BOARD"; then
	sed -i 's/^export RK_POST_OVERLAY="\(.*\)"/export RK_POST_OVERLAY="\1 overlay-rkfpv"/' "$BOARD"
fi
grep '^export RK_POST_OVERLAY' "$BOARD"
echo "overlay ready: $OV"
find "$OV" -type f | sed "s|$OV||" | sort
