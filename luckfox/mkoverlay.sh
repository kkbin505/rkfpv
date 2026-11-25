#!/bin/sh
# Assembles project/cfg/BoardConfig_IPC/overlay/overlay-rkfpv in the Luckfox
# SDK from rkfpv build outputs, and adds it to the board config.
# Run after `make` here and after luckfox/build-deps.sh.
#
#   DRONE_KEY=/path/to/drone.key [BOARD=mini|pro] luckfox/mkoverlay.sh [SDK]
#
#   mini (default): Pico Mini, RV1103, boots from the TF card, recordings on
#                   the card's "dvr" partition
#   pro:            Pico Pro/Max, RV1106, boots from SPI NAND, recordings on
#                   a TF card in the slot (ext4, `fpv dvr format --yes`)
#
# The SDK builds one board at a time: `./build.sh lunch` must match BOARD.
#
# The wfb-ng key is never kept in this repo. Default: ../openipc/keys/drone.key
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK=${1:-${SDK:-$(cd "$HERE/.." && pwd)/luckfox-pico}}
BOARD=${BOARD:-mini}
case "$BOARD" in
mini) BOARD_CFG=$SDK/project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk ;;
pro)  BOARD_CFG=$SDK/project/cfg/BoardConfig_IPC/BoardConfig-SPI_NAND-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk ;;
*)    echo "BOARD must be mini or pro" >&2; exit 1 ;;
esac
OV=$SDK/project/cfg/BoardConfig_IPC/overlay/overlay-rkfpv
WFB=$HERE/luckfox/out/wfb
KO=$HERE/luckfox/out/88XXau.ko	# all three drivers come from build-deps.sh
KEY=${DRONE_KEY:-$(cd "$HERE/.." && pwd)/openipc/keys/drone.key}

need() { [ -e "$1" ] || { echo "missing: $1" >&2; exit 1; }; }
# S99luckfoxconfigload / S99luckfoxcustomoverlay: bash scripts that dump the
# device tree to text in /tmp (RAM) and run dtc while rkfpv is starting; the
# board hung there at boot. Pins come from our DTS, so they have nothing to do.
TRIM_SERVICES="S49ntp S50telnet S41dhcpcd S99python S99hciinit S60micinit S99_auto_reboot S80dnsmasq S99luckfoxconfigload S99luckfoxcustomoverlay"
TRIM_MODULES="rknpu snd-soc-rv1106 motor rve imx415 os04a10 sc4336 sc530ai gc2053 sc200ai sc401ai sc450ai techpoint mis5001 mia1321"
need "$HERE/build/rkfpv"
need "$HERE/build/mavbridge"
need "$HERE/build/fpvd"
need "$HERE/build/dvrhttpd"
need "$HERE/build/bblog"
need "$HERE/build/bbsend"
need "$KEY"
need "$WFB/wfb_tx"
need "$WFB/wfb_rx"
need "$KO"

rm -rf "$OV"
install -d "$OV/usr/bin" "$OV/etc/init.d" "$OV/oem/usr/ko" "$OV/mnt/dvr"

install -m 755 "$HERE/build/rkfpv" "$HERE/build/mavbridge" "$HERE/build/fpvd" "$HERE/build/dvrhttpd" "$HERE/build/bblog" "$HERE/build/bbsend" "$OV/usr/bin/"
install -m 755 "$HERE/files/wfb.sh" "$HERE/files/fpv" "$OV/usr/bin/"
install -m 755 "$WFB/wfb_tx" "$WFB/wfb_rx" "$OV/usr/bin/"
[ -e "$WFB/wfb_tx_cmd" ] && install -m 755 "$WFB/wfb_tx_cmd" "$OV/usr/bin/"
install -m 755 "$HERE/files/S21appinit" "$HERE/files/S99fpv" "$OV/etc/init.d/"
install -m 644 "$HERE/files/rkfpv.conf" "$HERE/files/wfb.yaml" "$HERE/files/motd" "$OV/etc/"
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

if [ "$BOARD" = mini ]; then
	# DVR partition: fixed-size rootfs, the rest of the TF card for recordings
	# (formatted on first boot by S99fpv). Changes the card layout: reflash.
	if grep -q '256M(userdata),-(rootfs)"' "$BOARD_CFG"; then
		sed -i 's/256M(userdata),-(rootfs)"/256M(userdata),512M(rootfs),-(dvr)"/' "$BOARD_CFG"
	fi
	# The SDK packer (lf_blkenvpackage) wants an image for every partition. The
	# dvr one is 64 KB of zeros: it keeps sd_update.img small (the partition
	# grows to the end of the card) and clears any old filesystem header there,
	# so S99fpv formats it on first boot.
	install -d "$SDK/output/image"
	head -c 65536 /dev/zero > "$SDK/output/image/dvr.img"
else
	# Pro on SPI NAND (256 MB):
	# - no separate oem partition: the SDK packs a separate oem image from
	#   its own staging dir and empties rootfs/oem, which would drop our
	#   /oem/usr/ko drivers and insmod_ko.sh. Merged, the layout is the
	#   Mini's (oem inside rootfs).
	# - CMA 48M of 128 MB: link 720p + a 1080p DVR encoder, ~75 MB for Linux
	# - no SDIO WiFi/BT stack (that is for the onboard-module variant; our
	#   WiFi card is on USB)
	# Recordings go to the TF card slot, nothing to partition here.
	sed -i \
		-e 's/^export RK_BUILD_APP_TO_OEM_PARTITION=y/# rkfpv: oem merged into rootfs\n# export RK_BUILD_APP_TO_OEM_PARTITION=y/' \
		-e 's/4M(boot),30M(oem),10M(userdata),210M(rootfs)"/4M(boot),10M(userdata),240M(rootfs)"/' \
		-e 's/^export RK_PARTITION_FS_TYPE_CFG=rootfs@IGNORE@ubifs,oem@\/oem@ubifs,userdata@\/userdata@ubifs/export RK_PARTITION_FS_TYPE_CFG=rootfs@IGNORE@ubifs,userdata@\/userdata@ubifs/' \
		-e 's/^export RK_BOOTARGS_CMA_SIZE="66M"/export RK_BOOTARGS_CMA_SIZE="48M"/' \
		-e 's/^export RK_ENABLE_WIFI=y/# export RK_ENABLE_WIFI=y/' \
		-e 's/ overlay-luckfox-wifibt-firmware"/"/' \
		"$BOARD_CFG"
	grep -q '^export RK_BUILD_APP_TO_OEM_PARTITION=y' "$BOARD_CFG" && { echo "oem partition still on" >&2; exit 1; }
	grep -q '(oem)' "$BOARD_CFG" && { echo "oem partition still in the table" >&2; exit 1; }
	grep -E '^export (RK_PARTITION_CMD_IN_ENV|RK_PARTITION_FS_TYPE_CFG|RK_BOOTARGS_CMA_SIZE)' "$BOARD_CFG"
fi
grep '^export RK_PARTITION_CMD_IN_ENV' "$BOARD_CFG"

# hook the overlay into the board config once
if ! grep -q 'overlay-rkfpv' "$BOARD_CFG"; then
	sed -i 's/^export RK_POST_OVERLAY="\(.*\)"/export RK_POST_OVERLAY="\1 overlay-rkfpv"/' "$BOARD_CFG"
fi
grep '^export RK_POST_OVERLAY' "$BOARD_CFG"
echo "overlay ready ($BOARD): $OV"
find "$OV" -type f | sed "s|$OV||" | sort
