#!/bin/sh
# Builds the bench ground-station pieces for this Linux PC (no root needed):
# svpcom's RTL8812AU driver (wfb-ng's own, current kernels) and wfb_rx at the
# same wfb-ng revision the air unit runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
B=$HERE/build
DRV_REV=3577999972f350a2213e7509ae1f371b99fc96ab	# svpcom/rtl8812au v5.2.20
WFB_TGZ=$HERE/../luckfox/dl/wfb-ng-ae2122c1fb70cd215bbc2e1b517b7b91f9401220.tar.gz
mkdir -p "$B"; cd "$B"
rm -rf svp8812 wfb; mkdir svp8812 wfb
curl -sfL "https://github.com/svpcom/rtl8812au/archive/$DRV_REV.tar.gz" | tar xz -C svp8812 --strip-components=1
[ -e "$WFB_TGZ" ] || curl -sfL -o "$WFB_TGZ" https://github.com/svpcom/wfb-ng/archive/ae2122c1fb70cd215bbc2e1b517b7b91f9401220.tar.gz
tar xzf "$WFB_TGZ" -C wfb --strip-components=1
make -C wfb VERSION=rkfpv COMMIT=ae2122c1 wfb_rx -j4 > wfb.log 2>&1
(cd svp8812 && make -j"$(nproc)" KVER="$(uname -r)" > ../drv.log 2>&1)
ls -l wfb/wfb_rx svp8812/88XXau_wfb.ko
