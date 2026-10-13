#!/usr/bin/env python3
"""Propose and validate --coalesce-functions bounds for JSRF.

Every stub in recomp_stubs_unresolved.c that the generated code reaches by a
*jump* (never a call) is a branch from one detected function into code the
detector assigned elsewhere, or to nobody. The lifter cannot branch into
another function's body, so that code is silently skipped.

For each such jump this proposes an owner spanning the source function and
the target, then asks the toolkit's own validator (FunctionTranslator.
coalesce_function) to accept or reject it. That validator refuses to merge any
piece with independent entry evidence (callers, prologue, code pointers,
seeds) and requires a gap-free decoded CFG, so accepted entries are safe.

Run from the xboxrecomp checkout (needs its tools package):
    py -3 <this> default.xbe gen_dir recomp_manual.c out.json
"""
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.getcwd())
from tools.recomp import config                                    # noqa: E402
from tools.recomp.__main__ import _load_manual_protection, find_data_files  # noqa: E402
from tools.recomp import translator as _tmod                       # noqa: E402
from tools.recomp.translator import BatchTranslator                # noqa: E402
import tempfile

xbe, gen_dir, manual_c, out_path = sys.argv[1:5]

config.configure_from_xbe(xbe)
protected, _ = _load_manual_protection(None, manual_c)
files = find_data_files(disasm_dir=None, func_id_dir=None, abi_dir=None, overrides={})
# Build the translator exactly as `recomp --coalesce-functions` does, but stop
# before CFG ownership so coalesce_function can still be called: pass an empty
# coalescence file (selects the coalescing path) and no-op the ownership passes.
empty = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
empty.write("[]"); empty.close()
_own, _jt = _tmod.FunctionTranslator.discover_cfg_ownership, _tmod.FunctionTranslator.discover_jump_table_entries
_tmod.FunctionTranslator.discover_cfg_ownership = lambda self: None
_tmod.FunctionTranslator.discover_jump_table_entries = lambda self: None
bt = BatchTranslator(xbe_path=xbe, func_json_path=files["functions"],
                     labels_json_path=files.get("labels"),
                     identified_json_path=files.get("identified"),
                     abi_json_path=files.get("abi"), output_dir=None,
                     coalesce_json_paths=[empty.name],
                     protected_function_starts=protected)
_tmod.FunctionTranslator.discover_cfg_ownership, _tmod.FunctionTranslator.discover_jump_table_entries = _own, _jt
tr = bt.translator
db = tr.func_db

# --- which generated function jumps to which stub -------------------------
stub_src = open(os.path.join(gen_dir, "recomp_stubs_unresolved.c")).read()
stubs = sorted(int(m.group(1), 16) for m in re.finditer(r"void sub_([0-9A-F]{8})\(void\)", stub_src))
dispatch = open(os.path.join(gen_dir, "recomp_dispatch.c")).read()
name_to_va = {m.group(2): int(m.group(1), 16)
              for m in re.finditer(r"\{ 0x([0-9A-F]{8})u, \(recomp_func_t\)(\w+) \}", dispatch)}
sources = {}                      # stub -> set(source function VA)
func_re = re.compile(r"^void (\w+)\(void\)\n\{(.*?)^\}", re.M | re.S)
for path in glob.glob(os.path.join(gen_dir, "recomp_0*.c")):
    text = open(path).read()
    for m in func_re.finditer(text):
        fname, body = m.group(1), m.group(2)
        va = name_to_va.get(fname)
        if va is None and fname.endswith("_gen"):
            va = name_to_va.get(fname[:-4])
        if va is None:
            continue
        for s in re.findall(r"g_seh_ebp = ebp; sub_([0-9A-F]{8})\(\); return;", body):
            sources.setdefault(int(s, 16), set()).add(va)

starts = sorted(db)
def fn_end(s): return db[s].get("end", s)
def containing(v):
    import bisect
    i = bisect.bisect_right(starts, v) - 1
    return starts[i] if i >= 0 and v < fn_end(starts[i]) else None
def next_start(v):
    import bisect
    i = bisect.bisect_right(starts, v)
    return starts[i] if i < len(starts) else None

accepted, rejected = [], []
for v in stubs:
    for src in sorted(sources.get(v, ())):
        if src not in db:
            continue
        owner_of_v = containing(v)
        lo = min(src, owner_of_v if owner_of_v is not None else src)
        hi = max(fn_end(src), fn_end(owner_of_v) if owner_of_v is not None else (next_start(v) or v + 1))
        ok = False
        last = ""
        for _ in range(4):                       # widen to the next boundary a few times
            if lo not in db:
                last = "owner start not a function"; break
            interior = sorted(s for s in db if lo < s < hi)
            if not interior:
                last = "nothing to merge"; break
            try:
                tr.coalesce_function(lo, hi, interior)
                tr.discover_static_indirect_targets(coalescing=True)
                accepted.append({"start": f"0x{lo:08X}", "end": f"0x{hi:08X}",
                                 "coalesce_starts": [f"0x{s:08X}" for s in interior],
                                 "why": f"jump 0x{src:08X} -> 0x{v:08X}"})
                starts = sorted(db)
                ok = True
                break
            except ValueError as e:
                last = str(e)
                if "crosses end" in last or "does not cover" in last or "gap" in last:
                    n = next_start(hi - 1)
                    hi = fn_end(n) if n and n >= hi else hi + 0x10
                    continue
                break
        if not ok:
            rejected.append((v, src, last))

for e in accepted:
    print(f"ACCEPT {e['start']}-{e['end']} merging {len(e['coalesce_starts'])}  ({e['why']})")
print(f"\n{len(accepted)} accepted, {len(rejected)} rejected")
for v, src, why in rejected[:60]:
    print(f"  reject 0x{v:08X} from 0x{src:08X}: {why[:150]}")
json.dump([{k: e[k] for k in ("start", "end", "coalesce_starts")} for e in accepted],
          open(out_path, "w"), indent=1)
