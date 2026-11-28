#!/usr/bin/env python3
"""Host test for bblog (Betaflight serial blackbox logger) and bbsend.

ptys stand in for the UART. Part 1 feeds bblog directly (split markers,
pairing with the recording, append, timeout, no DVR). Part 2 is the same
loopback as `fpv blackbox selftest` on the board: bbsend -> bblog, the
saved logs must equal what was sent, byte for byte.

usage: test_bblog.py <host bblog> <host bbsend>
"""
import os
import pty
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tty

HDR = b"H Product:Blackbox flight data recorder by Nicholas Sherlock\n"
END = b"E\xffEnd of log\x00"
fails = 0


def check(name, cond, detail=""):
    global fails
    if cond:
        print(f"ok: {name}")
    else:
        fails += 1
        print(f"FAIL: {name} {detail}")


def make_log(n, seed):
    r = random.Random(seed)
    body = bytes(r.choice(b"IPabcdefghijklmnopqrstuvwxyz0123456789\x01\x02\xfe") for _ in range(n))
    return HDR + b"H Data version:2\n" + body + END


def wait_for(cond, timeout=5.0):
    t = time.time() + timeout
    while time.time() < t:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def status(path):
    s = read(path)
    if not s:
        return {}
    return dict(kv.split("=", 1) for kv in s.decode().split() if "=" in kv)


def send(master, data, rng):
    """Writes in small random pieces so markers get split across reads."""
    i = 0
    while i < len(data):
        k = rng.randint(1, 50)
        os.write(master, data[i:i + k])
        i += k
        if rng.random() < 0.2:
            time.sleep(0.002)


def part1(bblog, d):
    master, slave = pty.openpty()
    tty.setraw(master)
    tty.setraw(slave)
    dvr = os.path.join(d, "dvr.status")
    st = os.path.join(d, "bblog.status")
    out = os.path.join(d, "card")
    os.mkdir(out)
    with open(dvr, "w") as f:
        f.write("state=recording file=rec_0003_01.mp4 bytes=1 drops=0 free_mb=100\n")
    p = subprocess.Popen([bblog, "-d", os.ttyname(slave), "-b", "1500000", "-o", out,
                          "-s", dvr, "-S", st, "-t", "400"], stderr=subprocess.PIPE)
    rng = random.Random(7)
    try:
        time.sleep(0.3)
        l1, l2 = make_log(20000, 1), make_log(15000, 2)
        send(master, b"\x00junk before the log\xff" + l1, rng)
        f1 = os.path.join(out, "rec_0003_01.bbl")
        check("log saved under the recording's name", wait_for(lambda: read(f1) == l1),
              f"(got {len(read(f1) or b'')} bytes, want {len(l1)})")
        check("idle after the end marker", wait_for(lambda: status(st).get("state") == "idle"))

        send(master, b"noise" + l2, rng)
        check("second arm in the same segment appended", wait_for(lambda: read(f1) == l1 + l2))

        # no end marker: closed by the timeout; the next log goes to the new segment
        l3 = make_log(5000, 3)[:-len(END)]
        send(master, l3, rng)
        check("logging while data flows", wait_for(lambda: status(st).get("state") == "logging"))
        check("closed after silence", wait_for(lambda: status(st).get("state") == "idle", 3))
        check("log without end marker kept whole", read(f1) == l1 + l2 + l3)

        with open(dvr, "w") as f:
            f.write("state=recording file=rec_0004_00.ts bytes=1 drops=0 free_mb=100\n")
        # two logs back to back, the first without its end (FC reboot)
        l4, l5 = make_log(3000, 4)[:-len(END)], make_log(3000, 5)
        send(master, l4 + l5, rng)
        f2 = os.path.join(out, "rec_0004_00.bbl")
        check("new segment, new file; header without end splits logs",
              wait_for(lambda: read(f2) == l4 + l5))
        check("log count", wait_for(lambda: status(st).get("logs") == "5"), str(status(st)))

        with open(dvr, "w") as f:
            f.write("state=stopped file=- bytes=0 drops=0 free_mb=100\n")
        before = sorted(os.listdir(out))
        send(master, make_log(3000, 6)[:2000], rng)
        check("DVR stopped: log not saved",
              wait_for(lambda: status(st).get("why") == "dvr-not-recording"), str(status(st)))
        check("no new file", sorted(os.listdir(out)) == before)
        wait_for(lambda: status(st).get("state") == "idle", 3)
    finally:
        p.terminate()
        _, err = p.communicate(timeout=3)
        os.close(master)
        os.close(slave)
    check("bblog exits cleanly on TERM", p.returncode == 0, err.decode()[-500:])


def relay(src, dst, stop):
    while not stop.is_set():
        try:
            b = os.read(src, 65536)
        except OSError:
            time.sleep(0.01)
            continue
        if b:
            os.write(dst, b)


def part2(bblog, bbsend, d):
    """bbsend -> pty A -> relay -> pty B -> bblog, like the board loopback."""
    ma, sa = pty.openpty()
    mb, sb = pty.openpty()
    for fd in (ma, sa, mb, sb):
        tty.setraw(fd)
    out, ref = os.path.join(d, "loop"), os.path.join(d, "ref")
    os.mkdir(out)
    os.mkdir(ref)
    stop = threading.Event()
    t = threading.Thread(target=relay, args=(ma, mb, stop), daemon=True)
    t.start()
    log = subprocess.Popen([bblog, "-d", os.ttyname(sb), "-o", out, "-s", "-", "-S", "",
                            "-t", "300", "-n", "2"], stderr=subprocess.PIPE)
    time.sleep(0.3)
    snd = subprocess.run([bbsend, "-d", os.ttyname(sa), "-r", "0", "-n", "2", "-g", "600",
                          "-s", "200000", "-w", ref], stderr=subprocess.PIPE, timeout=30)
    check("bbsend ran", snd.returncode == 0, snd.stderr.decode())
    try:
        log.wait(timeout=10)
    except subprocess.TimeoutExpired:
        log.terminate()
    stop.set()
    err = log.communicate(timeout=3)[1].decode()
    for i in (1, 2):
        a, b = read(os.path.join(out, f"log_{i:03d}.bbl")), read(os.path.join(ref, f"ref_{i:03d}.bbl"))
        check(f"loopback log {i} byte for byte", a is not None and a == b,
              f"(saved {len(a or b'')}, sent {len(b or b'')})\n{err[-400:]}")
    check("synthetic log has a Betaflight header and end",
          (read(os.path.join(ref, "ref_001.bbl")) or b"").startswith(b"H Product:Blackbox flight data recorder")
          and (read(os.path.join(ref, "ref_001.bbl")) or b"").endswith(END))
    for fd in (ma, sa, mb, sb):
        os.close(fd)


def main():
    bblog, bbsend = sys.argv[1], sys.argv[2]
    d = tempfile.mkdtemp(prefix="bblog")
    try:
        part1(bblog, d)
        part2(bblog, bbsend, d)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    if fails:
        print(f"{fails} bblog test(s) FAILED")
        sys.exit(1)
    print("all bblog tests passed")


if __name__ == "__main__":
    main()
