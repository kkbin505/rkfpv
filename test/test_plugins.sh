#!/bin/sh
# Host test for files/fpv-plugins and `fpv plugins`: order, reverse stop,
# timeout, non-executable files, FPV_MODE, status file.
#   test/test_plugins.sh [shell]     (default: run under dash and busybox sh)
cd "$(dirname "$0")/.." || exit 1
if [ -z "$1" ]; then
	rc=0
	for sh in dash "busybox sh"; do
		command -v ${sh%% *} > /dev/null || { echo "skip: no $sh"; continue; }
		$sh test/test_plugins.sh "$sh" || rc=1
	done
	exit $rc
fi
SH=$1
P="$SH $PWD/files/fpv-plugins"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }
check() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (want '$2', got '$3')"; }

D=$T/root/etc/fpv/plugins.d
mkdir -p "$D" "$T/root/tmp"
mk() {	# name, body
	printf '#!/bin/sh\n%s\n' "$2" > "$D/$1"
	chmod 755 "$D/$1"
}
mk S10first 'echo "S10 $1 $FPV_MODE" >> '"$T"'/calls'
mk S20second 'echo "S20 $1" >> '"$T"'/calls; [ "$1" = status ] && echo running; [ "$1" != start ]'
mk S30slow '[ "$1" = start ] && sleep 30; echo "S30 $1" >> '"$T"'/calls'
printf '#!/bin/sh\necho "S40 $1" >> %s/calls\n' "$T" > "$D/S40noexec"
mk K50notS 'echo "K50 $1" >> '"$T"'/calls'
export FPV_PLUGIN_STATUS="$T/root/tmp/plugins.status" FPV_PLUGIN_TIMEOUT=1 FPV_PLUGIN_LOG="$T/fpv.log"

start=$(date +%s)
FPV_MODE=hotspot $P start "$D" 2> "$T/err"
took=$(($(date +%s) - start))
check "start: name order, S only, executable only" "S10 start hotspot|S20 start|" \
	"$(tr '\n' '|' < "$T/calls")"
[ "$took" -le 4 ] && ok "hung plugin killed after the timeout (${took}s)" || bad "start took ${took}s"
grep -q 'S30slow start: no answer' "$T/err" && ok "timeout logged" || bad "timeout not logged: $(cat "$T/err")"
grep -q 'S20second start: exit 1' "$T/err" && ok "failure logged" || bad "failure not logged"
grep -q 'plugins: 1 of 3 started' "$T/fpv.log" && ok "summary in fpv.log" || bad "fpv.log: $(cat "$T/fpv.log")"
check "status file" "plugins=3 started=1" "$(cat "$FPV_PLUGIN_STATUS")"

: > "$T/calls"
$P stop "$D" 2> /dev/null
check "stop: reverse order" "S30 stop|S20 stop|S10 stop |" "$(tr '\n' '|' < "$T/calls")"
[ -e "$FPV_PLUGIN_STATUS" ] && bad "status file left after stop" || ok "status file removed on stop"

check "status: one line per plugin" "S10first: ?|S20second: running|S30slow: ?|" \
	"$($P status "$D" 2> /dev/null | tr '\n' '|')"

# fpv integration: `fpv plugins` and the status field
fpv() { FPV_ROOT="$T/root" $SH "$PWD/files/fpv" "$@" 2> /dev/null; }
FPV_PLUGIN_STATUS=
echo "plugins=3 started=2" > "$T/root/tmp/plugins.status"
fpv status --json | grep -q '"plugins":"2/3"' && ok "fpv status: plugins field" || bad "fpv status: $(fpv status --json)"
fpv plugins --json | grep -q '{"name":"S20second","status":"running"}' && ok "fpv plugins --json" \
	|| bad "fpv plugins --json: $(fpv plugins --json)"
rm -rf "$D"/*
check "fpv plugins: none" "no plugins in $T/root/etc/fpv/plugins.d" "$(fpv plugins)"
$P start "$D" && ok "start with no plugins" || bad "start with no plugins"

[ "$fails" = 0 ] && echo "plugins ($SH): all passed" || { echo "plugins ($SH): $fails FAILED"; exit 1; }
