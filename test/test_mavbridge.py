#!/usr/bin/env python3
"""Host test for mavbridge: a pty stands in for the UART.

usage: test_mavbridge.py <path-to-host-mavbridge>
"""
import os
import pty
import random
import socket
import subprocess
import sys
import time
import tty

DOWN_PORT = 24551   # mavbridge -> us (wfb_tx side)
UP_PORT = 24550     # us -> mavbridge (wfb_rx side)


def v1(msgid, payload):
    return bytes([0xFE, len(payload), 1, 1, 1, msgid]) + payload + b"\x00\x00"


def v2(msgid, payload, signed=False):
    flags = 1 if signed else 0
    hdr = bytes([0xFD, len(payload), flags, 0, 7, 1, 1,
                 msgid & 0xFF, (msgid >> 8) & 0xFF, msgid >> 16])
    return hdr + payload + b"\x00\x00" + (bytes(13) if signed else b"")


def split_frames(data):
    """Splits a datagram back into frames; fails on any partial frame."""
    out, i = [], 0
    while i < len(data):
        if data[i] == 0xFE:
            n = 8 + data[i + 1]
        elif data[i] == 0xFD:
            n = 12 + data[i + 1] + (13 if data[i + 2] & 1 else 0)
        else:
            raise AssertionError(f"datagram does not start on a frame at {i}")
        assert i + n <= len(data), "partial frame in datagram"
        out.append(data[i:i + n])
        i += n
    return out


def main():
    exe = sys.argv[1]
    master, slave = pty.openpty()
    tty.setraw(master)
    slave_name = os.ttyname(slave)

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", DOWN_PORT))
    rx.settimeout(2)

    proc = subprocess.Popen(
        [exe, "-d", slave_name, "-o", f"127.0.0.1:{DOWN_PORT}",
         "-i", str(UP_PORT), "-a", "5", "-m", "600", "-t", "30"],
        stderr=subprocess.PIPE)
    time.sleep(0.3)

    try:
        random.seed(3)
        frames = []
        for k in range(60):
            plen = random.randint(0, 255)
            pl = bytes(random.randint(1, 255) for _ in range(plen))
            kind = k % 3
            frames.append(v1(k, pl) if kind == 0 else v2(k, pl, signed=(kind == 2)))

        # Junk (incl. fake STX bytes) before and between frames, then send
        # the stream in random-sized chunks to exercise partial reads.
        junk = b"\x11\x22\xfd\xff\x33"           # 0xFD with bad flags 0xFF
        stream = junk + b"".join(f + (junk if i == 10 else b"")
                                 for i, f in enumerate(frames))
        pos = 0
        while pos < len(stream):
            n = random.randint(1, 300)
            os.write(master, stream[pos:pos + n])
            pos += n
            time.sleep(0.002)

        got, datagrams = [], 0
        deadline = time.time() + 3
        while len(got) < len(frames) and time.time() < deadline:
            try:
                d, _ = rx.recvfrom(4096)
            except socket.timeout:
                break
            assert len(d) <= 600, f"datagram {len(d)} B over -m 600"
            fs = split_frames(d)
            assert len(fs) <= 5, f"{len(fs)} frames in one datagram, -a 5"
            got.extend(fs)
            datagrams += 1

        assert got == frames, f"frames differ: got {len(got)} want {len(frames)}"
        print(f"ok: downlink {len(frames)} frames in {datagrams} datagrams, "
              "boundaries intact, junk skipped")

        # A lone frame must still go out after the flush timeout.
        t0 = time.time()
        os.write(master, frames[0])
        d, _ = rx.recvfrom(4096)
        dt = (time.time() - t0) * 1000
        assert d == frames[0]
        print(f"ok: single frame flushed after {dt:.0f} ms")

        # Uplink: UDP -> UART unchanged.
        up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        msg = v2(76, bytes(range(33)))
        up.sendto(msg, ("127.0.0.1", UP_PORT))
        buf, deadline = b"", time.time() + 2
        while len(buf) < len(msg) and time.time() < deadline:
            try:
                buf += os.read(master, 4096)
            except BlockingIOError:
                time.sleep(0.01)
        assert buf == msg, f"uplink mismatch: {buf!r}"
        print("ok: uplink UDP -> UART")
        print("all mavbridge tests passed")
    finally:
        proc.terminate()
        _, err = proc.communicate(timeout=2)
        if proc.returncode not in (0, -15):
            print(err.decode(), file=sys.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
