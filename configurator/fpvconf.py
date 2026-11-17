#!/usr/bin/env python3
"""rkfpv configurator: a local web UI that drives the board's `fpv` command.

    python3 fpvconf.py [--port 8765] [--no-browser]

Talks to the board over ADB (board booted with the PC on its USB port) or,
once the wfb tunnel exists, over SSH. Every action is an `fpv ... --json`
command, so anything the UI does can also be typed in `adb shell`.

Python 3.8+ standard library only; needs `adb` (or `ssh`) on PATH.
"""
import argparse
import json
import os
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
UI_DIR = os.path.join(HERE, "ui")
REPO = os.path.dirname(HERE)
TIMEOUT = 20
# a page from another site could POST to localhost; it cannot read this token
TOKEN = secrets.token_hex(16)

# files `install` copies to the board: (repo path, board path, mode)
BOARD_FILES = [
    ("files/fpv", "/usr/bin/fpv", "755"),
    ("files/S99fpv", "/etc/init.d/S99fpv", "755"),
    ("files/wfb.sh", "/usr/bin/wfb.sh", "755"),
]
DEFAULT_FILES = [("files/rkfpv.conf", "rkfpv.conf"), ("files/wfb.yaml", "wfb.yaml")]


class TransportError(Exception):
    pass


class Transport:
    """Runs a shell command line on the board."""

    def shell(self, cmdline, timeout=TIMEOUT):
        raise NotImplementedError

    def push(self, local, remote):
        raise NotImplementedError

    def _run(self, argv, timeout):
        try:
            p = subprocess.run(argv, capture_output=True, timeout=timeout)
        except FileNotFoundError:
            raise TransportError(f"'{argv[0]}' not found on PATH")
        except subprocess.TimeoutExpired:
            raise TransportError("board did not answer (timeout)")
        out = p.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
        err = p.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
        return p.returncode, out, err


class Adb(Transport):
    def __init__(self, serial=None):
        self.serial = serial

    def _base(self):
        return ["adb"] + (["-s", self.serial] if self.serial else [])

    def shell(self, cmdline, timeout=TIMEOUT):
        rc, out, err = self._run(self._base() + ["shell", cmdline], timeout)
        if rc != 0 and not out and ("device" in err or "no devices" in err):
            raise TransportError(err.strip() or "adb failed")
        return rc, out, err

    def push(self, local, remote):
        rc, out, err = self._run(self._base() + ["push", local, remote], 60)
        if rc != 0:
            raise TransportError((err or out).strip())

    @staticmethod
    def devices():
        if not shutil.which("adb"):
            return {"ok": False, "error": "adb not found on PATH"}
        p = subprocess.run(["adb", "devices", "-l"], capture_output=True, timeout=10)
        devs = []
        for line in p.stdout.decode("utf-8", "replace").splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 2:
                info = dict(x.split(":", 1) for x in parts[2:] if ":" in x)
                devs.append({"serial": parts[0], "state": parts[1],
                             "model": info.get("model", info.get("product", ""))})
        return {"ok": True, "devices": devs}


class Ssh(Transport):
    """Key-based login only (BatchMode): the server cannot type a password."""

    def __init__(self, host):
        self.host = host

    def _base(self):
        return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                "-o", "StrictHostKeyChecking=accept-new", f"root@{self.host}"]

    def shell(self, cmdline, timeout=TIMEOUT):
        rc, out, err = self._run(self._base() + [cmdline], timeout)
        if rc == 255:
            raise TransportError(err.strip() or "ssh failed")
        return rc, out, err

    def push(self, local, remote):
        rc, out, err = self._run(["scp", "-o", "BatchMode=yes", local,
                                  f"root@{self.host}:{remote}"], 60)
        if rc != 0:
            raise TransportError((err or out).strip())


def transport_for(req):
    kind = req.get("transport", "adb")
    if kind == "adb":
        return Adb(req.get("device") or None)
    if kind == "ssh":
        host = req.get("host") or "10.5.0.10"
        return Ssh(host)
    raise TransportError(f"unknown transport {kind}")


def run_fpv(t, args):
    cmdline = "fpv " + " ".join(shlex.quote(str(a)) for a in args) + " --json"
    rc, out, err = t.shell(cmdline)
    text = out.strip()
    if "not found" in (text + err) and not text.startswith("{"):
        return {"ok": False, "error": "fpv is not installed on the board",
                "missing_fpv": True}
    # the JSON is the last line; anything before it is stray output
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                break
    return {"ok": False, "error": (text or err.strip() or f"exit {rc}")[:2000]}


def run_cli(t, line):
    line = line.strip()
    if not line:
        return {"ok": True, "output": ""}
    rc, out, err = t.shell(line)
    return {"ok": True, "rc": rc, "output": (out + err).rstrip("\n")}


def push_lf(t, rel, remote):
    """Push a repo file with LF line endings: a Windows checkout may have
    CRLF, and the board's shell cannot run a script with CR in it."""
    with open(os.path.join(REPO, rel), "rb") as f:
        data = f.read().replace(b"\r\n", b"\n")
    fd, tmp = tempfile.mkstemp(prefix="fpvconf-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        t.push(tmp, remote)
    finally:
        os.unlink(tmp)


def install(t):
    for rel, remote, mode in BOARD_FILES:
        push_lf(t, rel, remote)
        t.shell(f"chmod {mode} {shlex.quote(remote)}")
    # shipped defaults for `fpv reset`, only if the image did not bring them
    t.shell("mkdir -p /etc/fpv/defaults")
    for rel, name in DEFAULT_FILES:
        rc, out, _ = t.shell(f"[ -f /etc/fpv/defaults/{name} ] && echo yes")
        if "yes" not in out:
            push_lf(t, rel, f"/etc/fpv/defaults/{name}")
    t.shell("sync")
    return {"ok": True, "installed": [r for _, r, _ in BOARD_FILES]}


class Handler(BaseHTTPRequestHandler):
    server_version = "fpvconf"

    def log_message(self, fmt, *a):
        if os.environ.get("FPVCONF_DEBUG"):
            super().log_message(fmt, *a)

    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj))

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/":
            path = "/index.html"
        if path == "/api/devices":
            if self.headers.get("X-Token") != TOKEN:
                return self._json({"ok": False, "error": "bad token"}, 403)
            return self._json(Adb.devices())
        name = os.path.normpath(path.lstrip("/"))
        full = os.path.join(UI_DIR, name)
        if name.startswith("..") or not os.path.isfile(full):
            return self._send(404, "not found", "text/plain")
        ctype = {".html": "text/html; charset=utf-8", ".js": "text/javascript",
                 ".css": "text/css", ".svg": "image/svg+xml"}.get(
            os.path.splitext(full)[1], "application/octet-stream")
        with open(full, "rb") as f:
            data = f.read()
        if name == "index.html":
            data = data.replace(b"__TOKEN__", TOKEN.encode())
        self._send(200, data, ctype)

    def do_POST(self):
        if self.headers.get("X-Token") != TOKEN:
            return self._json({"ok": False, "error": "bad token"}, 403)
        try:
            n = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(n) or b"{}")
            t = transport_for(req)
            if self.path == "/api/fpv":
                args = req.get("args") or []
                if not isinstance(args, list):
                    raise TransportError("args must be a list")
                return self._json(run_fpv(t, args))
            if self.path == "/api/cli":
                return self._json(run_cli(t, str(req.get("line", ""))))
            if self.path == "/api/install":
                return self._json(install(t))
            return self._json({"ok": False, "error": "no such endpoint"}, 404)
        except TransportError as e:
            return self._json({"ok": False, "error": str(e), "transport_error": True})
        except Exception as e:  # keep the server alive, show it in the UI
            return self._json({"ok": False, "error": f"{type(e).__name__}: {e}"})


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    url = f"http://127.0.0.1:{a.port}/"
    print(f"rkfpv configurator on {url}  (Ctrl+C to quit)")
    if not shutil.which("adb"):
        print("warning: adb not found on PATH", file=sys.stderr)
    if not a.no_browser:
        threading.Timer(0.5, webbrowser.open, (url,)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
