#!/bin/bash
# Builds wfb-ng (wfb_tx, wfb_rx, wfb_tx_cmd) and the RTL8812AU driver
# (88XXau.ko) for the Luckfox SDK, after `./build.sh` has built the kernel
# and buildroot once. Sources are fetched at pinned versions into
# luckfox/dl/; outputs go to luckfox/out/.
#
#   luckfox/build-deps.sh [SDK]        (default SDK: ../luckfox-pico)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${1:-${SDK:-$(cd "$HERE/../.." && pwd)/luckfox-pico}}
OUT=$HERE/out
WORK=$HERE/work
DL=$HERE/dl

# Same versions OpenIPC ships, so ground stations stay compatible.
WFB_REPO=svpcom/wfb-ng
WFB_REV=ae2122c1fb70cd215bbc2e1b517b7b91f9401220
RTL_REPO=openipc/realtek-wlan
RTL_REV=ee92cdfa8c9e19321157df53471f891d69ba29a9	# branch rtl8812au_fpv

TRIPLE=arm-rockchip830-linux-uclibcgnueabihf
export PATH=$SDK/tools/linux/toolchain/$TRIPLE/bin:$PATH
CROSS=$TRIPLE-

KOBJ=$SDK/sysdrv/source/objs_kernel
KSRC=$SDK/sysdrv/source/kernel
STAGING=$(ls -d "$SDK"/sysdrv/source/buildroot/buildroot-*/output/staging 2>/dev/null | head -1)

[ -f "$KOBJ/.config" ] || { echo "kernel not built yet: $KOBJ" >&2; exit 1; }
[ -n "$STAGING" ] && [ -e "$STAGING/usr/include/sodium.h" ] && [ -e "$STAGING/usr/include/pcap.h" ] \
	|| { echo "buildroot staging lacks libsodium/libpcap: ${STAGING:-none}" >&2; exit 1; }

# fetch <repo> <rev> <dest-dir>
fetch() {
	local tgz=$DL/${1##*/}-$2.tar.gz
	mkdir -p "$DL"
	if [ ! -s "$tgz" ]; then
		curl -fL --retry 3 -o "$tgz.part" "https://github.com/$1/archive/$2.tar.gz"
		mv "$tgz.part" "$tgz"
	fi
	rm -rf "$3"; mkdir -p "$3"
	tar xzf "$tgz" -C "$3" --strip-components=1
}

mkdir -p "$OUT/wfb" "$WORK"

# ---- wfb-ng (wfb_tun skipped: it needs libevent and we don't use it) ----
fetch "$WFB_REPO" "$WFB_REV" "$WORK/wfb-ng"
# Makefile derives VERSION/COMMIT from git; pin them for a tarball.
make -C "$WORK/wfb-ng" \
	CC="${CROSS}gcc --sysroot=$STAGING" CXX="${CROSS}g++ --sysroot=$STAGING" \
	CFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard" \
	LDFLAGS="-s" VERSION=rkfpv COMMIT="${WFB_REV:0:8}" \
	wfb_tx wfb_rx wfb_tx_cmd
install -m 755 "$WORK"/wfb-ng/{wfb_tx,wfb_rx,wfb_tx_cmd} "$OUT/wfb/"

# ---- RTL8812AU (OpenIPC FPV branch + OpenIPC build fixes) ----
fetch "$RTL_REPO" "$RTL_REV" "$WORK/rtl8812au"
for p in "$HERE"/patches/rtl8812au/*.patch; do
	patch -d "$WORK/rtl8812au" -p1 -s < "$p"
done
KVER=$(make -s -C "$KSRC" O="$KOBJ" ARCH=arm kernelrelease 2>/dev/null | tail -1)
# the driver Makefile relies on kbuild's $(src), so build it as an external
# module through the kernel tree (as buildroot's kernel-module infra does)
make -C "$KSRC" O="$KOBJ" ARCH=arm CROSS_COMPILE="$CROSS" \
	M="$WORK/rtl8812au" CONFIG_RTL8812AU=m KVER="$KVER" KSRC="$KOBJ" \
	-j"$(nproc)" modules
ko=$(ls "$WORK"/rtl8812au/*.ko | head -1)
"${CROSS}strip" --strip-debug -o "$OUT/88XXau.ko" "$ko"

echo "== outputs =="
file "$OUT"/wfb/* "$OUT/88XXau.ko"
modinfo "$OUT/88XXau.ko" 2>/dev/null | grep -E "^vermagic|^depends" || true
echo "kernel release: $KVER"
