#!/bin/sh
# Bench ground station on a Linux PC:
#   RTL8812AU (svpcom wfb driver) -> monitor -> wfb_rx -> udp 127.0.0.1:5600
# Build first with gs/build.sh (as the normal user), then run this with sudo.
#   sudo gs/gs_start.sh [channel] [key]
HERE=$(cd "$(dirname "$0")" && pwd)
CH=${1:-161}
KEY=${2:-$HERE/../../openipc/keys/gs.key}
KO=$HERE/build/svp8812/88XXau_wfb.ko
RX=$HERE/build/wfb/wfb_rx
LOG=$HERE/build/wfb_rx.log

if lsmod | grep -q '^88XXau '; then
	echo "another 88XXau driver is loaded; reboot or rmmod it first"
	exit 1
fi
lsmod | grep -q '^88XXau_wfb' || insmod "$KO" || exit 1

# wait for the adapter's interface (any RTL8812AU USB id handled by the driver)
IF=""
for i in 1 2 3 4 5 6 7 8 9 10; do
	for n in /sys/class/net/*; do
		[ -e "$n/phy80211" ] || continue
		[ "$(basename "$(readlink -f "$n/device/driver")")" = rtl88xxau_wfb ] && IF=$(basename "$n")
	done
	[ -n "$IF" ] && break
	sleep 1
done
[ -n "$IF" ] || { echo "no 8812AU interface found"; ls /sys/class/net; exit 1; }
echo "interface: $IF"

nmcli dev set "$IF" managed no 2>/dev/null || true
# per-interface iw commands only; "iw dev" dumps every wiphy
ip link set "$IF" down
timeout 10 iw dev "$IF" set monitor otherbss || exit 1
ip link set "$IF" up
timeout 10 iw dev "$IF" set channel "$CH" HT20 || exit 1
echo "monitor, channel $CH HT20"

pkill -f "$RX" 2>/dev/null
nohup "$RX" -K "$KEY" -i 7669206 -p 0 -c 127.0.0.1 -u 5600 -l 1000 "$IF" > "$LOG" 2>&1 &
chmod 644 "$LOG"
sleep 3
echo "--- $LOG"
tail -5 "$LOG"
