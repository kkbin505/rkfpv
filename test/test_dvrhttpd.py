#!/usr/bin/env python3
"""Tests for src/dvrhttpd.c on a scratch directory.

    python3 -I test/test_dvrhttpd.py build-host/dvrhttpd
"""
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1]
PORT = 28000 + os.getpid() % 1000
ORIGIN = "https://web.example"
passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("ok  ", name)
    else:
        failed += 1
        print("FAIL", name, detail)


def req(method, path, headers=None, raw_path=False, body=None):
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    c.putrequest(method, path, skip_accept_encoding=True, skip_host="Host" in (headers or {}))
    for k, v in (headers or {}).items():
        c.putheader(k, v)
    if body is not None:
        c.putheader("Content-Length", str(len(body)))
    c.endheaders(body)
    r = c.getresponse()
    body = r.read()
    c.close()
    return r.status, dict(r.getheaders()), body


def main():
    d = tempfile.mkdtemp()
    outside = tempfile.mkdtemp()
    try:
        data = os.urandom(300000)
        with open(f"{d}/rec_0001_00.mp4", "wb") as f:
            f.write(data)
        with open(f"{d}/rec_0002_01.ts", "wb") as f:
            f.write(b"ts" * 10)
        with open(f"{d}/rec_0003_00.mp4", "wb") as f:
            f.write(b"recording")
        with open(f"{d}/notes.txt", "w") as f:
            f.write("keep")
        with open(f"{outside}/secret", "w") as f:
            f.write("secret")
        os.symlink(f"{outside}/secret", f"{d}/rec_0009_00.mp4")      # must not escape
        status = f"{d}/dvr.status"
        with open(status, "w") as f:
            f.write("state=recording file=rec_0003_00.mp4 bytes=9 drops=0 free_mb=100\n")

        # fake fpv: echoes its arguments as JSON
        fake_fpv = f"{outside}/fpv"
        with open(fake_fpv, "w") as f:
            f.write('#!/bin/sh\nprintf \'{"ok":true,"args":"%s"}\\n\' "$*"\n')
        os.chmod(fake_fpv, 0o755)
        import socket
        pv = socket.socket()
        pv.bind(("127.0.0.1", 0))
        pv.listen(1)
        pv_port = pv.getsockname()[1]

        p = subprocess.Popen([BIN, "-b", "127.0.0.1", "-p", str(PORT), "-d", d, "-s", status,
                              "-o", ORIGIN, "-f", fake_fpv, "-P", str(pv_port)],
                             stderr=subprocess.PIPE)
        time.sleep(0.4)

        st, h, b = req("GET", "/")
        check("page", st == 200 and b"rkfpv" in b and h.get("Content-Type", "").startswith("text/html"))

        st, h, b = req("GET", "/api/list")
        j = json.loads(b)
        names = sorted(f["name"] for f in j["files"])
        check("list: recordings only, no symlink",
              names == ["rec_0001_00.mp4", "rec_0002_01.ts", "rec_0003_00.mp4"], names)
        check("list: recording reported", j["recording"] == "rec_0003_00.mp4", j)
        check("list: sizes", {f["name"]: f["size"] for f in j["files"]}["rec_0001_00.mp4"] == 300000)

        st, h, b = req("GET", "/dvr/rec_0001_00.mp4")
        check("download whole file", st == 200 and b == data and h["Content-Type"] == "video/mp4", st)
        st, h, b = req("HEAD", "/dvr/rec_0001_00.mp4")
        check("HEAD", st == 200 and b == b"" and h["Content-Length"] == "300000")
        st, h, b = req("GET", "/dvr/rec_0001_00.mp4", {"Range": "bytes=1000-1999"})
        check("range a-b", st == 206 and b == data[1000:2000] and h["Content-Range"] == "bytes 1000-1999/300000", st)
        st, h, b = req("GET", "/dvr/rec_0001_00.mp4", {"Range": "bytes=299000-"})
        check("range a- (resume)", st == 206 and b == data[299000:], st)
        st, h, b = req("GET", "/dvr/rec_0001_00.mp4", {"Range": "bytes=-500"})
        check("range -n", st == 206 and b == data[-500:], st)
        st, h, b = req("GET", "/dvr/rec_0001_00.mp4", {"Range": "bytes=400000-"})
        check("range past the end -> 416", st == 416, st)
        st, h, b = req("GET", "/dvr/rec_0002_01.ts")
        check("ts content type", st == 200 and h["Content-Type"] == "video/mp2t")

        # captive portal: phones' connectivity checks go to the page
        for host, path in [("connectivitycheck.gstatic.com", "/generate_204"),
                           ("captive.apple.com", "/hotspot-detect.html"),
                           ("www.msftconnecttest.com", "/connecttest.txt"),
                           ("example.com", "/"), ("evil.example", "/api/list")]:
            st, h, b = req("GET", path, {"Host": host})
            check(f"portal: {host}{path} -> 302 to the page",
                  st == 302 and h.get("Location") == f"http://127.0.0.1:{PORT}/", (st, h.get("Location")))
        st, h, b = req("DELETE", "/dvr/rec_0001_00.mp4", {"Host": "evil.example", "X-FPV-Confirm": "1"})
        check("portal: foreign Host cannot delete", st == 302 and os.path.exists(f"{d}/rec_0001_00.mp4"), st)
        st, h, b = req("GET", "/api/list", {"Host": f"127.0.0.1:{PORT}"})
        check("own address with port is served", st == 200, st)

        for bad in ["/dvr/../notes.txt", "/dvr/notes.txt", "/dvr/rec_1.mp4", "/dvr/rec_0001_00.mp4.bak",
                    "/dvr/%2e%2e/notes.txt", "/dvr/rec_0009_00.mp4", "/etc/passwd", "/dvr/"]:
            st, h, b = req("GET", bad)
            check(f"refused {bad}", st == 404 and b"secret" not in b and b"keep" not in b, st)

        st, h, b = req("DELETE", "/dvr/rec_0002_01.ts")
        check("delete without confirm header -> 403", st == 403 and os.path.exists(f"{d}/rec_0002_01.ts"), st)
        st, h, b = req("DELETE", "/dvr/rec_0002_01.ts", {"X-FPV-Confirm": "1"})
        check("delete with confirm", st == 200 and not os.path.exists(f"{d}/rec_0002_01.ts"), st)
        st, h, b = req("DELETE", "/dvr/rec_0003_00.mp4", {"X-FPV-Confirm": "1"})
        check("file being recorded is kept", st == 409 and os.path.exists(f"{d}/rec_0003_00.mp4"), st)
        st, h, b = req("DELETE", "/dvr/notes.txt", {"X-FPV-Confirm": "1"})
        check("delete non-recording refused", st == 404 and os.path.exists(f"{d}/notes.txt"), st)
        st, h, b = req("DELETE", "/dvr/rec_0009_00.mp4", {"X-FPV-Confirm": "1"})
        check("delete through symlink refused", st == 404 and os.path.exists(f"{outside}/secret"), st)
        st, h, b = req("OPTIONS", "/api/delete-all", {"Origin": "http://evil", "Access-Control-Request-Method": "POST"})
        check("no CORS preflight answer", "Access-Control-Allow-Origin" not in h, h)
        st, h, b = req("GET", "/api/delete-all")
        check("delete-all needs POST", st == 405 and os.path.exists(f"{d}/rec_0001_00.mp4"), st)
        st, h, b = req("POST", "/api/delete-all")
        check("delete-all without confirm -> 403", st == 403 and os.path.exists(f"{d}/rec_0001_00.mp4"), st)
        st, h, b = req("POST", "/api/delete-all", {"X-FPV-Confirm": "1"})
        left = sorted(os.listdir(d))
        check("delete-all: recordings gone, others kept",
              st == 200 and json.loads(b)["deleted"] == 1 and
              left == ["dvr.status", "notes.txt", "rec_0003_00.mp4", "rec_0009_00.mp4"], left)

        # ---- web configurator: CORS for one origin, /api/fpv, /live
        pre = {"Origin": ORIGIN, "Access-Control-Request-Method": "POST",
               "Access-Control-Request-Headers": "x-fpv-confirm",
               "Access-Control-Request-Private-Network": "true"}
        st, h, b = req("OPTIONS", "/api/fpv", pre)
        check("preflight: configured origin allowed",
              st == 204 and h.get("Access-Control-Allow-Origin") == ORIGIN and
              "X-FPV-Confirm" in h.get("Access-Control-Allow-Headers", "") and
              h.get("Access-Control-Allow-Private-Network") == "true", (st, h))
        st, h, b = req("OPTIONS", "/api/fpv", dict(pre, Origin="https://evil.example"))
        check("preflight: other origin refused", st == 403 and "Access-Control-Allow-Origin" not in h, (st, h))
        st, h, b = req("GET", "/api/list", {"Origin": ORIGIN})
        check("list: CORS header for the origin", h.get("Access-Control-Allow-Origin") == ORIGIN, h)
        st, h, b = req("GET", "/api/list", {"Origin": "https://evil.example"})
        check("list: no CORS header for others", "Access-Control-Allow-Origin" not in h, h)

        st, h, b = req("POST", "/api/fpv", {"Origin": ORIGIN}, body=b"status")
        check("fpv: needs the confirm header", st == 403, st)
        st, h, b = req("GET", "/api/fpv", {"X-FPV-Confirm": "1"})
        check("fpv: needs POST", st == 405, st)
        H = {"X-FPV-Confirm": "1", "Origin": ORIGIN, "Content-Type": "text/plain"}
        st, h, b = req("POST", "/api/fpv", H, body=b"status")
        check("fpv: runs fpv status --json",
              st == 200 and json.loads(b) == {"ok": True, "args": "status --json"} and
              h.get("Access-Control-Allow-Origin") == ORIGIN, (st, b))
        st, h, b = req("POST", "/api/fpv", H, body=b"set bitrate 4000\n")
        check("fpv: set with arguments", json.loads(b).get("args") == "set bitrate 4000 --json", b)
        for body, why in [(b"reboot", "not allowed"), (b"get ;reboot", "invalid characters"),
                          (b"set ssid $(reboot)", "invalid characters"), (b"", "no command")]:
            st, h, b = req("POST", "/api/fpv", H, body=body)
            check(f"fpv: {body!r} refused", json.loads(b).get("ok") is False and why in json.loads(b)["error"], b)
        st, h, b = req("POST", "/api/fpv", H, body=b"get " + b"a " * 3000)
        check("fpv: body too large -> 413", st == 413, st)
        st, h, b = req("POST", "/api/fpv", H, body=b"get" + b" bitrate" * 40)
        check("fpv: too many arguments refused", json.loads(b).get("ok") is False, b)

        st, h, b = req("GET", "/dvr/rec_0001_00.mp4?dl=1")
        check("?dl=1 -> attachment", st == 200 and h.get("Content-Disposition", "").startswith("attachment"), h)

        # /live relays the preview server's bytes; nothing listening -> 503
        import threading
        frames = os.urandom(200000)

        def serve_preview():
            c, _ = pv.accept()
            c.sendall(frames)
            c.close()
        t = threading.Thread(target=serve_preview, daemon=True)
        t.start()
        st, h, b = req("GET", "/live", {"Origin": ORIGIN})
        check("live: relays the stream",
              st == 200 and b == frames and h.get("Content-Type") == "application/octet-stream" and
              h.get("Access-Control-Allow-Origin") == ORIGIN, (st, len(b)))
        t.join(5)
        pv.close()
        st, h, b = req("GET", "/live")
        check("live: no preview -> 503", st == 503, st)

        # several downloads at once: one long transfer must not block the list
        big = f"{d}/rec_0004_00.mp4"
        with open(big, "wb") as f:
            f.write(os.urandom(8 << 20))
        slow = socket.create_connection(("127.0.0.1", PORT))
        slow.sendall(b"GET /dvr/rec_0004_00.mp4 HTTP/1.1\r\nHost: x\r\n\r\n")   # read nothing yet
        t0 = time.time()
        st, h, b = req("GET", "/api/list")
        check("list answers while a download is open", st == 200 and time.time() - t0 < 2)
        slow.close()

        p.terminate()
        _, err = p.communicate(timeout=5)
        check("no sanitizer report", b"ERROR: AddressSanitizer" not in err and b"runtime error" not in err,
              err[-400:])
    finally:
        shutil.rmtree(d)
        shutil.rmtree(outside)
    print(f"{passed}/{passed + failed} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
