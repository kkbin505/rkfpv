#!/usr/bin/env python3
"""Host test for mavbridge -q: asking the FC for the OSD messages.

A pty plays the flight controller. Checks that mavbridge
  - asks nothing before a real FC heartbeat (not for a GCS/radio heartbeat),
  - then sends a valid MAVLink v2 COMMAND_LONG SET_MESSAGE_INTERVAL for
    SYS_STATUS, BATTERY_STATUS and RC_CHANNELS, interval 1e6/hz us,
  - stops asking for a message once it arrives, keeps asking for the rest,
  - gives up after 5 tries, and asks again after the FC restarts,
  - still forwards everything (the FC stream reaches the downlink).

usage: test_mavbridge_req.py <path-to-host-mavbridge>
"""
import os
import pty
import select
import socket
import struct
import subprocess
import sys
import time
import tty

DOWN_PORT = 25551
UP_PORT = 25550
HZ = 4
CRC_EXTRA = {0: 50, 1: 124, 65: 118, 76: 152, 147: 154}
NAMES = {1: "SYS_STATUS", 147: "BATTERY_STATUS", 65: "RC_CHANNELS"}


def x25(data):
    crc = 0xFFFF
    for c in data:
        t = c ^ (crc & 0xFF)
        t = (t ^ (t << 4)) & 0xFF
        crc = ((crc >> 8) ^ (t << 8) ^ (t << 3) ^ (t >> 4)) & 0xFFFF
    return crc


SEQ = [0]


def v2(msgid, payload, sysid=1, compid=1):
    SEQ[0] = (SEQ[0] + 1) & 0xFF
    h = bytes([len(payload), 0, 0, SEQ[0], sysid, compid,
               msgid & 0xFF, (msgid >> 8) & 0xFF, msgid >> 16]) + payload
    return b"\xfd" + h + struct.pack("<H", x25(h + bytes([CRC_EXTRA.get(msgid, 0)])))


def heartbeat(autopilot=3, sysid=1, compid=1):
    return v2(0, struct.pack("<IBBBBB", 0, 2, autopilot, 0, 3, 3), sysid, compid)


def commands(buf):
    """COMMAND_LONG frames in bytes the bridge wrote to the UART:
    [(target_sys, target_comp, command, param1, param2, sender_sys, sender_comp)]"""
    out, i = [], 0
    while i < len(buf):
        if buf[i] != 0xFD:
            raise AssertionError(f"unexpected byte {buf[i]:#x} on the UART at {i}")
        n = buf[i + 1]
        f = buf[i:i + 12 + n]
        assert len(f) == 12 + n, "partial frame"
        msgid = f[7] | f[8] << 8 | f[9] << 16
        assert msgid == 76, f"not COMMAND_LONG: {msgid}"
        crc = x25(f[1:10 + n] + bytes([152]))
        assert struct.unpack("<H", f[10 + n:12 + n])[0] == crc, "bad CRC"
        p = f[10:10 + n] + bytes(33)
        p1, p2 = struct.unpack_from("<ff", p, 0)
        cmd, ts, tc = struct.unpack_from("<HBB", p, 28)
        out.append((ts, tc, cmd, p1, p2, f[5], f[6]))
        i += 12 + n
    return out


def main():
    exe = sys.argv[1]
    master, slave = pty.openpty()
    tty.setraw(master)
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", DOWN_PORT))
    rx.setblocking(False)
    proc = subprocess.Popen([exe, "-d", os.ttyname(slave), "-o", f"127.0.0.1:{DOWN_PORT}",
                             "-i", str(UP_PORT), "-a", "1", "-q", str(HZ)],
                            stderr=subprocess.PIPE)
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok &= bool(cond)
        print(f"{'ok  ' if cond else 'FAIL'} {name}" + (f"  ({detail})" if detail else ""))

    def run(secs, send):
        """for secs: every 200 ms write send() to the UART; collect what comes back"""
        got, end, nxt = b"", time.time() + secs, 0
        while time.time() < end:
            if time.time() >= nxt:
                os.write(master, send())
                nxt = time.time() + 0.2
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                got += os.read(master, 4096)
            try:
                while True:
                    rx.recv(4096)
            except BlockingIOError:
                pass
        return got

    try:
        time.sleep(0.3)
        # a GCS (autopilot 8) and a gimbal (compid 154) are not the FC
        got = run(2.5, lambda: heartbeat(autopilot=8, sysid=255, compid=190) + heartbeat(3, 1, 154))
        check("no request without an FC heartbeat", got == b"", f"{len(got)} bytes")

        # FC heartbeat only: after ~1 s it asks for all three
        hb = heartbeat()
        got = run(2.0, lambda: hb)
        cmds = commands(got)
        ids = sorted(int(c[3]) for c in cmds)
        check("asks for the three OSD messages", ids == [1, 65, 147], ids)
        check("SET_MESSAGE_INTERVAL to sys 1 comp 1, from comp 191",
              all(c[0] == 1 and c[1] == 1 and c[2] == 511 and c[6] == 191 for c in cmds))
        check(f"interval 1e6/{HZ} us", all(abs(c[4] - 1e6 / HZ) < 1 for c in cmds), [c[4] for c in cmds])

        # SYS_STATUS starts arriving: only the other two are asked again (retry 5 s)
        sys_status = v2(1, bytes(14) + struct.pack("<Hh", 16400, 1230) + bytes(12) + b"\x4e")
        got = run(5.5, lambda: hb + sys_status)
        ids = sorted(int(c[3]) for c in commands(got))
        check("stops asking for a message that arrives", ids == [65, 147], ids)

        # the rest never arrives: 5 tries in all, then quiet
        got = run(16, lambda: hb + sys_status)
        ids = [int(c[3]) for c in commands(got)]
        check("gives up after 5 tries", ids.count(65) == 3 and ids.count(147) == 3, ids)
        got = run(6, lambda: hb + sys_status)
        check("quiet after giving up", got == b"", f"{len(got)} bytes")

        # FC restarts (heartbeat gone > 3 s): asks again for all three
        run(3.5, lambda: b"")
        got = run(2.0, lambda: hb)
        ids = sorted(int(c[3]) for c in commands(got))
        check("asks again after the FC restarts", ids == [1, 65, 147], ids)
    finally:
        proc.terminate()
        err = proc.communicate(timeout=3)[1].decode()
    check("logs the FC and the requests", "flight controller 1" in err and "asking the FC for SYS_STATUS" in err
          and "SYS_STATUS arriving" in err and "giving up" in err, err.strip().splitlines()[-1:])
    print("all passed" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
