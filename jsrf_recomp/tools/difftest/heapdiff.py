#!/usr/bin/env python3
"""Differential test: JSRF's XAPI heap, original x86 (Unicorn) vs lifted C (libjsrf.so).

usage: heapdiff.py default.xbe info.json libjsrf.so [seed] [ops]
"""
import ctypes, json, os, random, struct, sys
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_PROT_ALL, UcError
from unicorn.x86_const import *

XBE, INFO, LIB = sys.argv[1:4]
SEED = int(sys.argv[4]) if len(sys.argv) > 4 else 1
NOPS = int(sys.argv[5]) if len(sys.argv) > 5 else 3000

RTL_CREATE_HEAP, RTL_ALLOC, RTL_FREE, RTL_REALLOC = 0x1493B8, 0x1497DC, 0x149F5E, 0x14A12E
KBASE, RET_MAGIC = 0xFE000000, 0xCAFEF00D
SPAN = 0x02000000                      # guest RAM modelled: 32 MB
STACK_TOP = 0x00780000
HEAP_LO, HEAP_HI = 0x00F80000, 0x01100000

b = open(XBE, "rb").read()
info = json.load(open(INFO))
ords = [k["ordinal"] for k in info["kernel_imports"]]
kthunk = info["header"]["kernel_thunk"]

def build_image(mem_write):
    hdr = struct.unpack_from("<I", b, 0x108)[0]
    mem_write(0x10000, b[:hdr])
    for s in info["sections"]:
        data = b[s["raw"]:s["raw"] + s["rawsize"]]
        mem_write(s["vaddr"], data + b"\0" * max(0, s["vsize"] - len(data)))
    for i in range(len(ords)):                      # patch thunks to synthetic VAs
        mem_write(kthunk + 4 * i, struct.pack("<I", KBASE + 4 * i))

# ---------------- Unicorn (original x86) ----------------
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0, SPAN, UC_PROT_ALL)
uc.mem_map(KBASE, 0x1000, UC_PROT_ALL); uc.mem_write(KBASE, b"\xC3" * 0x1000)
uc.mem_map(RET_MAGIC & ~0xFFF, 0x1000, UC_PROT_ALL)
build_image(uc.mem_write)
# FS segment with base 0x4000 (fake TIB) via a GDT
GDT = 0x3000
def seg(base, limit, access, flags):
    return struct.pack("<HHBBBB", limit & 0xFFFF, base & 0xFFFF, (base >> 16) & 0xFF, access,
                       ((limit >> 16) & 0xF) | (flags << 4), (base >> 24) & 0xFF)
gdt = b"\0" * 8 + seg(0, 0xFFFFF, 0x9B, 0xC) + seg(0, 0xFFFFF, 0x93, 0xC) + seg(0x4000, 0xFFF, 0x93, 0x4) + seg(0, 0xFFFFF, 0x93, 0xC)
uc.mem_write(GDT, gdt)
uc.reg_write(UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
uc.reg_write(UC_X86_REG_DS, 2 << 3); uc.reg_write(UC_X86_REG_ES, 2 << 3)
uc.reg_write(UC_X86_REG_SS, 2 << 3)  # ring-0 data
uc.reg_write(UC_X86_REG_FS, 3 << 3)

u_reserve = [0x00F81000]
u_unknown = []
def u_kernel(uc_, addr, size, _):
    slot = (addr - KBASE) // 4
    ordn = ords[slot]
    esp = uc_.reg_read(UC_X86_REG_ESP)
    arg = lambda i: struct.unpack("<I", uc_.mem_read(esp + 4 + 4 * i, 4))[0]
    rd = lambda a: struct.unpack("<I", uc_.mem_read(a, 4))[0]
    wr = lambda a, v: uc_.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))
    n, eax = 0, 0
    if ordn in (291, 277, 294): n = 1
    elif ordn == 184:
        pbase, psize, typ = arg(0), arg(2), arg(3)
        base, sz = rd(pbase), (rd(psize) + 0xFFF) & ~0xFFF
        if base == 0 and (typ & 0x2000): base = u_reserve[0]; u_reserve[0] += sz
        wr(pbase, base & ~0xFFF); wr(psize, sz); n = 5
    elif ordn == 199: n = 3
    elif ordn == 8: n = -1          # cdecl
    else:
        u_unknown.append(ordn); eax = 0xC0000001
    ret = rd(esp)
    uc_.reg_write(UC_X86_REG_EAX, eax)
    uc_.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * max(n, 0))
    uc_.reg_write(UC_X86_REG_EIP, ret)
uc.hook_add(UC_HOOK_CODE, u_kernel, begin=KBASE, end=KBASE + 0xFFF)

def u_call(va, args):
    esp = STACK_TOP
    for a in reversed(args):
        esp -= 4; uc.mem_write(esp, struct.pack("<I", a & 0xFFFFFFFF))
    esp -= 4; uc.mem_write(esp, struct.pack("<I", RET_MAGIC))
    uc.reg_write(UC_X86_REG_ESP, esp); uc.reg_write(UC_X86_REG_EBP, 0)
    uc.emu_start(va, RET_MAGIC, count=50_000_000)
    return uc.reg_read(UC_X86_REG_EAX), uc.reg_read(UC_X86_REG_ESP)

# ---------------- lifted C ----------------
lib = ctypes.CDLL(LIB)
lib.rt_init.restype = ctypes.POINTER(ctypes.c_uint8)
lib.rt_call.restype = ctypes.c_uint32
lib.rt_call.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]
base_ptr = lib.rt_init(SPAN)
cmem = (ctypes.c_uint8 * SPAN).from_address(ctypes.addressof(base_ptr.contents))
for i, o in enumerate(ords): lib.rt_set_slot(i, o)
def c_write(a, data): ctypes.memmove(ctypes.addressof(cmem) + a, bytes(data), len(data))
def c_read(a, n): return ctypes.string_at(ctypes.addressof(cmem) + a, n)
def c_call(va, args):
    arr = (ctypes.c_uint32 * max(1, len(args)))(*[a & 0xFFFFFFFF for a in args])
    esp = ctypes.c_uint32()
    eax = lib.rt_call(va, STACK_TOP, len(args), arr, ctypes.byref(esp))
    return eax, esp.value
def c_flag(name): return ctypes.c_uint32.in_dll(lib, name).value

# ---------------- create heap on the original, clone state ----------------
params = 0x00700000
uc.mem_write(params, struct.pack("<I", 0x30) + b"\0" * 0x2C)
reserve, commit = struct.unpack_from("<II", b, 0x134)
heap, _ = u_call(RTL_CREATE_HEAP, [2, 0, reserve, commit, 0, params])
print(f"RtlCreateHeap (original) -> {heap:#x}  reserve={reserve:#x} commit={commit:#x}  unknown kernel={u_unknown}")
c_write(0, uc.mem_read(0, SPAN))
ctypes.c_uint32.in_dll(lib, "g_reserve_next").value = u_reserve[0]   # keep the reserve cursor in step

REGIONS = [(HEAP_LO, HEAP_HI), (0x001EB760, 0x0027E080)]   # heap + .data
def compare():
    for lo, hi in REGIONS:
        a, c = bytes(uc.mem_read(lo, hi - lo)), c_read(lo, hi - lo)
        if a != c:
            diffs = [lo + i for i in range(0, hi - lo, 4) if a[i:i+4] != c[i:i+4]]
            return diffs, a, c, lo
    return None

rnd = random.Random(SEED)
live = []
for op in range(NOPS):
    r = rnd.random()
    if live and r < 0.25:
        k = rnd.randrange(len(live)); p = live[k]
        sz = rnd.choice([rnd.randint(1, 64), rnd.randint(1, 0x400), rnd.randint(0x400, 0x4000)])
        flags = rnd.choice([0, 0, 8, 0x10])
        desc, va, args = f"realloc({p:#x}, {sz:#x}, flags={flags:#x})", RTL_REALLOC, [heap, flags, p, sz]
    elif live and (r < 0.6 or len(live) > 400):
        k = rnd.randrange(len(live)); p = live.pop(k)
        desc, va, args = f"free({p:#x})", RTL_FREE, [heap, 0, p]
    else:
        sz = rnd.choice([rnd.randint(1, 64), rnd.randint(1, 0x400), rnd.randint(0x400, 0x4000), 0x280, 0xC, 0x10C0])
        flags = rnd.choice([0, 0, 8])
        desc, va, args = f"alloc({sz:#x}, flags={flags})", RTL_ALLOC, [heap, flags, sz]
    ue, uesp = u_call(va, args)
    ce, cesp = c_call(va, args)
    bad = []
    if (ue != ce) and va in (RTL_ALLOC, RTL_REALLOC): bad.append(f"return x86={ue:#x} lifted={ce:#x}")
    if va == RTL_FREE and (ue & 0xFF) != (ce & 0xFF): bad.append(f"return x86={ue:#x} lifted={ce:#x}")
    if uesp != cesp: bad.append(f"esp x86={uesp:#x} lifted={cesp:#x}")
    for flag in ("g_icall_failed", "g_unimpl_va", "g_kernel_unknown"):
        if c_flag(flag): bad.append(f"{flag}={c_flag(flag):#x}")
    cmpres = compare()
    if cmpres:
        diffs, a, c, lo = cmpres
        bad.append(f"{len(diffs)} memory dwords differ, first: " + ", ".join(
            f"[{d:#x}] x86={struct.unpack_from('<I', a, d-lo)[0]:#010x} lifted={struct.unpack_from('<I', c, d-lo)[0]:#010x}" for d in diffs[:6]))
    if bad:
        print(f"DIVERGENCE at op {op}: {desc}")
        for x in bad: print("   ", x)
        sys.exit(1)
    if va == RTL_ALLOC and ue: live.append(ue)
    if os.environ.get("BADFREE_TEST") and va == RTL_FREE and op in (500, 900):
        # lifted side only: free the same pointer again, then an interior pointer of a live block
        e1, s1 = c_call(RTL_FREE, [heap, 0, args[2]])
        q = live[0] + 16 if live else args[2] + 16
        e2, s2 = c_call(RTL_FREE, [heap, 0, q])
        print(f"  injected: double free({args[2]:#x}) -> eax={e1:#x} esp={s1:#x};  interior free({q:#x}) -> eax={e2:#x} esp={s2:#x}  (expected esp {STACK_TOP:#x})")
        if s1 != STACK_TOP or s2 != STACK_TOP: print("  ESP MISMATCH"); sys.exit(2)
        cm = compare()
        print("  heap after injected bad frees:", "IDENTICAL to original" if not cm else f"DIFFERS ({len(cm[0])} dwords)")
        if cm: sys.exit(3)
    if va == RTL_REALLOC and ue: live[k] = ue
print(f"seed {SEED}: {NOPS} ops, no divergence. live blocks={len(live)}")
