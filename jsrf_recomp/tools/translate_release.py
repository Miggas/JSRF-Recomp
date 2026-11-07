#!/usr/bin/env python3
"""Move our code's game addresses to another release of the game.

recomp_manual.c and jsrf_widescreen.c name the US (NTSC) release's
functions (sub_XXXXXXXX, loc_XXXXXXXX) and data (0x00XXXXXX). For another release,
analysis/<tag>/addr_map.txt lists where each of them is there (made by
tools/versions/make_version.py), and this writes a copy of the file with
them replaced, plus `#define JSRF_RELEASE_<TAG> 1` at the top.

usage:  python3 tools/translate_release.py <tag> <source.c> <out.c>

Run by the build (CMakeLists.txt) and by generate_code.py. Every address
the file's code uses must be in the map: a missing one stops here, rather
than leaving a US address in another release's build.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
JR = os.path.dirname(HERE)
ADDR_LO, ADDR_HI = 0x11000, 0x290000

_SUB = re.compile(r"\b(sub|loc)_([0-9A-Fa-f]{8})(?![0-9A-Fa-f])")
_HEX = re.compile(r"\b0x([0-9A-Fa-f]{5,8})(u?)\b")


def strip_comments(s, python=False):
    if python:
        return re.sub(r"#[^\n]*", "", s)
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def addresses_in_text(s, python=False):
    """Game addresses the code uses (comments left out)."""
    code = strip_comments(s, python)
    out = {int(x, 16) for _, x in _SUB.findall(code)}
    for x, _ in _HEX.findall(code):
        v = int(x, 16)
        if ADDR_LO <= v < ADDR_HI:
            out.add(v)
    return out


def load_map(tag):
    path = os.path.join(JR, "analysis", tag, "addr_map.txt")
    m = {}
    for line in open(path):
        p = line.split()
        if len(p) == 2 and not line.startswith("#"):
            m[int(p[0], 16)] = int(p[1], 16)
    return m


def translate(text, amap, tag, python=False):
    missing = sorted(v for v in addresses_in_text(text, python) if v not in amap)
    if missing:
        raise SystemExit("translate_release: %d address(es) not in analysis/%s/addr_map.txt "
                         "(run tools/versions/make_version.py again): %s"
                         % (len(missing), tag, " ".join("%08X" % v for v in missing)))

    def sub(mo):
        v = int(mo.group(2), 16)
        return "%s_%08X" % (mo.group(1), amap[v]) if v in amap else mo.group(0)

    def hexlit(mo):
        v = int(mo.group(1), 16)
        if v in amap and ADDR_LO <= v < ADDR_HI:
            return "0x%08X%s" % (amap[v], mo.group(2))
        return mo.group(0)

    out = _SUB.sub(sub, text)
    return _HEX.sub(hexlit, out)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    tag, src, dst = sys.argv[1:4]
    text = open(src, encoding="utf-8", errors="replace").read()
    out = translate(text, load_map(tag), tag)
    head = ("/* %s for the %s release: made by tools/translate_release.py -- edit the original */\n"
            "#define JSRF_RELEASE_%s 1\n" % (os.path.basename(src), tag, tag.upper().replace("-", "_")))
    head += '#line 1 "%s"\n' % src.replace("\\", "/")
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    old = open(dst, encoding="utf-8").read() if os.path.isfile(dst) else None
    if old != head + out:   # unchanged: leave the file alone (no needless rebuild)
        with open(dst, "w", encoding="utf-8", newline="\n") as f:
            f.write(head + out)


if __name__ == "__main__":
    main()
