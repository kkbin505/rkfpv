#!/usr/bin/env python3
"""Minimal Betaflight blackbox decoder for the bblog tests.

    bbdecode.py FILE.bbl [--csv-dir DIR]     decode every log in the file
    bbdecode.py FILE.bbl --check REF.csv...  compare log N with REF N, value by value

Reads the field definitions from the log's own header lines ("H Field I
name/signed/predictor/encoding", "H Field P predictor/encoding", "H I
interval", "H P interval") like Betaflight's blackbox_decode, not from the
program that wrote the log. Supports the predictors and encodings bbsim uses
(predictors 0 none, 1 previous, 2 straight line, 6 increment; encodings 0
signed VB, 1 unsigned VB, 9 null) and the log-end event; anything else is
reported as unsupported instead of decoded wrongly.

A frame that does not parse (bad frame type byte, a VB running past the
end) is counted as corrupt and decoding resyncs at the next frame byte, as
blackbox_decode does.
"""
import csv, os, sys

HDR = b"H Product:Blackbox flight data recorder"
END = b"End of log\x00"


def split_logs(data):
    logs, i = [], data.find(HDR)
    while i >= 0:
        j = data.find(HDR, i + 1)
        logs.append(data[i:j if j >= 0 else len(data)])
        i = j
    return logs


class Reader:
    def __init__(self, b, pos):
        self.b, self.p = b, pos

    def byte(self):
        if self.p >= len(self.b):
            raise EOFError
        c = self.b[self.p]; self.p += 1
        return c

    def uvb(self):
        v = shift = 0
        for _ in range(5):
            c = self.byte()
            v |= (c & 0x7F) << shift
            if c < 128:
                return v
            shift += 7
        raise ValueError("VB too long")

    def svb(self):
        u = self.uvb()
        return (u >> 1) ^ -(u & 1)


def decode_log(log):
    head, pos = {}, 0
    while log.startswith(b"H ", pos):
        nl = log.index(b"\n", pos)
        k, _, v = log[pos + 2:nl].decode("latin-1").partition(":")
        head[k] = v
        pos = nl + 1
    names = head["Field I name"].split(",")
    ints = lambda k: [int(x) for x in head[k].split(",")]
    ipred, ienc = ints("Field I predictor"), ints("Field I encoding")
    ppred, penc = ints("Field P predictor"), ints("Field P encoding")
    for p in set(ipred + ppred):
        if p not in (0, 1, 2, 6):
            raise NotImplementedError(f"predictor {p}")
    for e in set(ienc + penc):
        if e not in (0, 1, 9):
            raise NotImplementedError(f"encoding {e}")
    pnum, pden = (int(x) for x in head.get("P interval", "1/1").split("/"))
    if (pnum, pden) != (1, 1):
        raise NotImplementedError("P interval other than 1/1")

    r = Reader(log, pos)
    rows, prev, prev2 = [], None, None
    stats = {"I": 0, "P": 0, "corrupt": 0, "end": False, "bytes_after_end": 0}
    while r.p < len(log):
        start = r.p
        t = r.byte()
        try:
            if t == ord("E"):
                ev = r.byte()
                if ev == 0xFF and log[r.p:r.p + len(END)] == END:
                    r.p += len(END)
                    stats["end"] = True
                    stats["bytes_after_end"] = len(log) - r.p
                    break
                raise ValueError("unknown event")
            if t == ord("I"):
                cur = []
                for f in range(len(names)):
                    v = r.uvb() if ienc[f] == 1 else r.svb() if ienc[f] == 0 else 0
                    cur.append(v)           # I predictors here are all 0 (checked above for P)
                if any(ipred):
                    raise NotImplementedError("I predictor other than 0")
                prev2 = prev = cur
                stats["I"] += 1
            elif t == ord("P"):
                if prev is None:
                    raise ValueError("P frame before any I frame")
                cur = []
                for f in range(len(names)):
                    d = r.uvb() if penc[f] == 1 else r.svb() if penc[f] == 0 else 0
                    p = {0: 0, 1: prev[f], 2: 2 * prev[f] - prev2[f], 6: prev[f] + 1}[ppred[f]]
                    cur.append(p + d)
                prev2, prev = prev, cur
                stats["P"] += 1
            else:
                raise ValueError(f"frame type {t:#x}")
            rows.append(cur)
        except EOFError:
            stats["truncated_last_frame"] = True
            break
        except ValueError:
            stats["corrupt"] += 1
            r.p = start + 1
            while r.p < len(log) and log[r.p] not in b"IPE":
                r.p += 1
    return head, names, rows, stats


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return 2
    data = open(a[0], "rb").read()
    logs = split_logs(data)
    refs = a[a.index("--check") + 1:] if "--check" in a else []
    bad = 0
    for n, log in enumerate(logs, 1):
        head, names, rows, st = decode_log(log)
        span = (rows[-1][1] - rows[0][1]) / 1e6 if rows else 0
        print(f"log {n}: {head.get('Firmware revision', '?')}, {len(names)} fields, "
              f"{st['I']} I + {st['P']} P frames, {span:.2f} s, corrupt {st['corrupt']}, "
              f"{'log end' if st['end'] else 'NO log end (cut)'}"
              + (", last frame truncated" if st.get("truncated_last_frame") else ""))
        if "--csv-dir" in a:
            d = a[a.index("--csv-dir") + 1]
            with open(os.path.join(d, f"decoded_{n:03d}.csv"), "w", newline="") as f:
                w = csv.writer(f); w.writerow(names); w.writerows(rows)
        if n <= len(refs):
            with open(refs[n - 1]) as f:
                rd = csv.reader(f); rn = next(rd); ref = [[int(x) for x in row] for row in rd]
            if rn != names:
                print(f"  FAIL field names differ: {rn} vs {names}"); bad += 1; continue
            if len(rows) > len(ref):
                print(f"  FAIL more frames decoded ({len(rows)}) than sent ({len(ref)})"); bad += 1; continue
            diff = [i for i, (x, y) in enumerate(zip(rows, ref)) if x != y]
            whole = len(rows) == len(ref)
            if diff:
                i = diff[0]
                print(f"  FAIL {len(diff)} frames differ, first at frame {i}: {rows[i]} vs {ref[i]}"); bad += 1
            elif st["corrupt"]:
                print(f"  FAIL {st['corrupt']} corrupt frames"); bad += 1
            else:
                print(f"  ok   all {len(rows)} decoded frames equal the values sent"
                      + ("" if whole else f" ({len(ref) - len(rows)} frames after the cut never arrived)"))
    if refs and len(logs) != len(refs):
        print(f"FAIL {len(logs)} logs in the file, {len(refs)} sent"); bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
