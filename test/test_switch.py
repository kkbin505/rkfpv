#!/usr/bin/env python3
"""Tests the hotspot button (BOOT on the ADC) in fpvd with a fake ADC file,
a fake mode file and a fake mode script that logs its argument.

    python3 -I test/test_switch.py build-host/fpvd
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

FPVD = sys.argv[1]
HOLD = 0.6                              # -H, seconds (2 s on the board)
UP, DOWN = 1023, 10
passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("ok  ", name)
    else:
        failed += 1
        print("FAIL", name, detail)


class Rig:
    def __init__(self, mode="fpv", adc=UP, script_sleep=0, adc_file=True):
        self.dir = tempfile.mkdtemp()
        self.adc = f"{self.dir}/in_voltage0_raw"
        self.modef = f"{self.dir}/mode.status"
        self.log = f"{self.dir}/actions"
        if adc_file:
            self.set_adc(adc)
        self.set_mode(mode)
        script = f"{self.dir}/S99fpv"
        # the fake script flips the mode like S99fpv does
        with open(script, "w") as f:
            f.write(f"#!/bin/sh\necho switching > {self.modef}\nsleep {script_sleep}\n"
                    f"echo \"$1\" >> {self.log}\n"
                    f"[ \"$1\" = hotspot-on ] && echo hotspot > {self.modef} || echo fpv > {self.modef}\n")
        os.chmod(script, 0o755)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        self.p = subprocess.Popen([FPVD, "-p", str(self.port), "-b", "127.0.0.1", "-a", self.adc,
                                   "-H", str(int(HOLD * 1000)), "-m", self.modef, "-x", script],
                                  stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

    def set_adc(self, v):
        # in place, same inode: fpvd keeps the file open and re-reads it with
        # pread, like the sysfs file (an empty read in between is skipped)
        with open(self.adc, "w") as f:
            f.write(f"{v}\n")

    def set_mode(self, m):
        with open(self.modef, "w") as f:
            f.write(m + "\n")

    def press(self, secs):
        self.set_adc(DOWN)
        time.sleep(secs)
        self.set_adc(UP)

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
        self.p.terminate()
        return self.p.communicate(timeout=5)[1]


r = Rig()
r.press(0.3)
time.sleep(0.4)
check("short press does nothing", r.actions() == [], r.actions())
r.press(HOLD + 0.5)
time.sleep(0.3)
check("held -> hotspot-on", r.actions() == ["hotspot-on"], r.actions())
r.press(HOLD + 0.5)
time.sleep(0.3)
check("held again -> hotspot-off", r.actions() == ["hotspot-on", "hotspot-off"], r.actions())
# one action per press, however long it is held
r.press(3 * HOLD + 0.5)
time.sleep(0.3)
check("long hold acts once", r.actions() == ["hotspot-on", "hotspot-off", "hotspot-on"], r.actions())
# mid-scale noise below the threshold for a moment does not count
r.set_adc(600)
time.sleep(HOLD + 0.4)
r.set_adc(UP)
check("600 counts as released", len(r.actions()) == 3, r.actions())
check("command service still answers", r.ping())
r.close()

# boot not finished / change in progress: ignored
for m, label in (("", "mode unset"), ("switching", "mode switching")):
    r = Rig(mode=m)
    r.press(HOLD + 0.5)
    time.sleep(0.3)
    err = r.close()
    check(f"{label}: ignored", r.actions() == [] and "ignored" in err, (r.actions(), err))

# held when fpvd starts: needs a release first
r = Rig(adc=DOWN)
time.sleep(HOLD + 0.4)
check("held at start: no action", r.actions() == [], r.actions())
r.set_adc(UP)
time.sleep(0.4)
r.press(HOLD + 0.5)
time.sleep(0.3)
check("after a release it works", r.actions() == ["hotspot-on"], r.actions())
r.close()

# a slow mode change: presses during it do nothing, the service keeps answering
r = Rig(script_sleep=2)
r.press(HOLD + 0.5)
check("service answers while the script runs", r.ping())
r.press(HOLD + 0.5)
time.sleep(1.5)
check("press during a change ignored", r.actions() == ["hotspot-on"], r.actions())
r.close()

# no ADC: fpvd still serves commands
r = Rig(adc_file=False)
check("no button: service runs", r.ping())
err = r.close()
check("no button: logged", "no hotspot button" in err, err)

print(f"{passed}/{passed + failed} passed")
sys.exit(1 if failed else 0)
