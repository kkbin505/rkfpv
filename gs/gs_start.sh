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

# the adapter's interface (any RTL8812AU USB id handled by the driver)
find_if() {
	IF=""
	for n in /sys/class/net/*; do
		[ -e "$n/phy80211" ] || continue
		[ "$(basename "$(readlink -f "$n/device/driver")")" = rtl88xxau_wfb ] && IF=$(basename "$n")
	done
}
for i in 1 2 3 4 5 6 7 8 9 10; do
	find_if
	[ -n "$IF" ] && break
	sleep 1
done
# udev renames the driver's "wlan0" to wlx<mac> a moment later
udevadm settle -t 5 2>/dev/null || sleep 2
find_if
[ -n "$IF" ] || { echo "no 8812AU interface found"; ls /sys/class/net; exit 1; }
echo "interface: $IF"

# keep NetworkManager / wpa_supplicant off it, or they put it back to managed
nmcli dev set "$IF" managed no 2>/dev/null || true
sleep 1
# per-interface iw commands only; "iw dev" dumps every wiphy
for try in 1 2 3; do
	ip link set "$IF" down
	timeout 10 iw dev "$IF" set type monitor || exit 1
	ip link set "$IF" up
	timeout 10 iw dev "$IF" set channel "$CH" HT20 || exit 1
	sleep 1
	# 803 = ARPHRD_IEEE80211_RADIOTAP, i.e. monitor mode stuck
	[ "$(cat /sys/class/net/$IF/type)" = 803 ] && break
	echo "interface went back to managed, retrying ($try)"
done
[ "$(cat /sys/class/net/$IF/type)" = 803 ] || { echo "could not keep $IF in monitor mode"; exit 1; }
echo "monitor, channel $CH HT20"

pkill -f "$RX" 2>/dev/null
nohup "$RX" -K "$KEY" -i 7669206 -p 0 -c 127.0.0.1 -u 5600 -l 1000 "$IF" > "$LOG" 2>&1 &
chmod 644 "$LOG"
sleep 3
echo "--- $LOG"
tail -5 "$LOG"
