#!/bin/sh
# The default OSD layout lives in two places: rkfpv (src/config.c) and the
# fpv parameter table (files/fpv). They must match.
#   test/check_osd_default.sh build-host/test_osd files/fpv
c=$("$1" --default-layout)
f=$(grep '^osd_layout|' "$2" | cut -d'|' -f7)
if [ "$c" = "$f" ]; then
	echo "osd default layout: rkfpv = fpv"
else
	echo "FAIL osd default layout differs"
	echo "  rkfpv: $c"
	echo "  fpv:   $f"
	exit 1
fi
