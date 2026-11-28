#!/usr/bin/env python3
"""Build the hosted web configurator from web/ into a folder.

    python3 configurator/build_site.py OUT [--version V]

What the site needs beyond a plain copy of web/:
  presets.json   configurator/presets/*.json as one file (what /api/presets serves locally)
  board-files/   the board scripts "Install / update on board" pushes over USB
  sw.js          a cache name per version and the full list of site files (offline use)
  index.html     ?v=<version> on style.css / app.js, so a new deploy is never mixed
                 with files the browser cached from the last one
The Pages workflow runs this; run it by hand to try a build (python3 -m http.server).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARD_FILES = ["files/fpv", "files/S99fpv", "files/wfb.sh", "files/rkfpv.conf", "files/wfb.yaml"]


def version():
    try:
        return subprocess.check_output(["git", "-C", REPO, "rev-parse", "--short=12", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "dev"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--version", default=None)
    a = ap.parse_args()
    ver = a.version or version()
    # every UI word comes from web/i18n/*.json: stop on a key the code uses but en.json lacks
    if subprocess.call([sys.executable, os.path.join(REPO, "configurator", "check_i18n.py")]) != 0:
        sys.exit("build_site: fix the language files first (configurator/check_i18n.py)")
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    shutil.copytree(os.path.join(REPO, "web"), out)

    presets = []
    d = os.path.join(REPO, "configurator", "presets")
    for f in sorted(os.listdir(d)):
        if f.endswith(".json"):
            with open(os.path.join(d, f), encoding="utf-8") as fh:
                p = json.load(fh)
            presets.append({"file": f, "name": str(p["name"]), "description": str(p.get("description", "")),
                            "values": {str(k): str(v) for k, v in p["values"].items()}})
    with open(os.path.join(out, "presets.json"), "w") as fh:
        json.dump({"ok": True, "presets": presets}, fh, indent=1)

    os.makedirs(os.path.join(out, "board-files"))
    for rel in BOARD_FILES:
        with open(os.path.join(REPO, rel), "rb") as fh:
            data = fh.read().replace(b"\r\n", b"\n")      # LF: the board's shell chokes on CR
        with open(os.path.join(out, "board-files", os.path.basename(rel)), "wb") as fh:
            fh.write(data)

    files = ["./"]
    for root, _, names in os.walk(out):
        for n in sorted(names):
            rel = os.path.relpath(os.path.join(root, n), out).replace(os.sep, "/")
            if rel != "sw.js" and not rel.startswith("board-files/") and not rel.endswith((".map", "LICENSE")):
                files.append(rel)
    sw = os.path.join(out, "sw.js")
    with open(sw, encoding="utf-8") as fh:
        s = fh.read()
    s = s.replace("__VERSION__", ver)
    s = re.sub(r"/\*FILES\*/.*?/\*END\*/", "\n  " + ",\n  ".join(json.dumps(f) for f in files) + ",\n",
               s, flags=re.S)
    with open(sw, "w", encoding="utf-8") as fh:
        fh.write(s)

    idx = os.path.join(out, "index.html")
    with open(idx, encoding="utf-8") as fh:
        s = fh.read()
    s = s.replace('href="style.css"', f'href="style.css?v={ver}"').replace('src="app.js"', f'src="app.js?v={ver}"')
    with open(idx, "w", encoding="utf-8") as fh:
        fh.write(s)
    print(f"site {ver}: {len(files)} files cached offline, {len(presets)} presets -> {out}")


if __name__ == "__main__":
    main()
