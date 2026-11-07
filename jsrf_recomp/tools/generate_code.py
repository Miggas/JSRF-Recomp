#!/usr/bin/env python3
"""Generate the recompiled C code (src/recomp/gen/) from your own default.xbe.

The generated code is a translation of the game's own program, so it is not
kept in the repository: everyone generates it from their copy of the game.
This runs the same lift that produced the code the project is built and
tested with, from the analysis kept in analysis/ (function boundaries,
labels, library identification, calling conventions -- addresses and names,
no game code):

  1. tools.xbe_parser   the XBE's section layout (xboxrecomp/game_files/)
  2. tools.recomp       every function, split 250 to a file, with the
                        functions recomp_manual.c defines or wraps left out
                        or renamed to sub_X_gen
  3. recomp_extra.c     functions reached only through data tables or stored
                        pointers that the function list folds into a
                        neighbour (two from v66, one from v185)
  4. log_stubs.py,      the project's fix-ups applied to the generated files
     wrap_dsound.py     (idempotent: safe to run again)

usage:
  py -3 tools/generate_code.py [path/to/default.xbe] [--xboxrecomp DIR] [--any-xbe]

default.xbe defaults to jsrf_recomp/game/default.xbe, the toolkit to
../xboxrecomp next to jsrf_recomp. Takes about two minutes. Then build
(the launcher's Build).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xbe_info  # noqa: E402  (tools/xbe_info.py)

HERE = os.path.dirname(os.path.abspath(__file__))
JR = os.path.normpath(os.path.join(HERE, ".."))
ANALYSIS = os.path.join(JR, "analysis")
GEN = os.path.join(JR, "src", "recomp", "gen")
MANUAL = os.path.join(JR, "src", "recomp_manual.c")

# The check is in xbe_info.py: it compares the game's code and data
# sections with the NTSC (North American) retail release the analysis was
# made from, not the whole file, since dumping tools often patch the header.

# v66: reached only through data tables; the function list folds each into
# the function before it, so they are lifted on their own into recomp_extra.c
# and added to the dispatch table (start, end).
EXTRA = (("0x0002F420", "0x0002F5A0"), ("0x000D02F0", "0x000D03F0"),
         # v185: stored as a function pointer at 0x2584C8 by the init at
         # 0x18C000 and called every frame from the future site's tutorial
         # (Professor K's lock-on lesson and the machine) on; folded into
         # sub_000E9A40, so each call failed ([ICALL] Failed to resolve)
         ("0x000E9C30", "0x000E9D80"),
         # v186e: a thiscall method (state 3 / 8, +0x98 = 255.0) reached only
         # by a tail jump from 0x107FD5 in VS mode; folded into sub_00109B70.
         # Skipped, the item box showed its item at once instead of cycling.
         ("0x00109FA0", "0x0010A035"))
EXTRA_HEADER = """/**
 * Jet Set Radio Future - functions the detector folded into a neighbour
 * and that are reached only through data tables (v66).
 *   0x0002F420  loader job 0x13 unload (table 0x1EC288[0x13]); without it
 *               job 19 stays "loaded" and BOOT state 44 waits forever
 *               when the graffiti screen is entered a second time.
 *   0x000D02F0  vtable 0x1CE3B8 slot 4.
 *   0x000E9C30  (v185) a function pointer stored at 0x2584C8, called every
 *               frame in the future site's tutorial (the machine, Professor
 *               K's lock-on lesson); it failed to resolve, so it never ran.
 *   0x00109FA0  (v186e) reached by a tail jump from 0x107FD5 in VS mode
 *               (item boxes); it was a stub that skipped its body.
 * Lifted with tools.recomp -f against functions.json plus these entries.
 */

#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <math.h>

"""


def die(msg):
    print("\nERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def run(cmd, cwd, capture=False):
    print("  > " + " ".join(os.path.basename(c) if os.path.isabs(c) and c.endswith(".py") else c
                            for c in cmd))
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE if capture else None,
                       universal_newlines=True if capture else None)
    if r.returncode != 0:
        die("step failed (exit code %d)" % r.returncode)
    return r.stdout


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description="Generate src/recomp/gen from default.xbe")
    ap.add_argument("xbe", nargs="?", default=os.path.join(JR, "game", "default.xbe"))
    ap.add_argument("--xboxrecomp", default=os.path.normpath(os.path.join(JR, "..", "xboxrecomp")))
    ap.add_argument("--any-xbe", action="store_true",
                    help="go on even if default.xbe is not the version the analysis is for")
    a = ap.parse_args()

    xr = os.path.abspath(a.xboxrecomp)
    py = sys.executable
    try:
        import capstone  # noqa: F401  (the toolkit's disassembler)
    except ImportError:
        die("Python package 'capstone' is missing: py -3 -m pip install capstone==5.0.7")
    if not os.path.isfile(os.path.join(xr, "tools", "recomp", "__main__.py")):
        die("no xboxrecomp toolkit at %s (pass --xboxrecomp DIR)" % xr)
    if not os.path.isfile(a.xbe):
        die("%s not found -- copy your game files into jsrf_recomp/game/" % a.xbe)
    print("Checking default.xbe...")
    try:
        rel, info, results = xbe_info.identify(a.xbe)
    except Exception as e:  # not an XBE, truncated, unreadable
        die("could not read %s: %s" % (a.xbe, e))
    digest = info["file_sha256"]
    if not rel:
        msg = ("this default.xbe's code is not a release the project knows\n"
               + xbe_info.describe(info, results) + "\n"
               "  Known: %s. " % ", ".join(r[1] for r in xbe_info.releases()))
        if not a.any_xbe:
            die(msg + "--any-xbe to try anyway (as the NTSC release); the result will very likely not work.")
        print("WARNING: " + msg)
        rel = xbe_info.releases()[0][:3]
    else:
        print("  release: %s" % rel[1])
    tag, an = rel[0], rel[2]
    for n in ("functions_named.json", "labels.json", "identified_functions.json",
              "abi_functions.json"):
        if not os.path.isfile(os.path.join(an, n)):
            die("%s is missing" % os.path.relpath(os.path.join(an, n), JR))
    # Another release: our code's US addresses moved to it
    # (analysis/<tag>/addr_map.txt, tools/translate_release.py).
    tr = None
    if tag != "usa":
        import translate_release
        amap = translate_release.load_map(tag)
        tr = lambda text, python=False: translate_release.translate(text, amap, tag, python)

    # 1. the toolkit reads <stem>_analysis.json beside the XBE
    gf = os.path.join(xr, "game_files")
    os.makedirs(gf, exist_ok=True)
    xbe = os.path.join(gf, "default.xbe")
    if not os.path.isfile(xbe) or sha256(xbe) != digest:
        shutil.copyfile(a.xbe, xbe)
    print("\n[1/4] Reading the XBE's layout...")
    run([py, "-m", "tools.xbe_parser", xbe, "--json", os.path.join(gf, "default_analysis.json")],
        xr, capture=True)

    # 2. the lift
    print("\n[2/4] Translating the game's code to C (a minute or two)...")
    os.makedirs(GEN, exist_ok=True)
    for f in os.listdir(GEN):
        if f.endswith((".c", ".h")) or f == "release.txt":
            os.remove(os.path.join(GEN, f))
    inputs = ["--functions", os.path.join(an, "functions_named.json"),
              "--labels", os.path.join(an, "labels.json"),
              "--identified", os.path.join(an, "identified_functions.json"),
              "--abi", os.path.join(an, "abi_functions.json")]
    manual = MANUAL
    tmpdir = None
    if tr:
        # the functions recomp_manual.c defines and wraps, at this release's addresses
        tmpdir = tempfile.mkdtemp(prefix="jsrf_gen_")
        manual = os.path.join(tmpdir, "recomp_manual.c")
        with open(manual, "w", encoding="utf-8", newline="\n") as f:
            f.write(tr(open(MANUAL, encoding="utf-8", errors="replace").read()))
    try:
        run([py, "-m", "tools.recomp", xbe, "--all", "--split", "250",
             "--game-name", "Jet Set Radio Future", "--gen-dir", GEN,
             "--exclude-manual", manual] + inputs, xr)
    finally:
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)

    # 3. recomp_extra.c
    print("\n[3/4] The functions reached only through data tables...")
    funcs = json.load(open(os.path.join(an, "functions_named.json")))
    have = {f["start"] for f in funcs}
    extra = EXTRA
    if tr:
        # at this release's addresses; the ones its function list already
        # holds (seeded) were lifted with the rest
        extra = tuple((tr('"%s"' % s)[1:-1], "0x%08X" % (int(tr('"%s"' % s)[1:-1], 16) + int(e, 16) - int(s, 16)))
                      for s, e in EXTRA)
        ep = os.path.join(an, "extra.json")
        if os.path.isfile(ep):   # the release's own (tools/versions/make_version.py)
            extra += tuple(tuple(x) for x in json.load(open(ep)))
    extra = tuple(sorted(set((s, e) for s, e in extra if s not in have)))
    for start, end in extra:
        if start not in have:
            s, e = int(start, 16), int(end, 16)
            funcs.append({"start": start, "end": end, "size": e - s,
                          "name": "sub_%08X" % s, "section": ".text"})
    tmp = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(funcs, tmp)
    tmp.close()
    bodies = []
    try:
        for start, _ in extra:
            ins = list(inputs)
            ins[1] = tmp.name
            out = run([py, "-m", "tools.recomp", xbe, "-f", start] + ins, xr, capture=True)
            i = out.find("/**")
            if i < 0:
                die("no function in the output for %s" % start)
            bodies.append(out[i:].rstrip() + "\n")
    finally:
        os.unlink(tmp.name)
    with open(os.path.join(GEN, "recomp_extra.c"), "w", newline="\n") as f:
        f.write(EXTRA_HEADER + "\n\n".join(bodies) + "\n")

    names = ["sub_%08X" % int(s, 16) for s, _ in extra]

    # A function the main lift only reached by a direct call or jump got an
    # empty placeholder in recomp_stubs_unresolved.c (v186e: 0x00109FA0, a
    # tail jump); now that recomp_extra.c defines it, the placeholder goes.
    sp = os.path.join(GEN, "recomp_stubs_unresolved.c")
    if os.path.isfile(sp):
        st = open(sp).read()
        kept = [l for l in st.split("\n")
                if not any(l.startswith("void %s(void) {" % n) for n in names)]
        if len(kept) != len(st.split("\n")):
            open(sp, "w", newline="\n").write("\n".join(kept))
    hp = os.path.join(GEN, "recomp_funcs.h")
    h = open(hp).read()
    decl = "".join("void %s(void);\n" % n for n in names)
    if decl not in h:
        anchor = "void sub_00145A5D(void);\n"
        if anchor in h:
            h = h.replace(anchor, anchor + decl, 1)
        else:
            k = h.rindex("#endif")
            h = h[:k] + decl + "\n" + h[k:]
        open(hp, "w", newline="\n").write(h)

    dp = os.path.join(GEN, "recomp_dispatch.c")
    d = open(dp).read()
    row = re.compile(r"^    \{ 0x([0-9A-F]{8})u, \(recomp_func_t\)\w+ \},\n", re.M)
    rows = list(row.finditer(d))
    if not rows:
        die("unexpected recomp_dispatch.c layout")
    added = 0
    for (start, _), n in zip(extra, names):
        va = int(start, 16)
        line = "    { 0x%08Xu, (recomp_func_t)%s },\n" % (va, n)
        if line in d:
            continue
        rows = list(row.finditer(d))
        after = [m for m in rows if int(m.group(1), 16) > va]
        at = after[0].start() if after else rows[-1].end()
        d = d[:at] + line + d[at:]
        added += 1
    m = re.search(r"static const size_t g_recomp_table_size = (\d+);", d)
    d = d[:m.start(1)] + str(int(m.group(1)) + added) + d[m.end(1):]
    open(dp, "w", newline="\n").write(d)

    # 4. the project's fix-ups
    print("\n[4/4] Project fix-ups...")
    run([py, os.path.join(HERE, "log_stubs.py"), os.path.join(GEN, "recomp_stubs_unresolved.c")], JR)
    run([py, os.path.join(HERE, "wrap_dsound.py"), GEN, "--release=" + tag], JR)
    # which release this code is for: the build moves our code's addresses
    # to it (CMakeLists.txt), the launcher checks it against that release's
    # expected code
    with open(os.path.join(GEN, "release.txt"), "w", newline="\n") as f:
        f.write(tag + "\n" + digest.lower() + "\n")   # v270f: and which file (the launcher checks it)

    n = len([f for f in os.listdir(GEN) if f.endswith(".c")])
    print("\nDone: %d files in src/recomp/gen. Now build the game (the launcher's Build)." % n)


if __name__ == "__main__":
    main()
