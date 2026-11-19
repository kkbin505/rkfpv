#!/bin/sh
# Live, readable view of wfb_rx's per-second log (gs/build/wfb_rx.log).
#   gs/stats.sh [logfile]
LOG=${1:-$(cd "$(dirname "$0")" && pwd)/build/wfb_rx.log}
tail -n 0 -F "$LOG" | awk -F'\t' '
$2 == "RX_ANT" {
	split($3, f, ":"); split($5, a, ":")
	ant[$4] = sprintf("ant%s rssi %s/%s/%s dBm snr %s dB", $4, a[2], a[3], a[4], a[6])
	chan = sprintf("%s MHz mcs %s %s MHz", f[1], f[2], f[3])
}
$2 == "PKT" {
	# all:bytes:dec_err:session:data:uniq:fec_rec:lost:bad:out:out_bytes
	split($3, p, ":")
	line = sprintf("%s  pkts %4d  lost %3d  fec %3d  dec_err %d  bad %d  -> %4d pkt %6.2f Mbps",
		chan, p[1], p[8], p[7], p[3], p[9], p[10], p[11] * 8 / 1e6)
	for (k in ant) line = line "  |  " ant[k]
	print line; fflush()
	delete ant
}'
