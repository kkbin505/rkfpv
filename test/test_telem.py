#!/usr/bin/env python3
"""Tests src/telem.c with MAVLink frames built here (own encoder, CRC_EXTRA
from common.xml), so a shared bug in encoder and decoder cannot hide.

    python3 -I test/test_telem.py build-host/test_telem
"""
import os
import struct
import subprocess
import sys
import tempfile

PROG = sys.argv[1]
CRC_EXTRA = {0: 50, 1: 124, 35: 244, 65: 118, 147: 154, 30: 39}
passed = failed = 0


def x25(data, crc=0xFFFF):
    for b in data:
        tmp = b ^ (crc & 0xFF)
        tmp = (tmp ^ (tmp << 4)) & 0xFF
        crc = ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xFFFF
    return crc


def v1(msgid, payload, sysid=1, compid=1, seq=0):
    hdr = bytes([len(payload), seq, sysid, compid, msgid])
    crc = x25(hdr + payload + bytes([CRC_EXTRA[msgid]]))
    return b"\xfe" + hdr + payload + struct.pack("<H", crc)


def v2(msgid, payload, sysid=1, compid=1, seq=0, trim=True, signed=False):
    if trim:
        payload = payload.rstrip(b"\x00") or payload[:1]
    hdr = bytes([len(payload), 1 if signed else 0, 0, seq, sysid, compid]) + struct.pack("<I", msgid)[:3]
    crc = x25(hdr + payload + bytes([CRC_EXTRA[msgid]]))
    return b"\xfd" + hdr + payload + struct.pack("<H", crc) + (bytes(13) if signed else b"")


def heartbeat(custom_mode, mtype=2, autopilot=3, armed=False):
    return struct.pack("<IBBBBB", custom_mode, mtype, autopilot, 0x80 if armed else 0x01, 4, 3)


def sys_status(mv, ca, pct):
    return struct.pack("<IIIHHhHHHHHHb", 0, 0, 0, 100, mv, ca, 0, 0, 0, 0, 0, 0, pct)


def battery_status(mah):
    return struct.pack("<iih10HhBBBb", mah, -1, 0x7FFF, *([0xFFFF] * 10), -1, 0, 0, 0, -1)


def rc_channels(rssi):
    return struct.pack("<I18HBB", 1000, *([1500] * 18), 16, rssi)


def rc_raw(rssi):
    return struct.pack("<I8HBB", 1000, *([1500] * 8), 0, rssi)


def attitude():
    return bytes(28)                    # an id the decoder ignores (still well-formed)


def run(records):
    """records: [(now_ms, datagram_bytes)] -> dict"""
    fd, path = tempfile.mkstemp()
    with os.fdopen(fd, "wb") as f:
        for now, d in records:
            f.write(struct.pack("<IH", now, len(d)) + d)
    out = subprocess.run([PROG, path], capture_output=True, text=True).stdout
    os.unlink(path)
    d = {}
    for kv in out.split():
        k, _, v = kv.partition("=")
        d[k] = v
    return d


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("ok  ", name)
    else:
        failed += 1
        print("FAIL", name, detail)


# struct sizes match the MAVLink wire lengths
assert len(heartbeat(0)) == 9 and len(sys_status(0, 0, 0)) == 31
assert len(battery_status(0)) == 36 and len(rc_channels(0)) == 42 and len(rc_raw(0)) == 22

# ArduCopter, v1, several frames per datagram (as mavbridge packs them)
r = run([(1000, v1(0, heartbeat(5)) + v1(1, sys_status(16420, 1234, 78)) + v1(30, attitude()[:28]) +
          v1(147, battery_status(1234)) + v1(65, rc_channels(254)))])
check("copter mode LOITER", r.get("mode") == "LOITER", r)
check("battery volts", r.get("volts") == "16.42", r)
check("battery amps", r.get("amps") == "12.34", r)
check("battery %", r.get("remaining") == "78", r)
check("mAh", r.get("mah") == "1234", r)
check("rc rssi (ELRS LQ)", r.get("rc_rssi") == "254", r)
check("4 known frames CRC-ok, ATTITUDE skipped", r.get("bad") == "0" and r.get("ok") == "4", r)

# v2 with trailing-zero truncation, plane modes, signed frame
r = run([(0, v2(0, heartbeat(5, mtype=1)) + v2(1, sys_status(12000, 0, 0), signed=True))])
check("v2 plane FBWA", r.get("mode") == "FBWA", r)
check("v2 truncated payload: amps 0, remaining 0", r.get("amps") == "0.00" and r.get("remaining") == "0", r)
check("v2 signed frame accepted", r.get("volts") == "12.00", r)

# PX4: main 4 (AUTO) sub 4 (MISSION)
r = run([(0, v2(0, heartbeat((4 << 16) | (4 << 24), autopilot=12)))])
check("PX4 AUTO.MISSION", r.get("mode") == "MISSION", r)
r = run([(0, v1(0, heartbeat(99)))])
check("unknown mode number", r.get("mode") == "MODE", r)   # printed as "MODE 99": split on space

# bad CRC ignored, valid frame after it still parsed
bad = bytearray(v1(1, sys_status(9999, 100, 10)))
bad[-1] ^= 0xFF
r = run([(0, bytes(bad) + v1(1, sys_status(15000, 100, 50)))])
check("bad CRC rejected", r.get("bad") == "1", r)
check("frame after a bad one used", r.get("volts") == "15.00", r)

# garbage between frames
r = run([(0, b"\x00\x11garbage" + v1(1, sys_status(14000, 100, 40)) + b"\xfe")])
check("garbage skipped", r.get("volts") == "14.00", r)

# heartbeat from a non-autopilot (GCS / radio): ignored
r = run([(0, v1(0, heartbeat(5))), (100, v1(0, heartbeat(0, autopilot=8), compid=190))])
check("GCS heartbeat ignored", r.get("mode") == "LOITER" and r.get("autopilot") == "3", r)
r = run([(0, v1(0, heartbeat(5))), (100, v1(0, heartbeat(0), compid=154))])
check("other component ignored", r.get("mode") == "LOITER", r)

# arm -> flight time runs; disarm -> last flight kept
r = run([(1000, v1(0, heartbeat(0, armed=False))), (2000, v1(0, heartbeat(0, armed=True))),
         (65000, v1(1, sys_status(16000, 100, 50)))])
check("armed", r.get("armed") == "1", r)
check("flight time while armed", r.get("flight_ms") == "63000", r)
r = run([(2000, v1(0, heartbeat(0, armed=True))), (12000, v1(0, heartbeat(0, armed=False))),
         (50000, v1(1, sys_status(16000, 100, 50)))])
check("disarmed keeps last flight", r.get("armed") == "0" and r.get("flight_ms") == "10000", r)

# RC_CHANNELS_RAW when RC_CHANNELS is absent; 255 = unknown
r = run([(0, v1(35, rc_raw(200)))])
check("rc_raw rssi", r.get("rc_rssi") == "200", r)
r = run([(0, v1(65, rc_channels(255)))])
check("rssi 255 = unknown", r.get("rc_rssi") == "-1", r)

print(f"{passed}/{passed + failed} passed")
sys.exit(1 if failed else 0)
