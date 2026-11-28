#!/bin/sh
# Blackbox hardware-in-the-loop test on a board over ADB, no flight
# controller and no wiring: bbsim plays the FC on a pseudo-terminal, the
# board's bblog reads it like UART4 and saves to the TF card next to the
# recording rkfpv is writing; the PC then checks the saved .bbl byte for
# byte against what was sent and decodes every value (test/bbdecode.py).
#
#   make build/bblog build/bbsim build-host/bbsim && test/bb_hil.sh [adb serial]
#   BB_UART=/dev/ttyS4 [BB_BAUD=1500000] test/bb_hil.sh    over the real UART:
#       jumper its TX to its RX (Mini/Pro UART4: GPIO1_C5 -> GPIO1_C4);
#       nothing else may hold the port (the board's own bblog stopped)
#
# Needs the video running with DVR recording (fpv set dvr 1; fpv preview on
# or FPV mode). Saves into /mnt/dvr/bbhil/ (never next to a real flight's
# log), named after the running recording as in flight. Runs:
#   r1  2 flights x 20 s at 2 kHz, noise before each   (a normal FC)
#   r2  1 flight x 15 s at 8 kHz                       (~4x a normal FC)
#   r3  1 flight x 5 s, no log end                     (cable pulled)
# The board's own bblog is not stopped or touched; the test copies run
# from /tmp and are removed afterwards. The .bbl stays on the card.
# Nothing but the .bbl is written on the board: the reference copies of what
# was sent (ref_NNN.bbl / .csv, several MB) are made on the PC by the host
# bbsim with the same options while the board runs (its output depends only
# on the options). On the board's RAM-only /tmp they hang a Pico Mini, and
# on its TF card, next to the DVR and bblog, they made the card time out.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
ADB="adb${1:+ -s $1}"
for b in bblog bbsim; do
	[ -f "$HERE/build/$b" ] || { echo "missing build/$b: make build/bblog build/bbsim" >&2; exit 1; }
done
[ -x "$HERE/build-host/bbsim" ] || { echo "missing build-host/bbsim: make build-host/bbsim" >&2; exit 1; }
$ADB get-state > /dev/null
st=$($ADB shell cat /tmp/dvr.status 2>/dev/null | tr -d '\r')
case "$st" in
	state=recording*) echo "dvr: $st" ;;
	*) echo "DVR is not recording (fpv set dvr 1, then start the video): '${st:-no /tmp/dvr.status}'" >&2; exit 1 ;;
esac
L=$(mktemp -d)
trap 'rm -rf "$L"' EXIT
$ADB push "$HERE/build/bblog" /tmp/hil-bblog > /dev/null
$ADB push "$HERE/build/bbsim" /tmp/hil-bbsim > /dev/null
cat > "$L/run.sh" <<'BOARD'
#!/bin/sh
R=/tmp/hil-log; O=/mnt/dvr/bbhil
rm -rf $R; mkdir -p $R $O
chmod 755 /tmp/hil-bblog /tmp/hil-bbsim
run() {
	name=$1; shift
	mkdir -p $R/$name
	if [ -n "$UART" ]; then
		# real port with TX and RX jumpered: the reader must be listening first
		/tmp/hil-bblog -d $UART -b $BAUD -o $O -s /tmp/dvr.status -S /tmp/hil-bblog.status -t 2000 > $R/$name/bblog.log 2>&1 &
		bl=$!
		usleep 500000
		/tmp/hil-bbsim -d $UART -b $BAUD -W 1000 "$@" > $R/$name/sim.log 2>&1 &
		sim=$!
	else
		/tmp/hil-bbsim -p /tmp/hil-uart -W 1500 "$@" > $R/$name/sim.log 2>&1 &
		sim=$!
		n=0; while [ ! -e /tmp/hil-uart ] && [ $n -lt 50 ]; do usleep 100000; n=$((n + 1)); done
		/tmp/hil-bblog -d /tmp/hil-uart -o $O -s /tmp/dvr.status -S /tmp/hil-bblog.status -t 2000 > $R/$name/bblog.log 2>&1 &
		bl=$!
	fi
	sleep 6
	cpu=$(top -b -n 1 | awk '/[h]il-bblog -d/ {print $7}')
	wait $sim
	sleep 1; kill $bl 2>/dev/null; wait $bl 2>/dev/null
	echo "$name: $(grep -o '[0-9]* frames.*' $R/$name/sim.log | tr '\n' ' ') bblog cpu ${cpu:-?}"
	grep -E "started|ended" $R/$name/bblog.log | sed 's/^/    /'
}
run r1 -n 2 -s 20 -r 2000 -g 3000
run r2 -n 1 -s 15 -r 8000 -g 3000
run r3 -n 1 -s 5 -r 2000 -g 4000 -c
echo "card: $(ls -l $O | awk '/\.bbl/ {print $NF, $5 " bytes"}' | tail -1)"
echo "video: $(sed 's/ bytes=[0-9]*//' /tmp/dvr.status)"
BOARD
$ADB push "$L/run.sh" /tmp/hil-run.sh > /dev/null
# the same runs on the PC, only to write the reference copies
hostrefs() {
	for spec in "r1 -n 2 -s 20 -r 2000 -g 3000" "r2 -n 1 -s 15 -r 8000 -g 3000" "r3 -n 1 -s 5 -r 2000 -g 4000 -c"; do
		set -- $spec; name=$1; shift
		mkdir -p "$L/hil-ref/$name"
		"$HERE/build-host/bbsim" -p "$L/pty-$name" -W 300 -w "$L/hil-ref/$name" "$@" &
		s=$!
		n=0; while [ ! -e "$L/pty-$name" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
		cat "$L/pty-$name" > /dev/null &
		c=$!
		wait $s; kill $c 2>/dev/null || true
	done
}
if [ -n "${BB_UART:-}" ]; then
	$ADB shell "[ -c $BB_UART ]" || { echo "no $BB_UART on the board" >&2; exit 1; }
	echo "link: $BB_UART @ ${BB_BAUD:-1500000} (TX jumpered to RX)"
else
	echo "link: pseudo-terminal"
fi
hostrefs > "$L/hostref.log" 2>&1 &
hostref=$!
$ADB shell "UART=${BB_UART:-} BAUD=${BB_BAUD:-1500000} sh /tmp/hil-run.sh" | tr -d '\r' | tee "$L/board.log"
f=$(sed -n 's/^card: \(rec_[0-9_]*\.bbl\).*/\1/p' "$L/board.log")
[ -n "$f" ] || { echo "FAIL: no .bbl on the card"; exit 1; }
$ADB pull "/mnt/dvr/bbhil/$f" "$L/" > /dev/null
wait $hostref
$ADB shell 'rm -rf /tmp/hil-log /tmp/hil-bblog /tmp/hil-bbsim /tmp/hil-run.sh /tmp/hil-bblog.status /tmp/hil-uart'
R=$L/hil-ref
fail=0
# a .bbl named after this recording may already hold an earlier HIL run: check its tail
cat "$R"/r1/ref_001.bbl "$R"/r1/ref_002.bbl "$R"/r2/ref_001.bbl "$R"/r3/ref_001.bbl > "$L/sent.bbl"
n=$(wc -c < "$L/sent.bbl"); total=$(wc -c < "$L/$f")
tail -c "$n" "$L/$f" | cmp -s - "$L/sent.bbl" && echo "ok   /mnt/dvr/bbhil/$f: the 4 logs as sent, byte for byte ($n bytes)" \
	|| { echo "FAIL /mnt/dvr/bbhil/$f differs from what was sent"; fail=1; }
tail -c "$n" "$L/$f" > "$L/this.bbl"
python3 -I "$HERE/test/bbdecode.py" "$L/this.bbl" --check "$R"/r1/ref_001.csv "$R"/r1/ref_002.csv \
	"$R"/r2/ref_001.csv "$R"/r3/ref_001.csv > "$L/decode.log" || fail=1
sed 's/^/     /' "$L/decode.log"
[ "$total" -gt "$n" ] && echo "     (the file also holds $((total - n)) bytes of an earlier HIL run in the same recording)"
[ $fail = 0 ] && echo "blackbox HIL: passed" || {
	K=$(mktemp -d "${TMPDIR:-/tmp}/bb_hil.XXXXXX"); cp -a "$L"/. "$K"/
	echo "blackbox HIL: FAILED (files kept in $K)"; exit 1; }
