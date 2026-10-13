#!/usr/bin/env python3
"""Add every real-code target in recomp_stubs_unresolved.c to the seed list.

A stub is emitted for a branch or call whose target is not a detected function.
For JSRF every remaining stub was reached by a jump: code the detector put in
another function's body, or in no function at all. Seeding the target makes the
disassembler build it as a function (in a gap) or an alias entry (inside a
function: a separate copy from that address to the owner's end, owner intact),
so the branch runs the real code instead of skipping it.

Skipped: targets outside code sections, bytes that decode as data, and targets
whose first instruction reads flags (an alias cannot inherit them).

    py -3 tools/seed_stub_targets.py gen/recomp_stubs_unresolved.c default.xbe info.json analysis/seeds.json
Prints how many seeds were added; 0 means converged.
"""
import json
import re
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

stub_c, xbe, info_json, seeds_json = sys.argv[1:5]
stubs = [int(m.group(1), 16) for m in re.finditer(r"void sub_([0-9A-F]{8})\(void\)", open(stub_c).read())]
b = open(xbe, "rb").read()
info = json.load(open(info_json))
md = Cs(CS_ARCH_X86, CS_MODE_32)
CODE = (".text", "D3D", "DSOUND", "MMATRIX", "XGRPH", "XPP")
BAD = {"hlt", "in", "out", "iretd", "arpl", "bound", "daa", "das", "aaa", "aas", "int3",
       "outsb", "insb", "insd", "outsd", "lcall", "ljmp", "lds", "les", "cli", "sti", "sldt", "into"}
FLAGREAD = ("j", "adc", "sbb", "set", "cmov", "pushf", "lahf", "rcl", "rcr")


def section(v):
    for s in info["sections"]:
        if s["name"] in CODE and s["vaddr"] <= v < s["vaddr"] + s["rawsize"]:
            return s
    return None


keep = []
for v in stubs:
    s = section(v)
    if not s:
        continue
    o = s["raw"] + v - s["vaddr"]
    ins = list(md.disasm(b[o:o + 32], v, count=5))
    if len(ins) < 4 or any(k.mnemonic in BAD for k in ins):
        continue
    if any(f"{k.mnemonic} {k.op_str}" == "add byte ptr [eax], al" for k in ins[:3]):
        continue
    if ins[0].mnemonic.startswith(FLAGREAD) and ins[0].mnemonic != "jmp":
        continue
    keep.append(v)

old = {int(x["start"], 16) for x in json.load(open(seeds_json))}
new = sorted(set(keep) - old)
json.dump([{"start": f"0x{v:08X}"} for v in sorted(old | set(keep))], open(seeds_json, "w"), indent=0)
print(f"{len(stubs)} stubs, {len(new)} new seeds ({len(old | set(keep))} total)")
