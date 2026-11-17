#!/usr/bin/env python3
"""rkfpv configurator: a local web UI that drives the board's `fpv` command.

    python3 fpvconf.py [--port 8765] [--no-browser]

Talks to the board over ADB (board booted with the PC on its USB port) or,
once the wfb tunnel exists, over SSH. Every action is an `fpv ... --json`
command, so anything the UI does can also be typed in `adb shell`.

Python 3.8+ standard library only; needs `adb` (or `ssh`) on PATH.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
UI_DIR = os.path.join(HERE, "ui")
PRESET_DIR = os.path.join(HERE, "presets")
DVR_BOARD_DIR = "/mnt/dvr"
DVR_NAME = re.compile(r"^rec_[0-9]+_[0-9]+\.(mp4|ts)$")
# where downloaded recordings go; --dvr-dir changes it
dvr_local_dir = os.path.join(os.path.expanduser("~"), "Videos", "rkfpv-dvr")
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


class Fpvd(Transport):
    """UDP to the board's fpvd (src/fpvd.c): `fpv` commands only, no shell.
    Works over any IP path: USB RNDIS (172.32.0.93) now, the wfb tunnel
    (10.5.0.10) later. Lost datagrams are handled by resending the request
    with the same id; fpvd answers a repeated id from its cache."""

    PORT = 7000
    TRIES = 4
    TRY_TIMEOUT = 4.0     # one try covers a slow `fpv schema` on the board

    def __init__(self, host):
        h, _, p = host.partition(":")
        self.addr = (h or "172.32.0.93", int(p) if p else self.PORT)

    def fpv(self, args):
        for a in args:
            a = str(a)
            if not a or any(not (c.isalnum() or c in "_.,:-") for c in a):
                return {"ok": False, "error": f"argument not allowed over fpvd: {a!r}"}
        rid = secrets.token_hex(6)
        req = ("FPV1 %s %s" % (rid, " ".join(map(str, args)))).encode()
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        parts, total = {}, None
        try:
            for _ in range(self.TRIES):
                s.sendto(req, self.addr)
                deadline = time.time() + self.TRY_TIMEOUT
                while time.time() < deadline:
                    s.settimeout(max(0.05, deadline - time.time()))
                    try:
                        data, _ = s.recvfrom(65535)
                    except socket.timeout:
                        break
                    except OSError as e:     # e.g. ICMP port unreachable
                        raise TransportError(f"fpvd at {self.addr[0]}:{self.addr[1]}: {e}")
                    head, _, body = data.partition(b"\n")
                    f = head.decode("ascii", "replace").split(" ")
                    if len(f) != 3 or f[0] != "FPV1" or f[1] != rid:
                        continue
                    seq, total = map(int, f[2].split("/"))
                    parts[seq] = body
                    if len(parts) == total:
                        raw = b"".join(parts[i] for i in range(1, total + 1))
                        return json.loads(raw.decode("utf-8", "replace"))
        finally:
            s.close()
        raise TransportError(f"no answer from fpvd at {self.addr[0]}:{self.addr[1]}")

    def shell(self, cmdline, timeout=TIMEOUT):
        raise TransportError("fpvd runs fpv commands only (no shell)")

    def push(self, local, remote):
        raise TransportError("installing files needs ADB")


def transport_for(req):
    kind = req.get("transport", "adb")
    if kind == "adb":
        return Adb(req.get("device") or None)
    if kind == "ssh":
        host = req.get("host") or "10.5.0.10"
        return Ssh(host)
    if kind == "fpvd":
        return Fpvd(req.get("host") or "172.32.0.93")
    raise TransportError(f"unknown transport {kind}")


def run_fpv(t, args):
    if isinstance(t, Fpvd):
        return t.fpv(args)
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
    if isinstance(t, Fpvd):
        words = line.split()
        if words[0] != "fpv" or len(words) < 2:
            return {"ok": True, "output": "over fpvd only `fpv <command>` works, e.g. fpv get"}
        r = t.fpv([w for w in words[1:] if w != "--json"])
        if "text" in r and r.get("ok"):          # fpv log
            return {"ok": True, "output": r["text"].rstrip("\n")}
        return {"ok": True, "output": json.dumps(r, indent=2)}
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


def list_presets():
    """configurator/presets/*.json: {"name", "description", "values": {key: value}}"""
    out, bad = [], []
    for f in sorted(os.listdir(PRESET_DIR)) if os.path.isdir(PRESET_DIR) else []:
        if not f.endswith(".json"):
            continue
        try:
            with open(os.path.join(PRESET_DIR, f), encoding="utf-8") as fh:
                d = json.load(fh)
            if not isinstance(d.get("values"), dict) or not d.get("name"):
                raise ValueError('needs "name" and a "values" object')
            out.append({"file": f, "name": str(d["name"]),
                        "description": str(d.get("description", "")),
                        "values": {str(k): str(v) for k, v in d["values"].items()}})
        except (OSError, ValueError) as e:
            bad.append(f"{f}: {e}")
    r = {"ok": True, "presets": out}
    if bad:
        r["warnings"] = bad
    return r


def dvr_pull(t, name):
    """copy one recording from the board's DVR partition to this PC (ADB)"""
    if not isinstance(t, Adb):
        raise TransportError("downloading recordings needs ADB (USB)")
    if not DVR_NAME.match(name or ""):
        raise TransportError(f"not a recording name: {name!r}")
    os.makedirs(dvr_local_dir, exist_ok=True)
    dest = os.path.join(dvr_local_dir, name)
    t0 = time.time()
    rc, out, err = t._run(t._base() + ["pull", f"{DVR_BOARD_DIR}/{name}", dest], 1800)
    if rc != 0:
        raise TransportError((err or out).strip() or "adb pull failed")
    size = os.path.getsize(dest)
    secs = max(time.time() - t0, 0.001)
    return {"ok": True, "path": dest, "size": size, "seconds": round(secs, 1),
            "mbps": round(size / secs / 1e6, 1)}


IQ_STOCK_DIR = "/oem/usr/share/iqfiles"
IQ_CUSTOM_DIR = "/userdata/iq"
IQ_NAME = re.compile(r"^[A-Za-z0-9_.-]+\.json$")
IQ_KEYS = ("sensor_calib", "module_calib", "main_scene")   # what rkaiq needs at top level
IQ_MAX = 8 * 1024 * 1024


def iq_pull(t, name, where):
    """IQ JSON from the board, returned to the browser as base64 (ADB)"""
    if not isinstance(t, Adb):
        raise TransportError("downloading IQ files needs ADB (USB)")
    if not IQ_NAME.match(name or "") or where not in ("stock", "custom"):
        raise TransportError(f"bad IQ file: {name!r}")
    src = f"{IQ_STOCK_DIR if where == 'stock' else IQ_CUSTOM_DIR}/{name}"
    fd, tmp = tempfile.mkstemp(prefix="fpvconf-iq-")
    os.close(fd)
    try:
        rc, out, err = t._run(t._base() + ["pull", src, tmp], 120)
        if rc != 0:
            raise TransportError((err or out).strip() or "adb pull failed")
        with open(tmp, "rb") as f:
            data = f.read()
    finally:
        os.unlink(tmp)
    return {"ok": True, "name": name, "size": len(data), "data": base64.b64encode(data).decode()}


def iq_push(t, name, data_b64):
    """Check an edited IQ JSON, push it next to the active custom file and let
    `fpv iq install` verify the md5 and swap it in (old version -> history).
    Nothing on the board changes unless every check passes."""
    if not isinstance(t, Adb):
        raise TransportError("uploading IQ files needs ADB (USB)")
    if not IQ_NAME.match(name or ""):
        raise TransportError(f"not an IQ file name: {name!r}")
    try:
        data = base64.b64decode(data_b64 or "", validate=True)
    except ValueError:
        raise TransportError("upload garbled (base64)")
    if not 1000 <= len(data) <= IQ_MAX:
        raise TransportError(f"{name}: {len(data)} bytes is not a plausible IQ file")
    try:
        doc = json.loads(data.decode("utf-8-sig"))
    except (UnicodeDecodeError, ValueError) as e:
        raise TransportError(f"{name} is not valid JSON: {e}")
    missing = [k for k in IQ_KEYS if not isinstance(doc, dict) or k not in doc]
    if missing:
        raise TransportError(f"{name} is missing {', '.join(missing)}: not an rkaiq IQ file")
    st = run_fpv(t, ["iq", "list"])
    if not st.get("ok"):
        raise TransportError(st.get("error", "fpv iq list failed"))
    if name not in [f["name"] for f in st.get("stock", [])]:
        raise TransportError(f"{name} does not match a stock IQ file name; rkaiq picks the file by name "
                             f"(stock: {', '.join(f['name'] for f in st.get('stock', []))})")
    md5 = hashlib.md5(data).hexdigest()
    fd, tmp = tempfile.mkstemp(prefix="fpvconf-iq-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)                       # byte for byte: CRLF etc. kept
        t.shell(f"mkdir -p {IQ_CUSTOM_DIR}")
        t.push(tmp, f"{IQ_CUSTOM_DIR}/{name}.new")
    finally:
        os.unlink(tmp)
    r = run_fpv(t, ["iq", "install", name, md5])
    if not r.get("ok"):
        raise TransportError(r.get("error", "install failed"))
    return r


def open_folder():
    os.makedirs(dvr_local_dir, exist_ok=True)
    if sys.platform.startswith("win"):
        os.startfile(dvr_local_dir)  # noqa: only on Windows
    else:
        opener = "open" if sys.platform == "darwin" else "xdg-open"
        subprocess.Popen([opener, dvr_local_dir], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return {"ok": True, "path": dvr_local_dir}


def install(t):
    if isinstance(t, Fpvd):
        raise TransportError("installing files needs ADB")
    installed = []
    # fpvd is a compiled binary: only there if this checkout ran `make`
    fpvd_bin = os.path.join(REPO, "build", "fpvd")
    if os.path.isfile(fpvd_bin):
        t.push(fpvd_bin, "/usr/bin/fpvd")
        t.shell("chmod 755 /usr/bin/fpvd")
        installed.append("/usr/bin/fpvd")
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
    installed += [r for _, r, _ in BOARD_FILES]
    return {"ok": True, "installed": installed}


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
        if path in ("/api/devices", "/api/presets"):
            if self.headers.get("X-Token") != TOKEN:
                return self._json({"ok": False, "error": "bad token"}, 403)
            return self._json(Adb.devices() if path == "/api/devices" else list_presets())
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
            if n > 4 * IQ_MAX:          # base64 of the largest IQ file, with room
                return self._json({"ok": False, "error": "request too large"}, 413)
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
            if self.path == "/api/dvr/pull":
                return self._json(dvr_pull(t, str(req.get("name", ""))))
            if self.path == "/api/dvr/open":
                return self._json(open_folder())
            if self.path == "/api/iq/pull":
                return self._json(iq_pull(t, str(req.get("name", "")), str(req.get("where", ""))))
            if self.path == "/api/iq/push":
                return self._json(iq_push(t, str(req.get("name", "")), req.get("data", "")))
            return self._json({"ok": False, "error": "no such endpoint"}, 404)
        except TransportError as e:
            return self._json({"ok": False, "error": str(e), "transport_error": True})
        except Exception as e:  # keep the server alive, show it in the UI
            return self._json({"ok": False, "error": f"{type(e).__name__}: {e}"})


def main():
    global dvr_local_dir
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--dvr-dir", help=f"where downloaded recordings go (default {dvr_local_dir})")
    a = ap.parse_args()
    if a.dvr_dir:
        dvr_local_dir = os.path.abspath(a.dvr_dir)
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
