#!/usr/bin/env python3
"""Add a 2x2-binned resolution to the LSC (lens shading) section of a rkaiq IQ json.

rkaiq refuses to run LSC for a sensor mode that has no entry in lscResName
("ALSC:E:can't find 1152x648 in lscResName"); AE then sees a dark image and
drives exposure to the maximum (all-white picture). 2x2 binning keeps the field
of view, so the full-resolution tables apply as they are: only the grid cell
sizes are halved.

usage: iq-add-binned-lsc.py in.json out.json [SRC_WxH [DST_WxH]]
"""
import copy
import json
import sys


def main():
    src_json, dst_json = sys.argv[1], sys.argv[2]
    src = sys.argv[3] if len(sys.argv) > 3 else "2304x1296"
    dst = sys.argv[4] if len(sys.argv) > 4 else "1152x648"
    w, h = (int(v) for v in dst.split("x"))
    j = json.load(open(src_json))
    n = 0
    for main_scene in j["main_scene"]:
        for sub in main_scene["sub_scene"]:
            for key, isp in sub.items():
                if not isinstance(isp, dict) or "lsc_v2" not in isp:
                    continue
                lsc = isp["lsc_v2"]
                res = lsc["common"]["resolutionAll"]
                if any(r["name"] == dst for r in res):
                    continue
                base = next(r for r in res if r["name"] == src)
                r = copy.deepcopy(base)
                r["name"] = dst
                # 16 cells across the whole width / height
                r["lsc_sect_size_x"] = [w // 16 + (1 if i < w % 16 else 0) for i in range(16)]
                # spread the remainder evenly (648 -> 40,41,40,41,...)
                rem = h % 16
                r["lsc_sect_size_y"] = [h // 16 + (1 if (i * rem) // 16 != ((i + 1) * rem) // 16 else 0)
                                        for i in range(16)]
                assert sum(r["lsc_sect_size_x"]) == w and sum(r["lsc_sect_size_y"]) == h
                res.append(r)
                lsc["common"]["resolutionAll_len"] = len(res)
                tables = lsc["tbl"]["tableAll"]
                for t in [t for t in tables if t["resolution"] == src]:
                    t2 = copy.deepcopy(t)
                    t2["name"] = t["name"].replace(src, dst)
                    t2["resolution"] = dst
                    tables.append(t2)
                lsc["tbl"]["tableAll_len"] = len(tables)
                n += 1
    json.dump(j, open(dst_json, "w"), separators=(",", ":"))
    print(f"{dst}: added to {n} LSC section(s)")


if __name__ == "__main__":
    main()
