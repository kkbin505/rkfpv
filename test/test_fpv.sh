#!/bin/sh
# Tests for files/fpv against copies of the shipped config files.
#   tests/test_fpv.sh [shell]     (default: run under dash and busybox sh)
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
FPV=$HERE/files/fpv

if [ $# -eq 0 ]; then
	rc=0
	for sh in dash "busybox sh"; do
		command -v ${sh%% *} > /dev/null || continue
		echo "== $sh"
		"$0" "$sh" || rc=1
	done
	exit $rc
fi
SH=$1

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0; n=0

setup() {
	rm -rf "$T/root"
	mkdir -p "$T/root/etc/fpv/defaults" "$T/root/userdata/log"
	cp "$HERE/files/rkfpv.conf" "$HERE/files/wfb.yaml" "$T/root/etc/"
	cp "$HERE/files/rkfpv.conf" "$HERE/files/wfb.yaml" "$T/root/etc/fpv/defaults/"
}
fpv() { FPV_ROOT=$T/root $SH "$FPV" "$@"; }
ok() { n=$((n + 1)); echo "ok   $1"; }
bad() { n=$((n + 1)); fail=$((fail + 1)); echo "FAIL $1"; }
check() { # check <name> <expected> <actual>
	[ "$2" = "$3" ] && ok "$1" || bad "$1: expected '$2', got '$3'"
}

setup
check "get bitrate" "bitrate = 3500" "$(fpv get bitrate)"
check "get mcs_index (yaml)" "mcs_index = 2" "$(fpv get mcs_index)"
check "get unset txpower -> auto" "txpower_8812eu = auto" "$(fpv get txpower_8812eu)"
check "get commented max_bitrate -> default" "max_bitrate = 0" "$(fpv get max_bitrate)"
check "get mirror ignores inline comment" "mirror = 1" "$(fpv get mirror)"

fpv set bitrate 4000 mcs_index 3 > /dev/null
check "set bitrate" "bitrate = 4000" "$(fpv get bitrate)"
check "set mcs_index" "mcs_index = 3" "$(fpv get mcs_index)"
check "only one bitrate line" 1 "$(grep -c '^bitrate' "$T/root/etc/rkfpv.conf")"

fpv set mirror 0 > /dev/null
check "inline comment kept" "mirror = 0                  # mirror + flip = rotate 180° (camera mounted upside down)" \
	"$(grep '^mirror' "$T/root/etc/rkfpv.conf")"

fpv set max_bitrate 4500 > /dev/null
check "uncomment rkfpv key" "max_bitrate = 4500        # VBR only" "$(grep '^max_bitrate' "$T/root/etc/rkfpv.conf")"

fpv set txpower_8812cu 25 > /dev/null
check "uncomment yaml setting, not the prose line" "  txpower_8812cu: 25" "$(grep '^  txpower_8812cu' "$T/root/etc/wfb.yaml")"
check "prose comments untouched" "$(grep '^  # .*[a-z] [a-z]' "$HERE/files/wfb.yaml")" "$(grep '^  # .*[a-z] [a-z]' "$T/root/etc/wfb.yaml")"
check "other cards stay unset" "txpower_8812au = auto" "$(fpv get txpower_8812au)"
fpv set txpower_8812cu auto > /dev/null
check "txpower auto -> commented out" "txpower_8812cu = auto" "$(fpv get txpower_8812cu)"
check "no active txpower line" 0 "$(grep -c '^  txpower' "$T/root/etc/wfb.yaml")"
fpv set txpower_8812eu 64 > /dev/null 2>&1 && bad "txpower 64 accepted" || ok "txpower 64 rejected"

# wfb.sh: per-card key -> iw value, defaults, legacy `txpower` fallback
txp() { # txp <driver> [yaml lines...]  -> mBm wfb.sh would pass to iw
	drv=$1; shift
	printf 'wireless:\n' > "$T/w.yaml"
	for l in "$@"; do printf '  %s\n' "$l" >> "$T/w.yaml"; done
	$SH -c '
		log() { :; }
		iw() { echo "$6"; }
		. /dev/stdin <<"EOS"
'"$(sed -n -e '/^wfb_yaml()/,/^}/p' -e '/^set_txpower()/,/^}/p' "$HERE/files/wfb.sh")"'
EOS
		wfb_dev=wlan0; driver=$1; wfb_yaml "$2"; set_txpower' x "$drv" "$T/w.yaml"
}
check "8812au default 20 -> index"   -2000 "$(txp 88XXau)"
check "8812cu default 20 -> 10 dBm"   1000 "$(txp 88x2cu)"
check "8812eu default 40 -> 20 dBm"   2000 "$(txp 8812eu)"
check "8812au own key"               -6300 "$(txp 88XXau 'txpower_8812au: 63' 'txpower_8812eu: 10')"
check "8812eu own key, not au's"       500 "$(txp 8812eu 'txpower_8812au: 63' 'txpower_8812eu: 10')"
check "8812cu ignores commented key"  1000 "$(txp 88x2cu '# txpower_8812cu: 50')"
check "legacy txpower fallback"       1500 "$(txp 88x2cu 'txpower: 30')"
check "own key beats legacy"          2500 "$(txp 88x2cu 'txpower: 30' 'txpower_8812cu: 50')"

# wfb.sh's parser must still read the edited yaml
fpv set channel 149 stbc 1 > /dev/null
got=$($SH -c '
	. /dev/stdin <<"EOS"
'"$(sed -n '/^wfb_yaml()/,/^}/p' "$HERE/files/wfb.sh")"'
EOS
	wfb_yaml "$1"; echo "$channel $stbc $mcs_index"' x "$T/root/etc/wfb.yaml")
check "wfb.sh parser reads edited yaml" "149 1 3" "$got"

# missing key -> appended under its section
setup
sed -i '/ldpc/d' "$T/root/etc/wfb.yaml"
fpv set ldpc 1 > /dev/null
check "missing yaml key added under section" "broadcast:|  ldpc: 1" \
	"$(grep -A1 '^broadcast:' "$T/root/etc/wfb.yaml" | tr '\n' '|' | sed 's/|$//')"

# validation: nothing written on any error
setup
cp "$T/root/etc/rkfpv.conf" "$T/a"; cp "$T/root/etc/wfb.yaml" "$T/b"
fpv set bitrate 4000 mcs_index 9 > /dev/null 2>&1 && bad "out of range accepted" || ok "out of range rejected"
fpv set bitrate abc > /dev/null 2>&1 && bad "non-number accepted" || ok "non-number rejected"
fpv set rc xyz > /dev/null 2>&1 && bad "bad enum accepted" || ok "bad enum rejected"
fpv set nosuch 1 > /dev/null 2>&1 && bad "unknown key accepted" || ok "unknown key rejected"
fpv set fec_k 12 fec_n 12 > /dev/null 2>&1 && bad "fec_n <= fec_k accepted" || ok "fec_n <= fec_k rejected"
fpv set bitrate > /dev/null 2>&1 && bad "odd args accepted" || ok "odd args rejected"
cmp -s "$T/a" "$T/root/etc/rkfpv.conf" && cmp -s "$T/b" "$T/root/etc/wfb.yaml" \
	&& ok "files untouched after errors" || bad "files changed after errors"
check "no temp files left" "" "$(ls "$T/root/etc" | grep -v -e '^rkfpv.conf$' -e '^wfb.yaml$' -e '^fpv$')"

# JSON output parses
for c in "get" "get bitrate" "schema" "status" "set bitrate 3000" "log rkfpv" "mode" "set bitrate 99999"; do
	out=$(fpv $c --json 2>/dev/null)
	echo "$out" | python3 -m json.tool > /dev/null 2>&1 && ok "json: $c" || bad "json: $c -> $out"
done
check "json error flag" "False" "$(fpv set bitrate 99999 --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["ok"])')"
check "schema has every key" 26 "$(fpv schema --json | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["params"]))')"

# logs with quotes / tabs / backslashes
printf 'stats: 30.0 fps, 3512 kbps, "x"\tq\\z\n' > "$T/root/userdata/log/rkfpv.log"
check "json log text" 'stats: 30.0 fps, 3512 kbps, "x" q\z' \
	"$(fpv log rkfpv --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["text"].strip())')"
check "status parses stats" "30.0 3512" \
	"$(fpv status --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["fps"], d["kbps"])')"

# mode + reset
fpv mode debug > /dev/null; [ -e "$T/root/userdata/fpv.off" ] && ok "mode debug" || bad "mode debug"
fpv mode auto > /dev/null; [ ! -e "$T/root/userdata/fpv.off" ] && ok "mode auto" || bad "mode auto"
fpv set bitrate 5000 > /dev/null; fpv reset > /dev/null
check "reset" "bitrate = 3500" "$(fpv get bitrate)"

echo "$((n - fail))/$n passed"
[ $fail -eq 0 ]
