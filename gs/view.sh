#!/bin/sh
# Live view of the H.265 RTP stream wfb_rx forwards to UDP 5600.
# Run as the desktop user (not root): root has no X session, so
# autovideosink falls back to GBM/KMS and gst-launch segfaults.
[ "$(id -u)" = 0 ] && { echo "run as your desktop user, not root"; exit 1; }
: "${DISPLAY:=:0}"; export DISPLAY
SINK=${SINK:-xvimagesink}
exec gst-launch-1.0 udpsrc port=5600 \
	caps='application/x-rtp,media=video,clock-rate=90000,encoding-name=H265,payload=97' \
	! rtpjitterbuffer latency=30 ! rtph265depay ! h265parse ! avdec_h265 \
	! videoconvert ! $SINK sync=false
