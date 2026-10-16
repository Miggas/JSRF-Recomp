"""v115c: wrap generated functions without a re-lift (v118b: also sub_00039D40).

recomp_manual.c defines DSOUND_IDirectSound_SetI3DL2Listener and
DSOUND_IDirectSound_SetEffectData itself (they catch the title's reverb
settings) and calls the generated bodies as <name>_gen. This renames the
generated definitions in src/recomp/gen to <name>_gen. Callers and the
dispatch table keep the plain names, so they reach the hand-written
wrappers. Idempotent; run it again after any future lift.

usage: py -3 wrap_dsound.py [path to src/recomp/gen]
"""
import os
import re
import sys

NAMES = ("DSOUND_IDirectSound_SetI3DL2Listener", "DSOUND_IDirectSound_SetEffectData",
         "sub_00039D40",   # v118b: the in-air input crash guard
         "D3D8_D3DDevice_SetTexture",   # v124: who sets the spark texture
         "sub_001A2E2E",                # v130: the idle-voice crash guard
         "XAPILIB_XInputSetState",      # v186c: the rumble trace
         "sub_00153FE0")                # v187: widescreen culling


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    gen = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "src", "recomp", "gen")
    done = {n: 0 for n in NAMES}
    already = {n: 0 for n in NAMES}
    for fn in sorted(os.listdir(gen)):
        if not fn.endswith(".c"):
            continue
        path = os.path.join(gen, fn)
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            src = f.read()
        out = src
        for n in NAMES:
            # a definition: "void NAME(void)" at line start, not a declaration
            pat = re.compile(r"^void %s\(void\)(?!\s*;)" % re.escape(n), re.M)
            out, k = pat.subn("void %s_gen(void)" % n, out)
            done[n] += k
            already[n] += len(re.findall(r"^void %s_gen\(void\)(?!\s*;)" % re.escape(n), src, re.M))
        if out != src:
            with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(out)
            print("patched", fn)
    ok = True
    for n in NAMES:
        total = done[n] + already[n]
        print("%s: %d renamed now, %d already renamed" % (n, done[n], already[n]))
        if total != 1:
            print("ERROR: expected exactly one definition of %s in %s" % (n, gen))
            ok = False
    return 0 if ok else 1


# v125: rcpss / rsqrtss as the hardware does them. The lifter writes them as an
# exact 1/x and 1/sqrt(x); a Pentium III returns a 12-bit approximation, and
# the title's matrix code (an unrefined Gauss-Jordan inverse in MMATRIX,
# sub_001BB460) was written against that.
# v126: each site is rewritten to recomp_rcpss_at(x, id) / recomp_rsqrtss_at(x, id)
# (recomp_manual.c), id numbered in file and line order, so single sites can be
# switched back to exact (RECOMP_SSE_EXACT_SITES). Lines already rewritten by
# v125 or v126 are renumbered. Idempotent.
SSE_PROTO_OLD = "float recomp_rcpss(float x); float recomp_rsqrtss(float x); /* v125 */\n"
SSE_PROTO = "float recomp_rcpss_at(float x, int id); float recomp_rsqrtss_at(float x, int id); /* v126 */\n"
SITE = re.compile(r"= (?:1\.0f / sqrtf\((?P<a>.+)\)|recomp_rsqrtss(?:_at)?\((?P<b>.+?)(?:, \d+)?\)|"
                  r"1\.0f / (?P<c>.+)|recomp_rcpss(?:_at)?\((?P<d>.+?)(?:, \d+)?\)); /\* (?P<k>rcpss|rsqrtss) \*/")


def sse_pass(gen):
    n = 0
    for fn in sorted(os.listdir(gen)):
        if not fn.endswith(".c"):
            continue
        path = os.path.join(gen, fn)
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            src = f.read()
        here = []

        def rep(m):
            nonlocal n
            k = m.group("k")
            if k == "rsqrtss":
                x = m.group("a") if m.group("a") is not None else m.group("b")
                if x is None:
                    return m.group(0)
            else:
                x = m.group("c") if m.group("c") is not None else m.group("d")
                if x is None:
                    return m.group(0)
            here.append((n, k))
            n += 1
            return "= recomp_%s_at(%s, %d); /* %s */" % (k, x, n - 1, k)

        out = SITE.sub(rep, src)
        if here:
            out = out.replace(SSE_PROTO_OLD, "")
            if SSE_PROTO not in out:
                i = out.find("\n", out.find("#include")) + 1
                out = out[:i] + SSE_PROTO + out[i:]
            if out != src:
                with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                    f.write(out)
            print("sse: %s: sites %s" % (fn, " ".join("%d(%s)" % h for h in here)))
    print("sse: %d sites" % n)


# Undo of v131-v137's widescreen line in recomp_0035.c (the generated code is
# not part of the update zips, so going back to v130 has to put it back).
def ws_undo(gen):
    path = os.path.join(gen, "recomp_0035.c")
    if not os.path.exists(path):
        return
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        src = f.read()
    out = src.replace("    edx = recomp_ws_xscale();\n", "    edx = 0x3F800000;\n")
    out = out.replace("uint32_t recomp_ws_xscale(void); /* v131 */\n", "")
    if out != src:
        with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(out)
        print("ws: widescreen line restored to the original")


# v152r: functions a later build wrapped and this one no longer does -- their
# generated definitions are given their plain names back (v166 wrapped D3D's
# vblank DPC, sub_00193D90; reverted with the rest of the RECOMP_ACK_FAST work).
UNWRAP = ("sub_00193D90",
          "D3D8_D3DDevice_SetTransform")   # v187c wrapped it, v187d does not


def unwrap_pass(gen):
    for fn in sorted(os.listdir(gen)):
        if not fn.endswith(".c"):
            continue
        path = os.path.join(gen, fn)
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            src = f.read()
        out = src
        for n in UNWRAP:
            out = re.sub(r"^void %s_gen\(void\)(?!\s*;)" % re.escape(n), "void %s(void)" % n,
                         out, flags=re.M)
        if out != src:
            with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(out)
            print("unwrapped", fn)



# v182 took out what v178-v181 patched into the generated code. v183 puts
# v178's two wait patches back (SPINS: whether each one sleeps or spins is
# chosen at run time, RECOMP_SLEEP_WAITS) and keeps v180's PUT-write wakes
# out (UNSPINS). Both idempotent.
SPINS = (
    ("if (TEST_NZ(_fa, _fb)) goto loc_001912E0; /* jne: not equal / not zero */",
     "if (TEST_NZ(_fa, _fb)) { extern void xbox_Nv2aWaitClear(volatile uint32_t *r, uint32_t m);"
     " xbox_Nv2aWaitClear(&MEM32(eax + 0x100410u), 0x10000u); goto loc_001912E0; }"
     " /* jne; v178: sleeps until acked */"),
    ("if (TEST_NZ(_fa, _fb)) goto loc_00193E40; /* jne: not equal / not zero */",
     "if (TEST_NZ(_fa, _fb)) { extern void xbox_Nv2aWaitClear(volatile uint32_t *r, uint32_t m);"
     " xbox_Nv2aWaitClear(&MEM32(ebx + 0x100u), 0x1000000u); goto loc_00193E40; }"
     " /* jne; v178: sleeps until acked */"),
)

UNSPINS = (
    # v180: CDevice_KickOff's two DMA_PUT writes wake the executor, which now
    # sleeps when it has nothing to do
    ("    MEM32(eax + 0x40) = edx;\n    edx = MEM32(esp);\n    MEM32(edx + 0x2C) = ecx;\n",
     "    MEM32(eax + 0x40) = edx; { extern void xbox_Nv2aPutWritten(void); xbox_Nv2aPutWritten(); }"
     " /* v180: DMA_PUT written, wake the executor */\n    edx = MEM32(esp);\n    MEM32(edx + 0x2C) = ecx;\n"),
    ("    MEM32(ecx + 0x40) = eax;\n    edx = MEM32(esp);\n    ecx = MEM32(edx + 0x2264);\n    MEM32(ecx + 0x44) = eax;\n",
     "    MEM32(ecx + 0x40) = eax;\n    edx = MEM32(esp);\n    ecx = MEM32(edx + 0x2264);\n    MEM32(ecx + 0x44) = eax;"
     " { extern void xbox_Nv2aPutWritten(void); xbox_Nv2aPutWritten(); } /* v180: PUT and GET reset, wake the executor */\n"),
)


def unspin_pass(gen):
    n = 0
    for fn in sorted(os.listdir(gen)):
        if not fn.endswith(".c"):
            continue
        path = os.path.join(gen, fn)
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            src = f.read()
        out = src
        for old, new in UNSPINS:
            n += out.count(new)
            out = out.replace(new, old)
        if out != src:
            with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(out)
            print("v180 PUT-write wakes taken out of", fn)
    return n


def spin_pass(gen):
    found = [0] * len(SPINS)
    for fn in sorted(os.listdir(gen)):
        if not fn.endswith(".c"):
            continue
        path = os.path.join(gen, fn)
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            src = f.read()
        out = src
        for i, (old, new) in enumerate(SPINS):
            out = out.replace(old, new)
            found[i] += out.count(new)
        if out != src:
            with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(out)
            print("spin waits patched in", fn)
    for i, n in enumerate(found):
        if n != 1:
            print("WARNING: spin wait %d found %d times (expected 1)" % (i, n))


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    gen_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "src", "recomp", "gen")
    unwrap_pass(gen_dir)
    unspin_pass(gen_dir)
    rc = main()
    sse_pass(gen_dir)
    ws_undo(gen_dir)
    spin_pass(gen_dir)
    sys.exit(rc)
