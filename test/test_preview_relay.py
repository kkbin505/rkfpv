#!/usr/bin/env python3
"""Tests the configurator's preview relay (configurator/fpvconf.py) against a
fake board port that behaves like `adb forward`: the first connections are
accepted and closed at once (rkfpv not listening yet), then records flow.

    python3 -I test/test_preview_relay.py
"""
import os
import socket
import sys
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "configurator"))
import fpvconf  # noqa: E402

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("ok  ", name)
    else:
        failed += 1
        print("FAIL", name, detail)


def records(n):
    out = b""
    for i in range(n):
        au = b"\0\0\0\1\x02\x01" + bytes([i % 256]) * (100 + i * 37)
        out += len(au).to_bytes(4, "big") + au
    return out


STREAM = records(300)


def fake_board(ls, refuse_first):
    for i in range(refuse_first + 1):
        c, _ = ls.accept()
        if i < refuse_first:
            c.close()                     # adb: nothing listening on the board yet
            continue
        for k in range(0, len(STREAM), 5000):
            c.sendall(STREAM[k:k + 5000])
            time.sleep(0.002)
        c.close()


ls = socket.socket()
ls.bind(("127.0.0.1", 0))
ls.listen(4)
board_port = ls.getsockname()[1]
threading.Thread(target=fake_board, args=(ls, 3), daemon=True).start()

srv = fpvconf.ThreadingHTTPServer(("127.0.0.1", 0), fpvconf.Handler)
threading.Thread(target=srv.serve_forever, daemon=True).start()
base = f"http://127.0.0.1:{srv.server_address[1]}"


def get(path, token=True):
    req = urllib.request.Request(base + path, headers={"X-Token": fpvconf.TOKEN} if token else {})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


code, _ = get(f"/api/preview/stream?port={board_port}", token=False)
check("stream needs the token", code == 403, code)
code, _ = get(f"/api/preview/stream?port={board_port}")
check("only ports the configurator forwarded", code == 404, code)

fpvconf.preview_ports.add(board_port)
t0 = time.time()
code, body = get(f"/api/preview/stream?port={board_port}")
check("retries until the board answers, then relays", code == 200 and body == STREAM,
      (code, len(body), len(STREAM)))
check("retry is quick", time.time() - t0 < 5, time.time() - t0)

# a port that never sends: the relay gives up instead of hanging forever
dead = socket.socket()
dead.bind(("127.0.0.1", 0))
dead_port = dead.getsockname()[1]
dead.close()                              # nothing listens there
fpvconf.preview_ports.add(dead_port)
real = time.time
calls = [0]


def fast_clock():                         # every look at the clock: 5 s later
    calls[0] += 1
    return real() + 5 * calls[0]


fpvconf.time.time = fast_clock
code, body = get(f"/api/preview/stream?port={dead_port}")
fpvconf.time.time = real
check("gives up on a board that never answers", code == 200 and body == b"", (code, body[:20]))

srv.shutdown()
print(f"{passed}/{passed + failed} passed")
sys.exit(1 if failed else 0)
