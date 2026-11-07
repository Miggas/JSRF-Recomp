"""Match the US (NTSC) default.xbe to another release of the game.

Every function of the US analysis is looked for in the other XBE by its code,
with the addresses inside it ignored (absolute operands, calls and jumps out of
the function, switch tables): a function that only moved matches exactly.
From the matched functions come the moves of every code and data address the
two have in common.

Used by make_version.py; needs both XBEs, so it is a developer tool -- what it
produces (analysis/<version>/) is addresses only.
"""
import bisect
import collections
import json
import struct

import capstone

_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
_md.detail = True


class Image:
    """An XBE's sections laid out at their addresses."""

    def __init__(self, path):
        d = open(path, "rb").read()
        if d[:4] != b"XBEH":
            raise ValueError("%s is not an XBE" % path)
        u = lambda o: struct.unpack_from("<I", d, o)[0]
        base = u(0x104)
        n, so = u(0x11C), u(0x120) - base
        self.sections = []
        for i in range(n):
            o = so + i * 0x38
            va, vs, ra, rs, nm = u(o + 4), u(o + 8), u(o + 12), u(o + 16), u(o + 20)
            name = d[nm - base:nm - base + 16].split(b"\0")[0].decode("latin-1")
            self.sections.append((name, va, vs, ra, rs))
        top = max(s[1] + s[2] for s in self.sections)
        img = bytearray(top + 16)
        for name, va, vs, ra, rs in self.sections:
            k = min(vs, rs)
            img[va:va + k] = d[ra:ra + k]
        self.mem = bytes(img)
        self.top = top

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        return None

    def section_of(self, va):
        for s in self.sections:
            if s[1] <= va < s[1] + s[2]:
                return s[0]
        return None

    def code_sections(self):
        return [s for s in self.sections if s[0] in (".text", "D3D", "DSOUND", "MMATRIX", "XGRPH", "XPP", "DOLBY")]


def signature(img, start, end, lo=0x10000, hi=None):
    """(pattern, mask, refs): the bytes of [start, end) with the addresses in
    them masked out; refs = [(offset, value, 'abs'|'rel')]."""
    hi = hi or img.top
    mem = img.mem
    pat = bytearray(mem[start:end])
    mask = bytearray(b"\xff" * len(pat))
    refs = []

    def hide(o, v, kind):
        for k in range(4):
            mask[o + k] = 0
        refs.append((o, v, kind))

    for ins in _md.disasm(mem[start:end], start):
        o = ins.address - start
        if ins.disp_size == 4 and ins.disp_offset:
            v = struct.unpack_from("<I", mem, ins.address + ins.disp_offset)[0]
            if lo <= v < hi:
                hide(o + ins.disp_offset, v, "abs")
        if ins.imm_size == 4 and ins.imm_offset:
            v = struct.unpack_from("<I", mem, ins.address + ins.imm_offset)[0]
            branch = ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL)
            if branch and ins.bytes[0] in (0xE8, 0xE9, 0x0F):
                tgt = ins.operands[0].imm if ins.operands else 0
                if not (start <= tgt < end):
                    hide(o + ins.imm_offset, tgt, "rel")
            elif lo <= v < hi:
                hide(o + ins.imm_offset, v, "abs")
    # switch tables kept inside the code: entries pointing into the function
    for p in range(0, len(pat) - 3):
        v = struct.unpack_from("<I", mem, start + p)[0]
        if start <= v < end and all(mask[p + k] for k in range(4)):
            hide(p, v, "abs")
    for i in range(len(pat)):
        if not mask[i]:
            pat[i] = 0
    return bytes(pat), bytes(mask), refs


def matches_at(img, pat, mask, pos):
    seg = img.mem[pos:pos + len(pat)]
    return len(seg) == len(pat) and bytes(a & m for a, m in zip(seg, mask)) == pat


def _anchor(mask):
    best, i = (0, 0), 0
    while i < len(mask):
        if mask[i]:
            j = i
            while j < len(mask) and mask[j]:
                j += 1
            if j - i > best[1] - best[0]:
                best = (i, j)
            i = j
        else:
            i += 1
    return best


def _find(img, sig, lo, hi, limit=2):
    pat, mask, _ = sig
    i, j = _anchor(mask)
    if j - i < 4:
        return None
    key, res = pat[i:j], []
    p = img.mem.find(key, max(lo, 0) + i, hi + i)
    while p >= 0 and len(res) < limit:
        if matches_at(img, pat, mask, p - i):
            res.append(p - i)
        p = img.mem.find(key, p + 1, hi + i)
    return res


class Matcher:
    def __init__(self, us_xbe, other_xbe, functions):
        """functions: [(start, end)] of the US analysis."""
        self.us, self.ot = Image(us_xbe), Image(other_xbe)
        self.funcs = sorted({a: b for a, b in functions if b > a}.items())
        self.ends = dict(self.funcs)
        self.sigs = {a: signature(self.us, a, b) for a, b in self.funcs}
        self.map = {}
        self._match()
        self._data_moves()

    def _range_for(self, us_va):
        """The other XBE's code section holding us_va's counterpart."""
        name = self.us.section_of(us_va)
        s = self.ot.section(name) if name else None
        return (s[1], s[1] + s[2]) if s else (0x11000, self.ot.top)

    def _match(self):
        found = {}
        # 1. longer functions found once anywhere in their section
        for a, b in self.funcs:
            pat, mask, _ = self.sigs[a]
            if sum(1 for m in mask if m) < 40:
                continue
            lo, hi = self._range_for(a)
            r = _find(self.ot, self.sigs[a], lo, hi)
            if r and len(r) == 1:
                found[a] = r[0]
        # keep the longest chain in address order (the linker keeps the order)
        ks = sorted(found)
        tails, tidx, prev = [], [], {}
        for k in ks:
            v = found[k]
            pos = bisect.bisect_left(tails, v)
            if pos == len(tails):
                tails.append(v)
                tidx.append(k)
            else:
                tails[pos], tidx[pos] = v, k
            prev[k] = tidx[pos - 1] if pos else None
        good = {}
        k = tidx[-1] if tidx else None
        while k is not None:
            good[k] = found[k]
            k = prev[k]
        # 2. the rest, between their matched neighbours
        anch = sorted(good)
        for a, b in self.funcs:
            if a in good:
                continue
            i = bisect.bisect_left(anch, a)
            lo = good[anch[i - 1]] if i else self._range_for(a)[0]
            hi = good[anch[i]] if i < len(anch) else self._range_for(a)[1]
            d0 = (good[anch[i - 1]] - anch[i - 1]) if i else 0
            pat, mask, _ = self.sigs[a]
            if lo <= a + d0 < hi and matches_at(self.ot, pat, mask, a + d0):
                good[a] = a + d0
                continue
            r = _find(self.ot, self.sigs[a], lo, hi)
            if r and len(r) == 1:
                good[a] = r[0]
        # 3. moved by the linker: found once anywhere in the section
        taken = set(good.values())
        for a, b in self.funcs:
            if a in good:
                continue
            pat, mask, _ = self.sigs[a]
            if sum(1 for m in mask if m) < 16:
                continue
            lo, hi = self._range_for(a)
            r = _find(self.ot, self.sigs[a], lo, hi)
            if r and len(r) == 1 and r[0] not in taken:
                good[a] = r[0]
                taken.add(r[0])
        self.map = good
        self.keys = sorted(good)
        self.verified = set(good)   # matched by their whole code (not by a vtable slot)

    def _data_moves(self):
        votes = collections.defaultdict(collections.Counter)
        for a, b in self.map.items():
            for o, v, kind in self.sigs[a][2]:
                ev = struct.unpack_from("<I", self.ot.mem, b + o)[0]
                if kind == "rel":
                    ev = (b + o + 4 + ev) & 0xFFFFFFFF
                votes[v][ev] += 1
        self.refs = {}
        for v, c in votes.items():
            (best, n), = c.most_common(1)
            if n >= 0.8 * sum(c.values()):
                self.refs[v] = best
        self.ref_keys = sorted(self.refs)
        self._vtable_pairs()

    def _vtable_pairs(self):
        """Functions changed between releases are still found through the
        vtables they sit in: same table, same slot."""
        rd = self.us.section(".rdata")
        if not rd:
            return
        self.slot_pairs = {}
        for v, ev in self.refs.items():
            if not (rd[1] <= v < rd[1] + rd[2]):
                continue
            k = 0
            while k < 64:
                p = struct.unpack_from("<I", self.us.mem, v + 4 * k)[0]
                q = struct.unpack_from("<I", self.ot.mem, ev + 4 * k)[0]
                if self.us.section_of(p) != ".text" or self.ot.section_of(q) != ".text":
                    break
                if p in self.ends and p not in self.map:
                    self.slot_pairs.setdefault(p, collections.Counter())[q] += 1
                k += 1
        for p, c in self.slot_pairs.items():
            (q, n), = c.most_common(1)
            if len(c) == 1:
                self.map[p] = q
        self.keys = sorted(self.map)

    # ── translating one address ──
    def code(self, va):
        """The other release's address of the code at US va, or None."""
        if va in self.map:
            return self.map[va]
        i = bisect.bisect_right(self.keys, va) - 1
        if i >= 0:
            s = self.keys[i]
            if va < self.ends.get(s, s):
                e = self.map[s] + (va - s)
                pat, mask, _ = signature(self.us, s, self.ends[s])
                if matches_at(self.ot, pat, mask, self.map[s]):
                    return e
        if va in self.refs and self.ot.section_of(self.refs[va]) == self.us.section_of(va):
            return self.refs[va]
        # a short masked window near the neighbours' move (whole instructions)
        end = va
        for ins in _md.disasm(self.us.mem[va:va + 0x40], va):
            if ins.address + ins.size > va + 0x40:
                break
            end = ins.address + ins.size
        if end - va < 8:
            return None
        sig = signature(self.us, va, end)
        d = (self.map[self.keys[i]] - self.keys[i]) if i >= 0 else 0
        pat, mask, _ = sig
        if any(mask[:8]) and matches_at(self.ot, pat, mask, va + d):
            return va + d
        hits = [c for c in range(va + d - 0x1000, va + d + 0x1000)
                if self.ot.mem[c] == pat[0] and matches_at(self.ot, pat, mask, c)]
        return hits[0] if len(hits) == 1 else None

    def data(self, va):
        """The other release's address of the data at US va, or None."""
        if va in self.refs:
            return self.refs[va]
        sec = self.us.section_of(va)
        i = bisect.bisect_right(self.ref_keys, va) - 1
        cands = []
        for j in (i, i + 1):
            if 0 <= j < len(self.ref_keys):
                r = self.ref_keys[j]
                if self.us.section_of(r) == sec and abs(r - va) < 0x400:
                    cands.append(self.refs[r] - r)
        if cands and all(c == cands[0] for c in cands):
            e = va + cands[0]
            if len(cands) == 2:
                return e
            # one neighbour: the bytes must agree (not in zero-filled data)
            u, o = self.us.mem[va:va + 16], self.ot.mem[e:e + 16]
            if u == o and any(u):
                return e
        return None

    def vtable(self, va, slots=4):
        """A table of code pointers (a vtable) in .rdata: found by its first
        slots, translated."""
        want = []
        for k in range(slots):
            p = struct.unpack_from("<I", self.us.mem, va + 4 * k)[0]
            t = self.code(p)
            if t is None:
                return None
            want.append(t)
        key = struct.pack("<%dI" % slots, *want)
        s = self.ot.section(self.us.section_of(va) or ".rdata")
        if not s:
            return None
        p = self.ot.mem.find(key, s[1], s[1] + s[2])
        if p < 0 or self.ot.mem.find(key, p + 1, s[1] + s[2]) >= 0:
            return None
        return p

    def any(self, va):
        sec = self.us.section_of(va)
        if sec in (".text", "D3D", "DSOUND", "MMATRIX", "XGRPH", "XPP", "DOLBY"):
            return self.code(va)
        r = self.data(va)
        if r is None and sec == ".rdata":
            r = self.vtable(va)
        return r
