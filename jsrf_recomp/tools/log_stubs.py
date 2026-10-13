#!/usr/bin/env python3
"""Make every unresolved-call stub report its first call.

The recompiler turns call targets it could not find a function for into stubs
that just pop the return address and return. A stub standing in for a real
function silently skips it, and its "return value" is whatever eax held,
which is how JSRF ended up with objects holding a pointer nobody allocated.

Run after tools.recomp, on the generated stub file:
    py -3 tools/log_stubs.py src/recomp/gen/recomp_stubs_unresolved.c

Each stub then calls recomp_stub_hit(stub_va, return_va, eax) once (defined in
src/recomp_manual.c). Idempotent: an already-patched file is left alone.
"""
import re
import sys

path = sys.argv[1]
src = open(path, encoding="utf-8").read()
if "recomp_stub_hit" in src:
    print("already patched")
    sys.exit(0)

pat = re.compile(r"^void sub_([0-9A-F]{8})\(void\) \{ (g_esp \+= \d+;) (/\*.*?\*/) \}$", re.M)
n = 0
def repl(m):
    global n
    n += 1
    va = m.group(1)
    return (f"void sub_{va}(void) {{ static int once; if (!once) {{ once = 1; "
            f"recomp_stub_hit(0x{va}u, MEM32(g_esp), g_eax); }} {m.group(2)} {m.group(3)} }}")
out = pat.sub(repl, src)
decl = "\nvoid recomp_stub_hit(uint32_t stub_va, uint32_t ret_va, uint32_t eax_val);\n"
out = out.replace('#include "recomp_funcs.h"\n', '#include "recomp_funcs.h"\n' + decl, 1)
open(path, "w", encoding="utf-8").write(out)
print(f"patched {n} stubs")
