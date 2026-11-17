#!/usr/bin/env python3
"""Tests the hotspot switch watcher in fpvd with fake input events (FIFO)
and a fake mode script that logs its argument.

    python3 -I test/test_switch.py build-host/fpvd
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

FPVD = sys.argv[1]
EV_KEY, KEY_WLAN = 1, 238
EVFMT = "llHHi"                         # struct input_event on this (64-bit) host
passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("ok  ", name)
    else:
        failed += 1
        print("FAIL", name, detail)


def ev(value, code=KEY_WLAN, etype=EV_KEY):
    return struct.pack(EVFMT, 0, 0, etype, code, value)


class Rig:
    def __init__(self, initial, script_sleep=0):
        self.dir = tempfile.mkdtemp()
        self.fifo = f"{self.dir}/events"
        self.log = f"{self.dir}/actions"
        os.mkfifo(self.fifo)
        script = f"{self.dir}/S99fpv"
        with open(script, "w") as f:
            f.write(f"#!/bin/sh\nsleep {script_sleep}\necho \"$1\" >> {self.log}\n")
        os.chmod(script, 0o755)
        self.port = 29000 + os.getpid() % 500 + initial * 7 + script_sleep
        self.p = subprocess.Popen([FPVD, "-p", str(self.port), "-b", "127.0.0.1", "-E", self.fifo,
                                   "-S", str(initial), "-x", script], stderr=subprocess.PIPE)
        self.w = open(self.fifo, "wb", buffering=0)     # fpvd holds the read end
        time.sleep(0.2)

    def send(self, *values):
        self.w.write(b"".join(ev(v) for v in values))

    def actions(self):
        try:
            return open(self.log).read().split()
        except FileNotFoundError:
            return []

    def ping(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(1)
        s.sendto(b"FPV1 p1 ping", ("127.0.0.1", self.port))
        try:
            return b"pong" in s.recvfrom(4096)[0]
        except socket.timeout:
            return False

    def close(self):
        self.w.close()
        self.p.terminate()
        self.p.wait(timeout=5)


# open at boot -> close -> hotspot-on after 1 s, not before
r = Rig(0)
r.send(1)
time.sleep(0.5)
check("no action before 1 s", r.actions() == [], r.actions())
time.sleep(1.0)
check("held closed 1 s -> hotspot-on", r.actions() == ["hotspot-on"], r.actions())
# bounce: short opens do nothing
r.send(0, 1, 0, 1)
time.sleep(0.3)
r.send(0)
time.sleep(0.3)
r.send(1)
time.sleep(1.3)
check("bounces ignored", r.actions() == ["hotspot-on"], r.actions())
# open and hold -> hotspot-off
r.send(0)
time.sleep(1.4)
check("held open 1 s -> hotspot-off", r.actions() == ["hotspot-on", "hotspot-off"], r.actions())
# other keys / event types ignored
r.send(*[])
r.w.write(ev(1, code=30) + ev(1, etype=0))
time.sleep(1.3)
check("other keys ignored", r.actions() == ["hotspot-on", "hotspot-off"], r.actions())
check("command service still answers", r.ping())
r.close()

# closed at boot: the boot mode already follows it, so no action at start
r = Rig(1)
time.sleep(1.4)
check("boot position not acted on", r.actions() == [], r.actions())
r.send(0)
time.sleep(1.4)
check("then open -> hotspot-off", r.actions() == ["hotspot-off"], r.actions())
r.close()

# a slow mode change: the next one waits for it, the service keeps answering
r = Rig(0, script_sleep=2)
r.send(1)
time.sleep(1.3)
check("service answers while the script runs", r.ping())
r.send(0)
time.sleep(1.5)
check("second change waits for the first", r.actions() == [], r.actions())
time.sleep(0.5)                         # t = 3.3 s: first script done (1 s hold + 2 s)
check("first finished, second running", r.actions() == ["hotspot-on"], r.actions())
time.sleep(2.2)                         # second started right after the first (held long enough)
check("both ran, in order", r.actions() == ["hotspot-on", "hotspot-off"], r.actions())
r.close()

# query mode
d = tempfile.mkdtemp()
f = f"{d}/ev"
open(f, "wb").close()
q = subprocess.run([FPVD, "-q", "-E", f, "-S", "1"], capture_output=True, text=True)
check("-q closed", q.returncode == 0 and q.stdout.strip() == "closed", q)
q = subprocess.run([FPVD, "-q", "-E", f, "-S", "0"], capture_output=True, text=True)
check("-q open", q.returncode == 1 and q.stdout.strip() == "open", q)
q = subprocess.run([FPVD, "-q", "-k", "no-such-device"], capture_output=True, text=True)
check("-q without a switch", q.returncode == 2 and q.stdout.strip() == "none", q)

print(f"{passed}/{passed + failed} passed")
sys.exit(1 if failed else 0)
