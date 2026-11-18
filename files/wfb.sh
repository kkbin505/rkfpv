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
		# skip comments: the eval below would run them
		case "${key#"${key%%[![:space:]]*}"}" in \#*) continue ;; esac
		key=$(eval echo "$key")
		value=$(eval echo "$value")
		[ -n "$value" ] && export "$key=$value"
	done < "$1"
}

# USB IDs -> driver. 88XXau as OpenIPC; the 88x2cu / 8812eu lists are the
# Realtek default IDs from those drivers' usb_intf.c tables.
detect_driver() {
	for card in $(lsusb | awk '{print $6}' | sort -u); do
		case "$card" in
			0bda:8812|0bda:881a|0b05:17d2|2357:0101|2604:0012)
				driver=88XXau ;;		# RTL8812AU / 8811AU
			0bda:c812|0bda:c82c|0bda:c82e|0bda:d820|0bda:d82b)
				driver=88x2cu ;;		# RTL8812CU / 8822CU
			0bda:a81a|0bda:a82a|0bda:e822)
				driver=8812eu ;;		# RTL8812EU / 8822EU
		esac
	done
}

load_modules() {
	# the adapter needs a moment to enumerate after the switch to host mode
	# (polled every 0.2 s, up to 10 s)
	i=0
	while [ $i -lt 50 ]; do
		detect_driver
		[ -n "$driver" ] && break
		usleep 200000
		i=$((i + 1))
	done
	if [ -z "$driver" ]; then
		log "no supported WiFi adapter (8812AU/8812CU/8822CU/8812EU) detected (is USB in host mode?)"
		return 1
	fi
	log "adapter driver: $driver"
	[ -e /sys/class/net/"$wfb_dev" ] && return 0
	# module options as OpenIPC passes them: power-by-rate and the regulatory
	# power limit tables off for the newer drivers, 88XXau left at defaults
	# (88XXau has no MaxTxBufLen parameter)
	opts=""
	[ "$driver" != 88XXau ] && opts="rtw_tx_pwr_by_rate=0 rtw_tx_pwr_lmt_enable=0 MaxTxBufLen=32"
	insmod "$ko_dir/$driver.ko" $opts || return 1
	i=0
	while [ $i -lt 25 ]; do
		[ -e /sys/class/net/"$wfb_dev" ] && return 0
		usleep 200000
		i=$((i + 1))
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
	# txpower uses OpenIPC's per-driver scale: 88XXau takes it as negative
	# mBm (x -100), the others as mBm (x 50). Unset -> OpenIPC's first-boot
	# default for that adapter (88XXau 20, 8812eu 40; 88x2cu has none there).
	if [ -z "$txpower" ]; then
		case "$driver" in
			8812eu) txpower=40 ;;
			*) txpower=20 ;;
		esac
	fi
	[ "$driver" = 88XXau ] && wfb_power=$((txpower * -100)) || wfb_power=$((txpower * 50))
	log "txpower $txpower -> iw $wfb_power"
	iw "$wfb_dev" set txpower fixed "$wfb_power"
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
