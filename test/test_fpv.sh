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
check "schema has every key" 67 "$(fpv schema --json | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["params"]))')"

# logs with quotes / tabs / backslashes
printf 'stats: 30.0 fps, 3512 kbps, "x"\tq\\z\n' > "$T/root/userdata/log/rkfpv.log"
check "json log text" 'stats: 30.0 fps, 3512 kbps, "x" q\z' \
	"$(fpv log rkfpv --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["text"].strip())')"
check "status parses stats" "30.0 3512" \
	"$(fpv status --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["fps"], d["kbps"])')"

# DVR
setup
mkdir -p "$T/root/mnt/dvr" "$T/root/tmp"
D="$T/root/mnt/dvr"
head -c 3000 /dev/zero > "$D/rec_0001_00.mp4"
head -c 2000 /dev/zero > "$D/rec_0001_01.mp4"
head -c 1000 /dev/zero > "$D/rec_0002_00.ts"
head -c 500 /dev/zero > "$D/rec_0001_01.bbl"
touch "$D/notes.txt"
check "dvr list newest first, blackbox logs too" "rec_0002_00.ts rec_0001_01.mp4 rec_0001_01.bbl rec_0001_00.mp4" \
	"$(fpv dvr list --json | python3 -c 'import json,sys; print(" ".join(f["name"] for f in json.load(sys.stdin)["files"]))')"
check "dvr list sizes" "1000" "$(fpv dvr list --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["files"][0]["size"])')"
fpv dvr delete ../../etc/rkfpv.conf > /dev/null 2>&1 && bad "path delete accepted" || ok "path delete rejected"
fpv dvr delete notes.txt > /dev/null 2>&1 && bad "non-recording delete accepted" || ok "non-recording delete rejected"
[ -f "$T/root/etc/rkfpv.conf" ] && [ -f "$D/notes.txt" ] && ok "other files untouched" || bad "other files touched"
fpv dvr delete rec_0001_00.mp4 > /dev/null && [ ! -e "$D/rec_0001_00.mp4" ] && ok "dvr delete" || bad "dvr delete"
fpv dvr delete rec_0009_00.mp4 > /dev/null 2>&1 && bad "missing file delete ok" || ok "missing file delete fails"
fpv dvr delete rec_0001_01.bbl > /dev/null && [ ! -e "$D/rec_0001_01.bbl" ] && ok "blackbox log delete" || bad "blackbox log delete"
fpv dvr delete rec_0001_01.bbl.txt > /dev/null 2>&1 && bad "bad .bbl name accepted" || ok "bad .bbl name rejected"
fpv dvr delete-all > /dev/null
check "delete-all keeps non-recordings" "notes.txt" "$(ls "$D")"
fpv dvr start > /dev/null 2>&1 && bad "start without FPV accepted" || ok "start refused when FPV is off"
check "dvr status off" "stopped" "$(fpv dvr status --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])')"
fpv set dvr_segment 300 dvr_format ts dvr_overwrite 0 > /dev/null
check "dvr settings written" "dvr_segment = 300|dvr_overwrite = 0|dvr_format = ts" \
	"$(grep -E '^dvr_(segment|overwrite|format)' "$T/root/etc/rkfpv.conf" | sed 's/ *#.*//' | tr '\n' '|' | sed 's/|$//')"
fpv set dvr_format mkv > /dev/null 2>&1 && bad "bad dvr_format accepted" || ok "bad dvr_format rejected"
fpv set dvr_segment 5 > /dev/null 2>&1 && bad "dvr_segment 5 accepted" || ok "dvr_segment below 10 rejected"

# Betaflight blackbox (bblog): settings, status, selftest guards
fpv set blackbox 1 blackbox_baud 2000000 dvr_osd 0 > /dev/null
check "blackbox settings written" "dvr_osd = 0|blackbox = 1|blackbox_baud = 2000000" \
	"$(grep -E '^(dvr_osd|blackbox|blackbox_baud) ' "$T/root/etc/rkfpv.conf" | sed 's/ *#.*//' | tr '\n' '|' | sed 's/|$//')"
fpv set blackbox_baud 115200 > /dev/null 2>&1 && bad "blackbox_baud 115200 accepted" || ok "blackbox_baud not in the list rejected"
check "blackbox status: enabled, not running" "True 2000000 stopped" \
	"$(fpv blackbox status --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["enabled"], d["baud"], d["state"])')"
check "fpv status has blackbox" "stopped" \
	"$(fpv status --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["blackbox"])')"
fpv blackbox selftest 1234 > /dev/null 2>&1 && bad "selftest odd baud accepted" || ok "selftest odd baud rejected"

# local MAVLink port (mavbridge -j), in wfb.yaml under telemetry:
check "mav_local_port default off" "0" "$(fpv get mav_local_port | sed 's/^[^=]*= *//')"
fpv set mav_local_port 14570 > /dev/null
check "mav_local_port written under telemetry" "telemetry:|  mav_local_port: 14570" \
	"$(grep -E '^telemetry:|mav_local_port:' "$T/root/etc/wfb.yaml" | grep -v '#' | tr '\n' '|' | sed 's/|$//')"
fpv set mav_local_port 70000 > /dev/null 2>&1 && bad "mav_local_port 70000 accepted" || ok "mav_local_port out of range rejected"
fpv set mav_local_port 0 > /dev/null
fpv blackbox selftest -r x > /dev/null 2>&1 && bad "selftest bad rate accepted" || ok "selftest bad rate rejected"
check "selftest without UART4" "no /dev/ttyS4" \
	"$(fpv blackbox selftest 2>&1 | sed -n 's/.*\(no \/dev\/ttyS4\).*/\1/p')"
fpv blackbox nope > /dev/null 2>&1 && bad "blackbox bad subcommand accepted" || ok "blackbox bad subcommand rejected"
rm -rf "$T/root/mnt" "$T/root/tmp"
check "no partition -> no-card" "no-card" "$(fpv dvr status --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])')"

# DVR card discovery and format (fake sysfs, fake mkfs/mount in PATH)
setup
B="$T/root/sys/class/block"
dstate() { fpv dvr status --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])'; }
# Mini: boot card mmcblk1, partition named dvr
mkdir -p "$B/mmcblk1/mmcblk1p6" "$B/mmcblk1/mmcblk1p7" "$B/mmcblk1p6" "$B/mmcblk1p7" "$T/root/proc"
printf 'DEVNAME=mmcblk1p6\nPARTNAME=rootfs\n' > "$B/mmcblk1p6/uevent"
printf 'DEVNAME=mmcblk1p7\nPARTNAME=dvr\n' > "$B/mmcblk1p7/uevent"
echo "console=ttyFIQ0 root=/dev/mmcblk1p6 rootwait" > "$T/root/proc/cmdline"
check "mini: dvr partition" "/dev/mmcblk1p7 part other" "$(fpv dvr device)"
rm -rf "$B/mmcblk1p7" "$B/mmcblk1/mmcblk1p7"
check "mini without dvr partition: boot card never picked" "" "$(fpv dvr device)"
# Pro: NAND boot (ubi), TF card mmcblk0 with one partition
rm -rf "$B"; mkdir -p "$B/mmcblk0/mmcblk0p1" "$B/mmcblk0p1" "$B/mtdblock3" "$T/root/dev"
echo "console=ttyFIQ0 ubi.mtd=5 root=ubi0:rootfs rootfstype=ubifs" > "$T/root/proc/cmdline"
printf 'Luckfox Pico Pro Max\000' > "$T/root/proc/device-tree/model" 2>/dev/null || { mkdir -p "$T/root/proc/device-tree"; printf 'Luckfox Pico Pro Max\000' > "$T/root/proc/device-tree/model"; }
check "pro: TF card first partition" "/dev/mmcblk0p1 card other" "$(fpv dvr device)"
head -c 4096 /dev/zero > "$T/root/dev/mmcblk0p1"          # FAT or blank: not ext4
check "pro: new card is unformatted" "unformatted" "$(dstate)"
check "status board + unformatted" "Luckfox Pico Pro Max unformatted" \
	"$(fpv status --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["board"], d["dvr_state"])')"
mkdir -p "$T/bin"
for c in mkfs.ext4 mount umount; do
	printf '#!/bin/sh\necho "%s $*" >> "%s/cmds"\n' "$c" "$T" > "$T/bin/$c"; chmod +x "$T/bin/$c"
done
fpvp() { PATH=$T/bin:$PATH fpv "$@"; }
fpvp dvr format > /dev/null 2>&1 && bad "format without --yes accepted" || ok "format needs --yes"
[ ! -e "$T/cmds" ] && ok "nothing run without --yes" || bad "commands ran without --yes"
fpvp dvr format --yes > /dev/null && grep -q "^mkfs.ext4 -F -q -L dvr -m 0 $T/root/dev/mmcblk0p1\$" "$T/cmds" \
	&& grep -q "^mount -t ext4 .* $T/root/dev/mmcblk0p1 $T/root/mnt/dvr\$" "$T/cmds" \
	&& ok "format: mkfs + mount on the TF card" || bad "format: $(cat "$T/cmds" 2>/dev/null)"
rm -rf "$B/mmcblk0" "$B/mmcblk0p1"
fpvp dvr format --yes > /dev/null 2>&1 && bad "format without a card accepted" || ok "format refuses with no card"
check "pro: no card -> no-card" "no-card" "$(rm -rf "$T/root/mnt"; dstate)"

# ISP settings: words and ranges
setup
fpv set isp_exp_max 4 isp_nr_temporal iq isp_blc off isp_antiflicker 60 isp_wb shade > /dev/null
check "isp values stored" "isp_exp_max = 4|isp_nr_temporal = iq|isp_blc = off|isp_antiflicker = 60|isp_wb = shade" \
	"$(fpv get isp_exp_max isp_nr_temporal isp_blc isp_antiflicker isp_wb | tr '\n' '|' | sed 's/|$//')"
check "rkfpv.conf keeps the word iq" "isp_nr_temporal = iq" "$(grep '^isp_nr_temporal' "$T/root/etc/rkfpv.conf" | sed 's/ *#.*//')"
fpv set isp_exp_max 34 > /dev/null 2>&1 && bad "isp_exp_max 34 accepted" || ok "isp_exp_max 34 rejected"
fpv set isp_exp_max off > /dev/null 2>&1 && bad "off accepted where not allowed" || ok "off only where allowed"
fpv set isp_sharpness auto > /dev/null 2>&1 && bad "auto accepted on an iq key" || ok "auto only on auto keys"
fpv set txpower_8812cu iq > /dev/null 2>&1 && bad "iq accepted on txpower" || ok "iq only on iq keys"
fpv set isp_antiflicker 55 > /dev/null 2>&1 && bad "antiflicker 55 accepted" || ok "antiflicker 55 rejected"
fpv set isp_metering bottom_strong > /dev/null
check "metering stored" "isp_metering = bottom_strong" "$(fpv get isp_metering)"
fpv set isp_metering top > /dev/null 2>&1 && bad "metering top accepted" || ok "metering top rejected"
check "metering on the Image tab" "Exposure" "$(fpv schema --json | python3 -c 'import json,sys; print(next(p for p in json.load(sys.stdin)["params"] if p["key"]=="isp_metering")["group"])')"
check "schema specials" "iq off" "$(fpv schema --json | python3 -c 'import json,sys; print(" ".join(next(p for p in json.load(sys.stdin)["params"] if p["key"]=="isp_nr_spatial")["specials"]))')"
check "schema base type" "int" "$(fpv schema --json | python3 -c 'import json,sys; print(next(p for p in json.load(sys.stdin)["params"] if p["key"]=="isp_nr_spatial")["type"])')"

# IQ files: stock never written, verified install, history, rollback
setup
S="$T/root/oem/usr/share/iqfiles"; C="$T/root/userdata/iq"
mkdir -p "$S"
printf 'stock\r\nno-newline' > "$S/sc3336_T.json"
cp "$S/sc3336_T.json" "$T/stock.orig"
fpv iq copy-stock > /dev/null
cmp -s "$S/sc3336_T.json" "$C/sc3336_T.json" && ok "copy-stock byte-identical (CRLF kept)" || bad "copy-stock changed bytes"
printf 'mine' > "$C/sc3336_T.json"
fpv iq copy-stock > /dev/null
check "copy-stock never overwrites an edit" "mine" "$(cat "$C/sc3336_T.json")"
for i in 1 2 3 4 5 6 7; do
	printf "v$i" > "$C/sc3336_T.json.new"
	fpv iq install sc3336_T.json "$(md5sum "$C/sc3336_T.json.new" | cut -d' ' -f1)" > /dev/null
done
check "install puts the new file in place" "v7" "$(cat "$C/sc3336_T.json")"
check "history keeps the last 5" "3 4 5 6 7" "$(ls "$C/history" | sed 's/.*\.//' | sort -n | tr '\n' ' ' | sed 's/ $//')"
printf 'corrupt' > "$C/sc3336_T.json.new"
fpv iq install sc3336_T.json 00000000000000000000000000000000 > /dev/null 2>&1 && bad "bad md5 accepted" || ok "bad md5 rejected"
check "bad upload leaves the active file" "v7" "$(cat "$C/sc3336_T.json")"
[ ! -e "$C/sc3336_T.json.new" ] && ok "bad upload removed" || bad "bad upload left behind"
cp "$C/history/sc3336_T.json.5" "$T/v5"
fpv iq rollback sc3336_T.json 5 > /dev/null
cmp -s "$T/v5" "$C/sc3336_T.json" && ok "rollback restores byte-identical" || bad "rollback content"
fpv iq rollback sc3336_T.json > /dev/null
check "rollback without a version = previous" "v7" "$(cat "$C/sc3336_T.json")"
fpv iq install ../x.json 0123456789abcdef0123456789abcdef > /dev/null 2>&1 && bad "path name accepted" || ok "path name rejected"
fpv iq install other.json 0123456789abcdef0123456789abcdef > /dev/null 2>&1 && bad "non-stock name accepted" || ok "non-stock name rejected"
cmp -s "$T/stock.orig" "$S/sc3336_T.json" && ok "stock file never changed" || bad "stock file changed"
check "iq status lists both" "sc3336_T.json sc3336_T.json" \
	"$(fpv iq status --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["stock"][0]["name"], d["custom"][0]["name"])')"

# hotspot settings (str type)
setup
fpv set hotspot_ssid rkfpv-1a2b hotspot_pass Secret_12.ab hotspot_channel 149 > /dev/null
check "hotspot settings stored" "hotspot_ssid = rkfpv-1a2b|hotspot_pass = Secret_12.ab|hotspot_channel = 149" \
	"$(fpv get hotspot_ssid hotspot_pass hotspot_channel | tr '\n' '|' | sed 's/|$//')"
check "stored under hotspot: in wfb.yaml" "hotspot:|  hotspot_pass: Secret_12.ab|  hotspot_ssid: rkfpv-1a2b" \
	"$(grep -A2 '^hotspot:' "$T/root/etc/wfb.yaml" | tr '\n' '|' | sed 's/|$//')"
for bad in "has space" 'x;reboot' 'a$b' "q'q" ''; do
	fpv set hotspot_ssid "$bad" > /dev/null 2>&1 && bad "ssid '$bad' accepted" || ok "ssid '$bad' rejected"
done
fpv set hotspot_pass short1 > /dev/null 2>&1 && bad "7-char password accepted" || ok "password under 8 rejected"
fpv set hotspot_ssid "$(printf 'a%.0s' $(seq 33))" > /dev/null 2>&1 && bad "33-char ssid accepted" || ok "ssid over 32 rejected"
fpv set hotspot_channel 13 > /dev/null 2>&1 && bad "channel 13 accepted" || ok "channel 13 rejected"
check "schema str pattern" "str 1 32" "$(fpv schema --json | python3 -c 'import json,sys; p=next(x for x in json.load(sys.stdin)["params"] if x["key"]=="hotspot_ssid"); print(p["type"], p["min"], p["max"])')"
mkdir -p "$T/root/tmp"; echo "switch=closed mode_request=hotspot" > "$T/root/tmp/switch.status"; echo hotspot > "$T/root/tmp/mode.status"
check "hotspot status" "closed hotspot rkfpv-1a2b" \
	"$(fpv hotspot status --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["switch"], d["mode"], d["ssid"])')"

# every preset in configurator/presets must be accepted by fpv as a whole
for f in "$HERE"/configurator/presets/*.json; do
	setup
	pairs=$(python3 -c 'import json,sys; v=json.load(open(sys.argv[1]))["values"]; print(" ".join(f"{k} {x}" for k,x in v.items()))' "$f")
	fpv set $pairs > /dev/null 2>&1 && ok "preset $(basename "$f")" || bad "preset $(basename "$f"): $(fpv set $pairs 2>&1)"
done
setup

# mode + reset
fpv mode debug > /dev/null; [ -e "$T/root/userdata/fpv.off" ] && ok "mode debug" || bad "mode debug"
fpv mode auto > /dev/null; [ ! -e "$T/root/userdata/fpv.off" ] && ok "mode auto" || bad "mode auto"
fpv set bitrate 5000 > /dev/null; fpv reset > /dev/null
check "reset" "bitrate = 3500" "$(fpv get bitrate)"

# wired preview: only in debug mode, runs S99fpv preview-on/off
setup
mkdir -p "$T/root/tmp" "$T/root/etc/init.d"
printf '#!/bin/sh\necho "$1" >> %s/init.log\n' "$T" > "$T/root/etc/init.d/S99fpv"
chmod 755 "$T/root/etc/init.d/S99fpv"
rm -f "$T/init.log"
echo fpv > "$T/root/tmp/mode.status"
fpv preview on > /dev/null 2>&1 && bad "preview refused in FPV mode" || ok "preview refused in FPV mode"
[ ! -e "$T/init.log" ] && ok "refused preview ran nothing" || bad "refused preview ran nothing"
echo debug > "$T/root/tmp/mode.status"
fpv preview on > /dev/null && fpv preview off > /dev/null
check "preview on/off in debug mode" "preview-on|preview-off" "$(tr '\n' '|' < "$T/init.log" | sed 's/|$//')"
check "preview status json" '{"ok":true,"running":false,"port":5602,"mode":"debug"}' "$(fpv preview status --json)"
# hotspot mode: live view for the web configurator
rm -f "$T/init.log"
echo hotspot > "$T/root/tmp/mode.status"
fpv preview on > /dev/null && fpv preview off > /dev/null
check "preview on/off in hotspot mode" "preview-on|preview-off" "$(tr '\n' '|' < "$T/init.log" | sed 's/|$//')"
check "status mode in hotspot mode" "hotspot" "$(fpv status --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["mode"])')"
sed -i 's/^preview_port = .*/preview_port = 0/' "$T/root/etc/rkfpv.conf"
fpv preview on > /dev/null 2>&1 && bad "preview_port 0 refused" || ok "preview_port 0 refused"

echo "$((n - fail))/$n passed"
[ $fail -eq 0 ]
