#!/bin/sh
# Host test: bbsim (simulated Betaflight serial blackbox) -> pty -> bblog ->
# .bbl named after the recording; the file must equal the logs as sent and
# decode (test/bbdecode.py) to exactly the values bbsim logged. The same
# chain as test/bb_hil.sh on the board, minus the board.
#
#   test/test_bbsim.sh build-host/bbsim build-host/bblog
set -u
SIM=$1 BBLOG=$2
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "ok   $1"; }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

# one run: bbsim with "$@", bblog reads its pty until bbsim is done
run() {
	rm -rf "$T/out" "$T/ref"; mkdir -p "$T/out" "$T/ref"
	echo "state=recording file=rec_0042_07.mp4 bytes=1 drops=0 free_mb=1000" > "$T/dvr.status"
	"$SIM" -p "$T/uart" -W 500 -w "$T/ref" "$@" > "$T/sim.log" 2>&1 &
	sim=$!
	n=0; while [ ! -e "$T/uart" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
	"$BBLOG" -d "$T/uart" -o "$T/out" -s "$T/dvr.status" -S "$T/bblog.status" -t 1000 > "$T/bblog.log" 2>&1 &
	bl=$!
	wait $sim
	sleep 1.5; kill $bl 2>/dev/null; wait $bl 2>/dev/null
	cat "$T"/ref/ref_*.bbl > "$T/sent.bbl"
}

# 1) two flights with noise before each, both end with the log-end event
run -n 2 -s 2 -r 2000 -g 1500 -j 64
f=$T/out/rec_0042_07.bbl
[ -f "$f" ] && ok "saved as rec_0042_07.bbl (named after the recording)" || bad "no rec_0042_07.bbl: $(ls "$T/out")"
cmp -s "$T/sent.bbl" "$f" && ok "file equals both logs as sent, noise dropped" || bad "file differs from what was sent"
grep -q "logs=2 dropped=128" "$T/bblog.status" && ok "2 logs, 128 noise bytes dropped" || bad "status: $(cat "$T/bblog.status")"
python3 -I "$HERE/bbdecode.py" "$f" --check "$T/ref/ref_001.csv" "$T/ref/ref_002.csv" > "$T/dec.log" \
	&& ok "decodes to the values sent ($(grep -c '^  ok' "$T/dec.log") logs, $(grep -o '[0-9]* decoded frames' "$T/dec.log" | head -1))" \
	|| { bad "decode"; cat "$T/dec.log"; }

# 2) a corrupted copy must not pass (the check really compares)
python3 -c "import sys; d=bytearray(open(sys.argv[1],'rb').read()); d[len(d)//2]^=0x40; open(sys.argv[2],'wb').write(d)" "$f" "$T/bad.bbl"
python3 -I "$HERE/bbdecode.py" "$T/bad.bbl" --check "$T/ref/ref_001.csv" "$T/ref/ref_002.csv" > /dev/null \
	&& bad "a flipped byte was not noticed" || ok "a flipped byte is caught"

# 3) cable pulled: no log-end event, bblog ends the log after its timeout
run -n 1 -s 2 -r 2000 -g 2500 -c
grep -q "ended (timeout)" "$T/bblog.log" && ok "cut log ended by the timeout" || bad "cut log: $(cat "$T/bblog.log")"
cmp -s "$T/sent.bbl" "$T/out/rec_0042_07.bbl" && ok "cut log saved complete" || bad "cut log differs"
python3 -I "$HERE/bbdecode.py" "$T/out/rec_0042_07.bbl" --check "$T/ref/ref_001.csv" > "$T/dec.log" \
	&& grep -q "NO log end" "$T/dec.log" && ok "cut log decodes up to the cut" || { bad "cut log decode"; cat "$T/dec.log"; }

echo "$pass/$((pass + fail)) passed"
[ $fail = 0 ]
