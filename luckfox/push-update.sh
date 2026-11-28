#!/bin/sh
# Updates the rkfpv programs on a running board over ADB, without
# reflashing. For the Pico Pro (SPI NAND: the image cannot be edited on a
# PC like the Mini's TF card); works on the Mini too.
#
#   make && [EXTRA_OVERLAY=dir[:dir...]] luckfox/push-update.sh [adb serial]
#
# EXTRA_OVERLAY: the same directories as for mkoverlay.sh, copied over the
# board's root after rkfpv's files.
#
# Settings (/etc/rkfpv.conf, /etc/wfb.yaml) and the wfb key are left alone;
# the shipped defaults for `fpv reset` are updated. Reboot afterwards.
# On Windows, run the same `adb push` lines by hand.
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDK:-$(cd "$HERE/.." && pwd)/luckfox-pico}
STRIP=${STRIP:-$SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-strip}
ADB="adb${1:+ -s $1}"

IFS=:
for x in ${EXTRA_OVERLAY:-}; do
	[ -d "$x" ] || { echo "EXTRA_OVERLAY: not a directory: $x" >&2; exit 1; }
done
unset IFS

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for b in rkfpv fpvd mavbridge dvrhttpd bblog bbsend ftdump; do
	[ -f "$HERE/build/$b" ] || { echo "missing build/$b: run make first" >&2; exit 1; }
	"$STRIP" -o "$TMP/$b" "$HERE/build/$b"
done

$ADB get-state > /dev/null || { echo "no board on adb" >&2; exit 1; }
$ADB shell 'mount -o remount,rw / 2>/dev/null; mkdir -p /tmp/rkfpv-update'
for b in rkfpv fpvd mavbridge dvrhttpd bblog bbsend ftdump; do
	$ADB push "$TMP/$b" /tmp/rkfpv-update/ > /dev/null
done
for f in fpv fpv-plugins wfb.sh S99fpv rkfpv.conf; do
	$ADB push "$HERE/files/$f" /tmp/rkfpv-update/ > /dev/null
done
# add-on directories: one tar each, files and symlinks only, owned by root.
# `adb push` of a directory does not keep file modes on the board's adbd
# (755 arrived as 777), and copying directories would put the add-on's
# directory modes on the board's /, /etc, /usr/bin. Missing directories are
# created 755; empty ones are not copied.
n=0
IFS=:
for x in ${EXTRA_OVERLAY:-}; do
	unset IFS
	n=$((n + 1))
	(cd "$x" && find . ! -type d -print0 |
		tar --null -T - --owner=0 --group=0 --numeric-owner -cf -) > "$TMP/extra$n.tar"
	$ADB push "$TMP/extra$n.tar" /tmp/rkfpv-update/ > /dev/null
	echo "extra overlay: $x ($(tar tf "$TMP/extra$n.tar" | wc -l) files)"
	IFS=:
done
unset IFS
# replace in place only after every file arrived (a half update would not boot FPV)
$ADB shell 'set -e; cd /tmp/rkfpv-update
	for b in rkfpv fpvd mavbridge dvrhttpd bblog bbsend ftdump fpv fpv-plugins wfb.sh; do cp "$b" /usr/bin/"$b".new && chmod 755 /usr/bin/"$b".new && mv /usr/bin/"$b".new /usr/bin/"$b"; done
	cp S99fpv /etc/init.d/S99fpv.new && chmod 755 /etc/init.d/S99fpv.new && mv /etc/init.d/S99fpv.new /etc/init.d/S99fpv
	mkdir -p /etc/fpv/plugins.d /etc/fpv/defaults && cp rkfpv.conf /etc/fpv/defaults/rkfpv.conf
	for t in /tmp/rkfpv-update/extra*.tar; do if [ -f "$t" ]; then tar xf "$t" -C /; fi; done
	cd /; rm -rf /tmp/rkfpv-update; sync; echo "updated: $(cat /proc/device-tree/model 2>/dev/null | tr -d "\000")"'
echo "done: reboot the board to use the new programs (adb shell reboot)"
