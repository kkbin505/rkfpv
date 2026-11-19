#!/usr/bin/env python3
"""Tests for fpvd against the real files/fpv on a scratch FPV_ROOT.

    python3 -I test/test_fpvd.py build-host/fpvd files/fpv
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

FPVD, FPV = sys.argv[1], os.path.abspath(sys.argv[2])
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 27000 + os.getpid() % 1000

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("ok  ", name)
    else:
        failed += 1
        print("FAIL", name, detail)


def request(sock, rid, line, drop=()):
    """send one request, collect chunks; `drop` = seq numbers to ignore
    (simulated radio loss), so the caller can retry with the same id"""
    sock.sendto(f"FPV1 {rid} {line}".encode(), ("127.0.0.1", PORT))
    parts, total = {}, None
    deadline = time.time() + 5
    while time.time() < deadline:
        sock.settimeout(max(0.05, deadline - time.time()))
        try:
            data, _ = sock.recvfrom(65535)
        except socket.timeout:
            break
        head, _, body = data.partition(b"\n")
        magic, got_id, frac = head.decode().split(" ")
        if magic != "FPV1" or got_id != rid:
            continue
        seq, total = map(int, frac.split("/"))
        if seq in drop:
            continue
        parts[seq] = body
        if len(parts) == total:
            break
    if total is None or len(parts) != total:
        return None, parts, total
    return json.loads(b"".join(parts[i] for i in range(1, total + 1))), parts, total


def main():
    root = tempfile.mkdtemp()
    try:
        os.makedirs(f"{root}/etc/fpv/defaults")
        os.makedirs(f"{root}/userdata/log")
        for f in ("rkfpv.conf", "wfb.yaml"):
            shutil.copy(f"{REPO}/files/{f}", f"{root}/etc/")
        # fake fpv that counts executions, then runs the real one
        counter = f"{root}/count"
        wrapper = f"{root}/fpv"
        with open(wrapper, "w") as w:
            w.write(f"#!/bin/sh\necho x >> {counter}\nexec sh {FPV} \"$@\"\n")
        os.chmod(wrapper, 0o755)
        env = dict(os.environ, FPV_ROOT=root)
        d = subprocess.Popen([FPVD, "-p", str(PORT), "-b", "127.0.0.1", "-f", wrapper],
                             env=env, stderr=subprocess.PIPE)
        time.sleep(0.4)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        runs = lambda: open(counter).read().count("x") if os.path.exists(counter) else 0

        r, _, _ = request(s, "p1", "ping")
        check("ping", r == {"ok": True, "pong": True}, r)
        check("ping does not run fpv", runs() == 0)

        r, _, _ = request(s, "g1", "get bitrate")
        check("get bitrate", r and r["values"] == {"bitrate": "3500"}, r)

        r, parts, total = request(s, "s1", "schema")
        check("schema arrives in several chunks", total and total > 1, total)
        check("schema reassembles to JSON", r and len(r["params"]) >= 20, r and len(r["params"]))
        check("chunks fit a radio packet", all(len(p) <= 1200 for p in parts.values()))

        r, _, _ = request(s, "w1", "set bitrate 4200 mcs_index 3")
        check("set", r and r["ok"] and [x["key"] for x in r["results"]] == ["bitrate", "mcs_index"], r)
        n = runs()
        r2, _, _ = request(s, "w1", "set bitrate 4200 mcs_index 3")
        check("repeated id answered from cache", r2 == r and runs() == n, (runs(), n))
        r, _, _ = request(s, "g2", "get bitrate mcs_index")
        check("value written", r and r["values"] == {"bitrate": "4200", "mcs_index": "3"}, r)

        # lost chunk -> retry with the same id -> complete, fpv not re-run
        n = runs()
        r, parts, total = request(s, "s2", "schema", drop=(2,))
        check("lost chunk leaves reply incomplete", r is None and total and 2 not in parts)
        r, _, _ = request(s, "s2", "schema")
        check("retry completes from cache", r and r["ok"] and runs() == n + 1, runs() - n)

        r, _, _ = request(s, "e1", "set bitrate 999999")
        check("fpv error passed through", r and r["ok"] is False and "range" in r["error"], r)
        r, _, _ = request(s, "e2", "sh -c reboot")
        check("non-fpv command refused", r and r["ok"] is False and "not allowed" in r["error"], r)
        n = runs()
        for i, bad in enumerate(["get $(reboot)", "get `id`", "set bitrate 1;reboot", "get a|b", "log ../../etc/x"]):
            r, _, _ = request(s, f"x{i}", bad)
            check(f"rejected: {bad}", r and r["ok"] is False and "invalid" in r["error"], r)
        check("rejected requests never ran fpv", runs() == n)

        s.sendto(b"HELLO", ("127.0.0.1", PORT))
        s.sendto(b"FPV1 bad/id get", ("127.0.0.1", PORT))
        s.settimeout(0.3)
        try:
            s.recvfrom(100)
            check("garbage ignored", False)
        except socket.timeout:
            check("garbage ignored", True)
        r, _, _ = request(s, "after", "ping")
        check("still alive after garbage", r == {"ok": True, "pong": True})

        d.terminate()
        _, err = d.communicate(timeout=5)
        check("clean exit, no sanitizer report", d.returncode == 0 and b"ERROR" not in err, err[-500:])
    finally:
        shutil.rmtree(root)
    print(f"{passed}/{passed + failed} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
