#!/usr/bin/env python3
"""Compare src/recomp/gen with the code this version of the project is expected
to produce from the NTSC default.xbe (analysis/gen_hashes.txt, SHA-256 of each
file with Linux line endings). After generate_code.bat:  py -3 tools/check_gen.py"""
import hashlib, os, sys
here = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
gen = os.path.join(here, "src", "recomp", "gen")
want = {}
for line in open(os.path.join(here, "analysis", "gen_hashes.txt")):
    h, name = line.split()
    want[name] = h
try:
    import capstone
    print("capstone", capstone.__version__)
except ImportError:
    print("capstone not installed")
bad = 0
for name, h in sorted(want.items()):
    p = os.path.join(gen, name)
    if not os.path.isfile(p):
        print("  missing:", name); bad += 1; continue
    data = open(p, "rb").read().replace(b"\r\n", b"\n")   # Windows line endings do not count
    got = hashlib.sha256(data).hexdigest().upper()
    if got != h:
        print("  DIFFERENT:", name, "(not just line endings)"); bad += 1
extra = [f for f in os.listdir(gen) if f.endswith((".c", ".h")) and f not in want]
for f in extra:
    print("  extra:", f); bad += 1
print("\nall %d files match" % len(want) if not bad else "\n%d file(s) differ from the expected code" % bad)
