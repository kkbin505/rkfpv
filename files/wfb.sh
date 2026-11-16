#!/bin/sh
# wfb-ng video TX for rkfpv on Luckfox firmware.
# Trimmed from OpenIPC's /usr/bin/wifibroadcast: no majestic, no cli.
# wfb_tx reads RTP from UDP $video_port (rkfpv sends there) instead of the
# rtp_local unix socket majestic uses.

wfb_key=/etc/drone.key
wfb_cfg=/etc/wfb.yaml
wfb_dev=wlan0
ko_dir=/oem/usr/ko

log() { echo "$1" | logger -s -t wfb; }

wfb_yaml() {
	[ -e "$1" ] || return
	while IFS=: read -r key value; do
		key=$(eval echo "$key")
		value=$(eval echo "$value")
		[ -n "$value" ] && export "$key=$value"
	done < "$1"
}

load_modules() {
	[ -e /sys/class/net/"$wfb_dev" ] && return 0
	# the adapter needs a moment to enumerate after the switch to host mode
	for i in 1 2 3 4 5 6 7 8 9 10; do
		for card in $(lsusb | awk '{print $6}' | sort -u); do
			case "$card" in
				0bda:8812|0bda:881a|0b05:17d2|2357:0101|2604:0012) driver=88XXau ;;
			esac
		done
		[ -n "$driver" ] && break
		sleep 1
	done
	if [ -z "$driver" ]; then
		log "RTL8812AU not detected (is USB in host mode?)"
		return 1
	fi
	insmod "$ko_dir/$driver.ko" rtw_tx_pwr_by_rate=0 rtw_tx_pwr_lmt_enable=0 || return 1
	for i in 1 2 3 4 5; do
		[ -e /sys/class/net/"$wfb_dev" ] && return 0
		sleep 1
	done
	log "$wfb_dev did not appear"
	return 1
}

load_interface() {
	ifconfig "$wfb_dev" up || return 1
	iw "$wfb_dev" set monitor none
	[ "$width" = "40" ] && ht=HT40+ || ht=HT20
	iw "$wfb_dev" set channel "$channel" "$ht"
	iw reg set 00
	# 88XXau takes the power in negative mBm units (see OpenIPC script)
	iw "$wfb_dev" set txpower fixed $((txpower * -100))
}

# MAVLink over wfb-ng, same radio ports as OpenIPC so ground stations need
# no change: downlink radio port 16 (FC -> GS), uplink radio port 144.
#   UART3 -> mavbridge -> udp:14551 -> wfb_tx -p 16
#   wfb_rx -p 144 -> udp:14550 -> mavbridge -> UART3
start_telemetry() {
	[ "$telemetry_enabled" = "true" ] || return 0
	: "${serial:=ttyS3}" "${baud:=115200}" "${tun_index:=1}"
	log "telemetry: /dev/$serial @ $baud"
	wfb_rx -K "$wfb_key" -i "$link_id" -p 144 -u 14550 "$wfb_dev" > /dev/null 2>&1 &
	wfb_tx -K "$wfb_key" -M "$tun_index" -B "$width" -k 1 -n 2 \
		-S "$stbc" -L "$ldpc" -i "$link_id" -p 16 -u 14551 "$wfb_dev" > /dev/null 2>&1 &
	mavbridge -d /dev/"$serial" -b "$baud" -o 127.0.0.1:14551 -i 14550 \
		-a 15 -m 1000 -t 30 > /dev/null 2>&1 &
}

start() {
	wfb_yaml "$wfb_cfg"
	: "${video_port:=5600}"
	load_modules || exit 1
	load_interface || exit 1
	stop
	log "wfb_tx: ch $channel ${width}MHz mcs $mcs_index fec $fec_k/$fec_n udp $video_port"
	wfb_tx -K "$wfb_key" -M "$mcs_index" -B "$width" -k "$fec_k" -n "$fec_n" \
		-S "$stbc" -L "$ldpc" -i "$link_id" -u "$video_port" \
		-C 8000 -J 10 -E 5000 "$wfb_dev" > /dev/null 2>&1 &
	start_telemetry
}

stop() {
	killall -q mavbridge wfb_tx wfb_rx
}

case "$1" in
	start|stop) "$1" ;;
	*) echo "Usage: $0 {start|stop}"; exit 1 ;;
esac
