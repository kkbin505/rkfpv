#!/bin/sh
# Live view of the H.265 RTP stream wfb_rx forwards to UDP 5600.
# Run as the desktop user (not root): root has no X session, so
# autovideosink falls back to GBM/KMS and gst-launch segfaults.
#
#   PT=96   RTP payload type (rkfpv pt; images before Nov 17 sent 96)
#   JB=30   add an RTP jitter buffer of N ms. Default 0 = none: wfb_rx
#           already delivers packets in order after FEC, so a jitter
#           buffer only holds every frame back by N ms.
[ "$(id -u)" = 0 ] && { echo "run as your desktop user, not root"; exit 1; }
: "${DISPLAY:=:0}"; export DISPLAY
SINK=${SINK:-xvimagesink}
PT=${PT:-97}
JB=${JB:-0}
if [ "$JB" -gt 0 ] 2>/dev/null; then
	JITTER="rtpjitterbuffer latency=$JB !"
else
	JITTER=""
fi
echo "view: payload $PT, jitter buffer ${JB} ms"
# max-threads=1: one decoder thread never queues frames for parallel decode
exec gst-launch-1.0 udpsrc port=5600 \
	caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H265,payload=$PT" \
	! $JITTER rtph265depay ! h265parse ! avdec_h265 max-threads=1 \
	! videoconvert ! $SINK sync=false
