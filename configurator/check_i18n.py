#!/usr/bin/env python3
"""Check the web configurator's language files (web/i18n/*.json).

    python3 configurator/check_i18n.py          report, exit 1 on errors

Errors (the build stops):
  - a key the code uses (t("..."), tf("..."), data-i18n*) that en.json lacks
  - {placeholders} in a translation that differ from the English ones
  - param.* / preset.* keys for settings or presets that do not exist
Warnings:
  - keys in en.json nothing uses
  - keys a language has not translated yet (they show in English)
  - settings in files/fpv without a translation in a language

en.json is the reference. Setting help (param.<key>) and preset texts
(preset.<file>.name/.desc) are only in the other languages: in English
they come from the board (files/fpv) and the preset files themselves.
"""
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(REPO, "web")
I18N = os.path.join(WEB, "i18n")
# keys built at run time from data: t(`badge.${mode}`) and the like
DYNAMIC = ("badge.", "group.", "param.", "preset.", "dvr.state.", "mode.", "iq.where.", "restart.", "restarted.")
QUOTED = re.compile(r"""["']([a-z][\w.&-]*)["']""")


def first_args(text):
    """the first argument of every t(...) / tf(...) call, as source text"""
    for m in re.finditer(r"\bt[f]?\(", text):
        i, depth, quote = m.end(), 1, None
        start = i
        while i < len(text) and depth:
            c = text[i]
            if quote:
                if c == "\\":
                    i += 1
                elif c == quote:
                    quote = None
            elif c in "\"'`":
                quote = c
            elif c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 1:
                break
            i += 1
        yield text[start:i]


def used_keys():
    """(keys passed to t()/tf(), every quoted string in the code)"""
    called, quoted = set(), set()
    for name in sorted(os.listdir(WEB)):
        path = os.path.join(WEB, name)
        if name.endswith(".js") and name != "i18n.js":
            with open(path, encoding="utf-8") as f:
                text = f.read()
            quoted.update(QUOTED.findall(text))
            for arg in first_args(text):
                # "a.b" keys; plain words in a ternary condition (=== "usb") are not keys
                called.update(k for k in QUOTED.findall(arg) if "." in k)
        elif name.endswith(".html"):
            with open(path, encoding="utf-8") as f:
                keys = re.findall(r'data-i18n(?:-html|-title|-placeholder)?="([^"]+)"', f.read())
            called.update(keys)
            quoted.update(keys)
    return called, quoted


def placeholders(s):
    return sorted(set(re.findall(r"\{(\w+)\}", s)))


def board_params():
    keys = set()
    with open(os.path.join(REPO, "files", "fpv"), encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("|")
            if len(parts) == 10 and re.fullmatch(r"[a-z][a-z0-9_]*", parts[0]):
                keys.add(parts[0])
    return keys


def main():
    errors, warnings = [], []
    langs = {}
    for f in sorted(os.listdir(I18N)):
        if f.endswith(".json"):
            with open(os.path.join(I18N, f), encoding="utf-8") as fh:
                langs[f[:-5]] = json.load(fh)
    en = langs.pop("en")
    called, quoted = used_keys()
    params = board_params()
    presets = set(os.listdir(os.path.join(REPO, "configurator", "presets")))

    for k in sorted(called - set(en)):
        if not k.startswith(("param.", "preset.")):
            errors.append(f"en.json: missing key {k!r} (used in web/)")
    for k in sorted(set(en) - quoted):
        if not k.startswith(DYNAMIC):
            warnings.append(f"en.json: {k!r} is not used")

    for code, tr in langs.items():
        for k, v in tr.items():
            if k.startswith("param."):
                if k[6:] not in params:
                    errors.append(f"{code}.json: {k!r}: no such setting in files/fpv")
                continue
            if k.startswith("preset."):
                if k.split(".")[1] + ".json" not in presets:
                    errors.append(f"{code}.json: {k!r}: no such preset file")
                continue
            if k not in en:
                errors.append(f"{code}.json: {k!r} is not in en.json")
            elif placeholders(v) != placeholders(en[k]):
                errors.append(f"{code}.json: {k!r} placeholders {placeholders(v)} != en {placeholders(en[k])}")
        untranslated = [k for k in en if k not in tr]
        if untranslated:
            warnings.append(f"{code}.json: {len(untranslated)} keys untranslated: {', '.join(untranslated[:10])}"
                            + (" …" if len(untranslated) > 10 else ""))
        no_help = sorted(p for p in params if f"param.{p}" not in tr)
        if no_help:
            warnings.append(f"{code}.json: settings without a translation: {', '.join(no_help)}")

    for w in warnings:
        print("warning:", w)
    for e in errors:
        print("error:", e)
    print(f"i18n: {len(en)} keys, languages: en, {', '.join(langs)}; {len(errors)} errors, {len(warnings)} warnings")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
