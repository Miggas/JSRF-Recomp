#!/usr/bin/env python3
"""Make the analysis for another release of the game (developer tool).

usage:
  python3 tools/versions/make_version.py <US default.xbe> <other .xbe> <tag>
          [--xboxrecomp DIR]

Writes analysis/<tag>/ -- addresses only, no game code:
  functions_named.json, labels.json, identified_functions.json,
  abi_functions.json   the toolkit's analysis of that XBE (tools.disasm,
                       tools.func_id, tools.abi_analysis), seeded from ours
  seeds.json           our seeds, moved to that release
  xbsymboldb_symbols.txt  the XDK names, moved
  addr_map.txt         every game address our own code uses (recomp_manual.c,
                       jsrf_widescreen.c, the fix-up tools), US -> <tag>
  version.json         how to recognise the release (its sections' SHA-256)

generate_code.py reads these when it is given that release's XBE.
"""
import argparse
import bisect
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
JR = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(JR, "tools"))
from xbe_match import Matcher  # noqa: E402
from translate_release import addresses_in_text, strip_comments, ADDR_LO, ADDR_HI  # noqa: E402

ANALYSIS = os.path.join(JR, "analysis")

# The files whose game addresses are moved for another release.
SOURCES = ["src/recomp_manual.c", "src/jsrf_widescreen.c", "tools/wrap_dsound.py",
           "tools/generate_code.py"]

# Numbers in the range of addresses that are not addresses: kept as they are.
NOT_ADDRESSES = {
    0x00080000,   # recomp_manual.c GPS_WORDS: a size
    0x001D0000,   # jsrf_widescreen.c: "is it code" upper bound, inside .rdata in every release
    0x00100410,   # wrap_dsound.py SPINS: an NV2A register offset
}

NAMES = {"eur": "PAL (Europe)", "jpn": "NTSC-J (Japan)",
         "usa-combo": "NTSC (North America), Sega GT 2002 + JSRF disc"}

# Addresses the matcher cannot place, checked by hand (tag -> {US: other}).
OVERRIDES = {
    "eur": {
        0x00023790: 0x00023C20,   # the event's update: changed (PAL 50 Hz step); vtable 0x1C5668 slot 4
        0x0001E8C0: 0x0001E890,   # the event's end: changed (fields +8); called the same way
        0x00115F40: 0x00116970,   # jsrf_widescreen.c: the end of the character select's function (0x116470)
    },
}


def addresses_in(path):
    """Game addresses the file's code uses (comments left out)."""
    s = open(path, encoding="utf-8", errors="replace").read()
    return addresses_in_text(s, python=path.endswith(".py"))


def sections_fingerprint(xbe):
    d = open(xbe, "rb").read()
    u = lambda o: struct.unpack_from("<I", d, o)[0]
    base = u(0x104)
    n, so = u(0x11C), u(0x120) - base
    out = {}
    for i in range(n):
        o = so + i * 0x38
        ra, rs, nm = u(o + 12), u(o + 16), u(o + 20)
        name = d[nm - base:nm - base + 16].split(b"\0")[0].decode("latin-1")
        if name.startswith("$$X"):
            continue
        out[name] = hashlib.sha256(d[ra:ra + rs]).hexdigest()
    cert = u(0x118) - base
    return out, u(cert + 8)


def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        sys.exit("failed: %s\n%s" % (" ".join(cmd), r.stdout[-3000:]))
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("us_xbe")
    ap.add_argument("other_xbe")
    ap.add_argument("tag")
    ap.add_argument("--xboxrecomp", default=os.path.join(os.path.dirname(JR), "xboxrecomp"))
    a = ap.parse_args()
    out = os.path.join(ANALYSIS, a.tag)
    os.makedirs(out, exist_ok=True)
    py = sys.executable

    print("Matching the two releases...")
    fs = json.load(open(os.path.join(ANALYSIS, "identified_functions.json")))
    m = Matcher(a.us_xbe, a.other_xbe, [(int(f["start"], 16), int(f["end"], 16)) for f in fs])
    print("  %d of %d functions found in the other release" % (len(m.map), len(m.funcs)))
    over = OVERRIDES.get(a.tag, {})

    def move(va):
        if va in over:
            return over[va]
        if va in NOT_ADDRESSES:
            return va
        return m.any(va)

    # seeds (and the functions generate_code.py adds by hand)
    import generate_code  # noqa: E402  (tools/generate_code.py: EXTRA)
    seeds = [int(x["start"], 16) for x in json.load(open(os.path.join(ANALYSIS, "seeds.json")))]
    seeds += [int(s, 16) for s, _ in generate_code.EXTRA]
    moved, lost = set(), []
    for s in seeds:
        e = move(s)
        (moved.add(e) if e else lost.append(s))
    # every function our code names is a function start there too
    for src in SOURCES:
        text = strip_comments(open(os.path.join(JR, src), encoding="utf-8", errors="replace").read(),
                              python=src.endswith(".py"))
        for x in re.findall(r"\bsub_([0-9A-Fa-f]{8})(?![0-9A-Fa-f])", text):
            e = move(int(x, 16))
            if e:
                moved.add(e)
    # functions our analysis has whose code changed (so the matcher could
    # not place them), found through the data that points to them: the same
    # table slot in the other release (v270c: 0x2AD60 -> 0x2B300, called
    # through a pointer, never found, and the first level's loading hung)
    _un = sorted((int(f["start"], 16), int(f["end"], 16))
                 for f in json.load(open(os.path.join(ANALYSIS, "functions_named.json"))))
    # only functions of their own: an entry inside another US function (a
    # switch's case, a return address) seeded here would cut that function
    us_starts_all, _top = set(), 0
    for _s, _e in _un:
        if _s >= _top:
            us_starts_all.add(_s)
        _top = max(_top, _e)
    ot_text = m.ot.section(".text")
    lost_set = set(lost)
    us_end_all = {a: b - a for a, b in _un}
    ptr_size = {}
    by_ptr = 0
    for sec in (".rdata", ".data"):
        su = m.us.section(sec)
        if not su:
            continue
        for p in range(su[1], su[1] + min(su[2], su[4]) - 3, 4):
            v = struct.unpack_from("<I", m.us.mem, p)[0]
            # standalone US functions, and US seeds the matcher could not move
            # (v270e: 0x1FD10 -> 0x1FD40, a cutscene change went black)
            if (v not in us_starts_all and v not in lost_set) or v in m.map:
                continue
            ep = m.data(p)
            if ep is None:
                continue
            ev = struct.unpack_from("<I", m.ot.mem, ep)[0]
            if not (ot_text[1] <= ev < ot_text[1] + ot_text[2]) or ev in moved:
                continue
            i = bisect.bisect_right(m.keys, v) - 1
            d = (m.map[m.keys[i]] - m.keys[i]) if i >= 0 else 0
            if ev == v:
                continue        # the same number: not a moved pointer (v270d: 0x20004..0x200EF,
                                # a table of numbers, cut the event code into pieces)
            if abs((ev - v) - d) > 0x400:
                continue
            moved.add(ev)
            ptr_size[ev] = us_end_all.get(v, 0x40)
            by_ptr += 1
            if v in lost_set:
                lost_set.discard(v)
    lost = sorted(lost_set)
    print("  %d more found through the data pointing to them" % by_ptr)
    print("  seeds: %d moved, %d not found%s" % (len(moved), len(lost),
          (" (" + " ".join("%X" % x for x in lost) + ")") if lost else ""))

    # XDK names
    sym_out = []
    for line in open(os.path.join(ANALYSIS, "xbsymboldb_symbols.txt")):
        mm = re.search(r"= 0x([0-9a-f]+)\s*$", line)
        if not mm:
            continue
        e = move(int(mm.group(1), 16))
        if e:
            sym_out.append(line[:mm.start(1)] + "%08x" % e + line[mm.end(1):])
    open(os.path.join(out, "xbsymboldb_symbols.txt"), "w", newline="\n").writelines(sym_out)

    # the toolkit's analysis of the other XBE, in a scratch copy of the toolkit
    tmp = tempfile.mkdtemp(prefix="jsrf_ver_")
    try:
        xr = os.path.join(tmp, "xboxrecomp")
        shutil.copytree(a.xboxrecomp, xr, ignore=shutil.ignore_patterns(
            "game_files", "output", "__pycache__", "*.obj", ".git"))
        gf = os.path.join(xr, "game_files")
        os.makedirs(gf, exist_ok=True)
        shutil.copyfile(a.other_xbe, os.path.join(gf, "default.xbe"))
        xbe = os.path.join(gf, "default.xbe")
        run([py, "-m", "tools.xbe_parser", xbe, "--json", os.path.join(gf, "default_analysis.json")], xr)
        us_named = json.load(open(os.path.join(ANALYSIS, "functions_named.json")))
        json.dump([{"start": "0x%08X" % e} for e in sorted(moved)],
                  open(os.path.join(out, "seeds.json"), "w"), indent=0)
        print("Finding the functions (tools.disasm)...")
        run([py, "-m", "tools.disasm", xbe, "--force", "--seed-functions",
             os.path.join(out, "seeds.json")], xr)
        got = {int(f["start"], 16) for f in json.load(open(os.path.join(xr, "tools/disasm/output/functions.json")))}
        # Not seeded again: what the toolkit does not find here is, nearly
        # always, an entry in the middle of a function that only US code
        # jumped to, and a seed there would cut the function in two (v270b:
        # 0x179475 cut the DirectSound set-up and the game never opened).
        fl = sorted((int(f["start"], 16), int(f["end"], 16))
                    for f in json.load(open(os.path.join(xr, "tools/disasm/output/functions.json"))))
        starts = [x for x, _ in fl]

        def inside(va):
            i = bisect.bisect_right(starts, va) - 1
            return i >= 0 and fl[i][1] > va

        miss = [m.map[int(f["start"], 16)] for f in us_named
                if int(f["start"], 16) in m.map and m.map[int(f["start"], 16)] not in got]
        images = set(m.map.values())
        ours0 = set(moved)
        pre_drop = set()

        def holder(va):
            i = bisect.bisect_right(starts, va) - 1
            return starts[i] if i >= 0 and fl[i][1] > va else None

        free = []
        alone = {m.map[g] for g in us_starts_all if g in m.map}
        for x in miss:
            if x not in alone:
                continue        # inside another US function: never seeded (it would cut one)
            h = holder(x)
            if h is None:
                free.append(x)
            elif h not in images and h not in ours0:
                # inside a start only this release's toolkit run found (in
                # the padding before it, say): that start goes, ours is seeded
                pre_drop.add(h)
                free.append(x)
        print("  %d functions; of ours not found there: %d inside another (left out), %d seeded"
              % (len(got), len(miss) - len(free), len(free)))
        if free:
            moved |= set(free)
            json.dump([{"start": "0x%08X" % e} for e in sorted(moved)],
                      open(os.path.join(out, "seeds.json"), "w"), indent=0)
            run([py, "-m", "tools.disasm", xbe, "--force", "--seed-functions",
                 os.path.join(out, "seeds.json")], xr)
        # seeded and still not kept by the toolkit (0x7C6A0, just past a
        # switch table): lifted on their own, as generate_code.py's EXTRA
        got2 = {int(f["start"], 16) for f in json.load(open(os.path.join(xr, "tools/disasm/output/functions.json")))}
        us_end = {m.map[int(f["start"], 16)]: int(f["end"], 16) - int(f["start"], 16)
                  for f in us_named if int(f["start"], 16) in m.map}
        # and ours inside another function there that its code jumps or
        # calls to (0x41218: a tail jump into 0x41212; the US toolkit made it
        # an alias of its own, here it did not, and a seed would cut 0x41212)
        mem = m.ot.mem
        tlo, thi = ot_text[1], ot_text[1] + ot_text[2]
        want = set(x for x in miss if x not in free)
        hit = set()
        for q in range(tlo, thi - 5):
            b = mem[q]
            if b in (0xE8, 0xE9):
                t_ = q + 5 + struct.unpack_from("<i", mem, q + 1)[0]
            elif b == 0x0F and 0x80 <= mem[q + 1] <= 0x8F:
                t_ = q + 6 + struct.unpack_from("<i", mem, q + 2)[0]
            elif b == 0xEB or 0x70 <= b <= 0x7F:
                t_ = q + 2 + struct.unpack_from("<b", mem, q + 1)[0]
            else:
                continue
            if t_ in want and holder(t_) != holder(q):
                hit.add(t_)
        # seeds found through data pointers that the toolkit swallowed into
        # a start of its own (0x1FD40, inside a "gap" start at 0x1FD02)
        swallowed = set(x for x in ptr_size if x not in got2)
        extra = sorted(set(x for x in free if x not in got2) | hit | swallowed)
        size_of = dict(ptr_size)
        size_of.update(us_end)
        json.dump([["0x%08X" % x, "0x%08X" % (x + size_of[x])] for x in extra],
                  open(os.path.join(out, "extra.json"), "w"), indent=0)
        if extra:
            print("  lifted on their own: " + " ".join("%X" % x for x in extra))
        # A start the toolkit found inside a function that is the same code
        # as ours (from a number in the code that only looks like an
        # address) cuts that function short: its end then jumps to a stub
        # (v270b). Such starts go, and the function keeps its whole length.
        fp = os.path.join(xr, "tools/disasm/output/functions.json")
        fl = json.load(open(fp))
        by = {int(f["start"], 16): f for f in fl}
        us_starts = sorted(int(f["start"], 16) for f in us_named)
        ours = set(moved)
        drop = set(pre_drop)
        for f in us_named:
            g = int(f["start"], 16)
            if g not in m.verified or m.map[g] not in by:
                continue
            size = int(f["end"], 16) - g
            e0 = m.map[g]
            i = bisect.bisect_right(us_starts, g)
            inner = set()
            while i < len(us_starts) and us_starts[i] < g + size:
                if us_starts[i] in m.map:
                    inner.add(m.map[us_starts[i]])
                i += 1
            for st in by:
                if e0 < st < e0 + size and st not in inner and st not in ours:
                    drop.add(st)
            fe = by[e0]
            if int(fe["end"], 16) < e0 + size:
                fe["end"] = "0x%08X" % (e0 + size)
                fe["size"] = size
        fl = [f for f in fl if int(f["start"], 16) not in drop]
        json.dump(fl, open(fp, "w"), indent=2)
        lp = os.path.join(xr, "tools/disasm/output/labels.json")
        ll = [x for x in json.load(open(lp)) if not (x.get("type") == "function" and int(x["address"], 16) in drop)]
        json.dump(ll, open(lp, "w"), indent=2)
        print("  %d starts inside functions that are the same code as ours removed" % len(drop))
        run([py, "-m", "tools.func_id", xbe], xr)
        run([py, "-m", "tools.abi_analysis", xbe], xr)
        run([py, os.path.join(JR, "tools", "apply_xdk_names.py"), os.path.join(out, "xbsymboldb_symbols.txt"),
             os.path.join(xr, "tools/disasm/output/functions.json")], xr)
        for src, dst in (("tools/disasm/output/functions.json", "functions_named.json"),
                         ("tools/disasm/output/labels.json", "labels.json"),
                         ("tools/func_id/output/identified_functions.json", "identified_functions.json"),
                         ("tools/abi_analysis/output/abi_functions.json", "abi_functions.json")):
            shutil.copyfile(os.path.join(xr, src), os.path.join(out, dst))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # our code's addresses
    need = set()
    for s in SOURCES:
        need |= addresses_in(os.path.join(JR, s))
    rows, bad = [], []
    for v in sorted(need):
        e = move(v)
        if e is None:
            bad.append(v)
        else:
            rows.append("%08X %08X\n" % (v, e))
    with open(os.path.join(out, "addr_map.txt"), "w", newline="\n") as f:
        f.write("# US address -> %s, for every game address our code uses (tools/versions/make_version.py)\n" % a.tag)
        f.writelines(rows)
    print("  addresses of our code: %d moved" % len(rows))
    if bad:
        print("  NOT FOUND (add to OVERRIDES or NOT_ADDRESSES): " + " ".join("%08X" % v for v in bad))

    secs, title = sections_fingerprint(a.other_xbe)
    json.dump({"tag": a.tag, "name": NAMES.get(a.tag, a.tag), "title_id": "%08X" % title, "sections": secs},
              open(os.path.join(out, "version.json"), "w"), indent=1)
    print("Done: analysis/%s/" % a.tag)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
