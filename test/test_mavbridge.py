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
COPY_PORTS = (24560, 24561)   # -c twice (rkfpv OSD, another listener)
LOCAL_PORT = 24570  # -j: programs on the board


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
    copies = []
    for cp in COPY_PORTS:
        c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        c.bind(("127.0.0.1", cp))
        c.settimeout(0.5)
        copies.append(c)

    proc = subprocess.Popen(
        [exe, "-d", slave_name, "-o", f"127.0.0.1:{DOWN_PORT}",
         "-i", str(UP_PORT), "-a", "5", "-m", "600", "-t", "30",
         "-c", f"127.0.0.1:{COPY_PORTS[0]}", "-c", f"127.0.0.1:{COPY_PORTS[1]}",
         "-j", str(LOCAL_PORT)],
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

        # Every -c gets the same datagrams as the radio.
        for cp, c in zip(COPY_PORTS, copies):
            cgot = []
            while True:
                try:
                    cgot.extend(split_frames(c.recvfrom(4096)[0]))
                except socket.timeout:
                    break
            assert cgot == frames + [frames[0]], f"copy {cp}: {len(cgot)} frames"
        print(f"ok: {len(COPY_PORTS)} downlink copies (-c twice)")

        # -j: only on loopback
        port_hex = f"{LOCAL_PORT:04X}"
        with open("/proc/net/udp") as f:
            local = [l.split()[1] for l in f.readlines()[1:] if l.split()[1].endswith(":" + port_hex)]
        assert local == ["0100007F:" + port_hex], f"-j bound to {local}"
        print("ok: local port listens on 127.0.0.1 only")

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

        # -j: a local program's datagram reaches the UART unchanged ...
        loc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        loc.bind(("127.0.0.1", 0))
        loc.settimeout(2)
        cmd = v2(76, bytes(range(40, 73)))
        loc.sendto(cmd, ("127.0.0.1", LOCAL_PORT))
        buf, deadline = b"", time.time() + 2
        while len(buf) < len(cmd) and time.time() < deadline:
            try:
                buf += os.read(master, 4096)
            except BlockingIOError:
                time.sleep(0.01)
        assert buf == cmd, f"local uplink mismatch: {buf!r}"
        print("ok: local port -> UART")

        # ... and it gets the FC's answer, as does a listener that only
        # subscribed with an empty datagram; the radio still gets it too
        sub = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sub.bind(("127.0.0.1", 0))
        sub.settimeout(2)
        sub.sendto(b"", ("127.0.0.1", LOCAL_PORT))
        time.sleep(0.1)
        ack = v2(77, bytes(3))
        os.write(master, ack)
        assert loc.recvfrom(4096)[0] == ack, "sender got no downlink"
        assert sub.recvfrom(4096)[0] == ack, "subscriber got no downlink"
        assert rx.recvfrom(4096)[0] == ack, "radio lost the downlink"
        print("ok: local senders get the downlink back")
        print("all mavbridge tests passed")
    finally:
        proc.terminate()
        _, err = proc.communicate(timeout=2)
        if proc.returncode not in (0, -15):
            print(err.decode(), file=sys.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
