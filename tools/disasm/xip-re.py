#!/usr/bin/env python3
"""Cross-module static RE for CE6 XIP ROM DLLs (Tegra APX 2600 / Pavo v4.5).

These ROM modules are execute-in-place: PE ImageBase == runtime VA, and
imports are pre-bound (no import-name table to read). To resolve call
targets we build a global VA->name map from every module's EXPORT table,
then disassemble Thumb and annotate bl/blx and pc-relative literals.

No device required. Pure file-offset analysis.
"""
from __future__ import annotations
import argparse, glob, struct, sys
from pathlib import Path
import capstone

from xip_paths import extracted_dir

_EXTRACTED = extracted_dir()
XIP = _EXTRACTED / "v4.5-eimgfs-nk-out/xip"
XIP2 = _EXTRACTED / "v4.5-eimgfs-out/xip"


class PE:
    def __init__(self, path: Path):
        self.path = path
        self.name = path.name
        self.d = path.read_bytes()
        d = self.d
        lf = struct.unpack_from('<I', d, 0x3c)[0]
        assert d[lf:lf+4] == b'PE\0\0', f"{path} not PE"
        coff = lf + 4
        nsec = struct.unpack_from('<H', d, coff+2)[0]
        opt = coff + 20
        self.base = struct.unpack_from('<I', d, opt+28)[0]
        self.ep = struct.unpack_from('<I', d, opt+16)[0]
        ddir = opt + 96
        self.exp_rva, self.exp_sz = struct.unpack_from('<II', d, ddir+0)
        sec0 = opt + struct.unpack_from('<H', d, coff+16)[0]
        self.sections = []
        for i in range(nsec):
            o = sec0 + i*40
            nm = d[o:o+8].rstrip(b'\0').decode('latin1')
            vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o+8)
            self.sections.append((nm, va, vsz, rptr, rsz))

    def rva_to_off(self, rva):
        for nm, va, vsz, rptr, rsz in self.sections:
            if va <= rva < va + max(vsz, rsz):
                return rptr + (rva - va)
        return None

    def va_to_off(self, va):
        return self.rva_to_off(va - self.base)

    def contains(self, va):
        return self.base <= va < self.base + 0x100000 and self.va_to_off(va) is not None

    def u32(self, va):
        o = self.va_to_off(va)
        if o is None or o+4 > len(self.d):
            return None
        return struct.unpack_from('<I', self.d, o)[0]

    def cstr(self, va, mx=128):
        o = self.va_to_off(va)
        if o is None:
            return None
        end = self.d.find(b'\0', o, o+mx)
        if end < 0:
            return None
        return self.d[o:end].decode('latin1', 'replace')

    def imports(self):
        """Yield (iat_slot_va, bound_target_va, 'DLL!name') for each import."""
        lf = struct.unpack_from('<I', self.d, 0x3c)[0]
        coff = lf+4
        opt = coff+20
        imp_rva = struct.unpack_from('<I', self.d, opt+96+8)[0]
        if not imp_rva:
            return
        o = self.rva_to_off(imp_rva)
        d = self.d
        while True:
            oft, ts, fw, nameRva, fthunk = struct.unpack_from('<IIIII', d, o)
            o += 20
            if oft == 0 and nameRva == 0 and fthunk == 0:
                break
            no = self.rva_to_off(nameRva)
            dll = d[no:d.find(b'\0', no)].decode('latin1')
            oo = self.rva_to_off(oft or fthunk)
            fo = self.rva_to_off(fthunk)
            j = 0
            while True:
                ote = struct.unpack_from('<I', d, oo+j*4)[0]
                bound = struct.unpack_from('<I', d, fo+j*4)[0]
                if ote == 0 and bound == 0:
                    break
                if ote & 0x80000000:
                    nm = f"{dll}!ord#{ote & 0xffff}"
                else:
                    so = self.rva_to_off(ote & 0x7fffffff)
                    if so is not None:
                        nm = f"{dll}!{d[so+2:d.find(chr(0).encode(), so+2)].decode('latin1')}"
                    else:
                        nm = f"{dll}!?"
                yield self.base+fthunk+j*4, bound, nm
                j += 1

    def exports(self):
        """Yield (va, name) for every named + ordinal export."""
        if not self.exp_rva:
            return
        o = self.rva_to_off(self.exp_rva)
        if o is None:
            return
        d = self.d
        nfunc, nname = struct.unpack_from('<II', d, o+20)
        af, an, ao = struct.unpack_from('<III', d, o+28)
        ofa = self.rva_to_off(af)
        ofn = self.rva_to_off(an)
        ofo = self.rva_to_off(ao)
        names = {}
        if ofn and ofo:
            for i in range(nname):
                nrva = struct.unpack_from('<I', d, ofn+i*4)[0]
                ordi = struct.unpack_from('<H', d, ofo+i*2)[0]
                ns = self.rva_to_off(nrva)
                if ns is None:
                    continue
                e = d.find(b'\0', ns, ns+256)
                names[ordi] = d[ns:e].decode('latin1', 'replace')
        if ofa:
            for i in range(nfunc):
                frva = struct.unpack_from('<I', d, ofa+i*4)[0]
                if not frva:
                    continue
                va = self.base + frva
                nm = names.get(i, f"ord_{i}")
                yield va, f"{self.name}!{nm}"


def load_all():
    mods = []
    seen = set()
    for base in (XIP, XIP2):
        for p in sorted(base.glob("*.dll")) + sorted(base.glob("*.exe")):
            if p.name in seen:
                continue
            seen.add(p.name)
            try:
                mods.append(PE(p))
            except Exception:
                pass
    if not mods:
        raise FileNotFoundError(
            f"no XIP modules under {XIP} or {XIP2}; set LYRA_XIP_DIR to the "
            "firmwares/extracted dir"
        )
    return mods


def build_symbols(mods):
    sym = {}
    for m in mods:
        for va, nm in m.exports():
            for k in (va, va & ~1, va | 1):
                sym.setdefault(k, nm)
    # Per-module IAT: slot VA and bound target VA both resolve to import name.
    for m in mods:
        slots = {}
        for slot_va, bound, nm in m.imports():
            resolved = (sym.get(bound) or sym.get(bound & ~1) or
                        sym.get(bound | 1) or nm)
            slots[slot_va] = resolved
            for k in (bound, bound & ~1, bound | 1):
                sym.setdefault(k, resolved)
        m.iat = slots
    return sym


def find_mod(mods, va):
    for m in mods:
        if m.contains(va):
            return m
    return None


def resolve_thunk(m, va, md):
    """If va is a tiny import thunk (ldr ip,[pc]; bx ip / ldr pc,[pc]),
    return the IAT slot VA it dereferences, else None."""
    import re
    off = m.va_to_off(va & ~1)
    if off is None:
        return None
    for ins in list(md.disasm(m.d[off:off+12], va & ~1))[:3]:
        if ins.mnemonic.startswith('ldr') and '[pc' in ins.op_str:
            mm = re.search(r'#(-?0x[0-9a-f]+|-?\d+)', ins.op_str.split('[pc')[1])
            if mm:
                lit = ins.address + 8 + int(mm.group(1), 0)
                return m.u32(lit)  # the IAT slot VA
    return None


def disasm(target_va, length, mods, sym):
    m = find_mod(mods, target_va)
    if not m:
        print(f"!! 0x{target_va:08x} not in any loaded module")
        return
    off = m.va_to_off(target_va & ~1)
    code = m.d[off:off+length]
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.detail = True
    iat = getattr(m, 'iat', {})
    print(f";; {m.name}  va=0x{target_va:08x}  off=0x{off:x}  len={length}")
    start = target_va & ~1
    pos = 0
    while pos < len(code):
        chunk = list(md.disasm(code[pos:], start+pos))
        if not chunk:
            if pos + 4 <= len(code):
                print(f"  0x{start+pos:08x}: .word    0x{struct.unpack_from('<I', code, pos)[0]:08x}")
                pos += 4
            else:
                print(f"  0x{start+pos:08x}: .hw      0x{struct.unpack_from('<H', code, pos)[0]:04x}")
                pos += 2
            continue
        import re
        for ins in chunk:
            ann = ""
            if ins.mnemonic.startswith(('bl', 'b', 'bx', 'blx', 'cbz', 'cbnz')):
                for op in ins.operands:
                    if op.type == capstone.arm.ARM_OP_IMM:
                        tv = op.imm
                        nm = sym.get(tv) or sym.get(tv & ~1) or sym.get(tv | 1)
                        # Resolve import thunks: tiny stub that loads an IAT slot.
                        if not nm:
                            w = resolve_thunk(m, tv, md)
                            if w:
                                nm = (iat.get(w) or sym.get(w) or sym.get(w & ~1)
                                      or sym.get(w | 1))
                        if nm:
                            ann = f"  -> {nm}"
                        else:
                            tm = find_mod(mods, tv)
                            if tm and tm is not m:
                                ann = f"  -> {tm.name}+0x{tv-tm.base:x}"
                            elif tm:
                                ann = f"  -> loc_{tv:08x}"
            if 'ldr' in ins.mnemonic and '[pc' in ins.op_str:
                mm = re.search(r'#(-?0x[0-9a-f]+|-?\d+)', ins.op_str.split('[pc')[1])
                if mm:
                    lit = ins.address + 8 + int(mm.group(1), 0)  # ARM pc+8
                    val = m.u32(lit)
                    if val is not None:
                        nm = (iat.get(val) or sym.get(val) or sym.get(val & ~1)
                              or iat.get(m.u32(val) or 0))
                        s = m.cstr(val)
                        extra = f" = 0x{val:08x}"
                        if nm:
                            extra += f" ({nm})"
                        elif s and s.isprintable() and len(s) > 2:
                            extra += f" \"{s}\""
                        ann = f"  ; [0x{lit:08x}]{extra}"
            print(f"  0x{ins.address:08x}: {ins.mnemonic:8s} {ins.op_str}{ann}")
        pos = (chunk[-1].address - start) + chunk[-1].size


def cmd_modules(mods, sym, args):
    for m in mods:
        if args.grep and args.grep.lower() not in m.name.lower():
            continue
        print(f"{m.name:36s} base=0x{m.base:08x} ep=0x{m.base+m.ep:08x} exp={'Y' if m.exp_rva else '-'}")


def cmd_exports(mods, sym, args):
    for m in mods:
        if args.grep.lower() in m.name.lower():
            for va, nm in sorted(m.exports()):
                print(f"0x{va:08x} {nm}")


def cmd_sym(mods, sym, args):
    needle = args.grep.lower()
    for va, nm in sorted(sym.items()):
        if needle in nm.lower():
            print(f"0x{va:08x} {nm}")


def _scan_text(m, md):
    for nm, va, vsz, rptr, rsz in m.sections:
        if nm != '.text':
            continue
        code = m.d[rptr:rptr+rsz]
        for ins in md.disasm(code, m.base+va):
            yield ins, m, code, rptr, va


def cmd_xref(mods, sym, args):
    """Find branch + ldr-literal references to a target VA across modules."""
    tgts = {int(x, 0) for x in args.va.split(',')}
    tgts |= {t & ~1 for t in list(tgts)} | {t | 1 for t in list(tgts)}
    mlist = [m for m in mods if args.grep.lower() in m.name.lower()] if args.grep else mods
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.detail = True
    import re
    for m in mlist:
        for ins, _, code, rptr, sva in _scan_text(m, md):
            hit = None
            if ins.mnemonic.startswith(('bl', 'b', 'blx', 'cbz', 'cbnz')):
                for op in ins.operands:
                    if op.type == capstone.arm.ARM_OP_IMM and op.imm in tgts:
                        hit = f"{ins.mnemonic} {ins.op_str}"
            if not hit and 'ldr' in ins.mnemonic and '[pc' in ins.op_str:
                mm = re.search(r'#(-?0x[0-9a-f]+|-?\d+)', ins.op_str.split('[pc')[1])
                if mm:
                    lit = ((ins.address + 4) & ~3) + int(mm.group(1), 0)
                    v = m.u32(lit)
                    if v in tgts:
                        hit = f"{ins.mnemonic} {ins.op_str}  ; literal->0x{v:08x}"
            if hit:
                print(f"{m.name}: 0x{ins.address:08x}: {hit}")


def cmd_calls(mods, sym, args):
    """Profile a module's external call targets (bl/blx to resolved symbols)."""
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.detail = True
    for m in mods:
        if args.grep.lower() not in m.name.lower():
            continue
        from collections import Counter
        c = Counter()
        sites = {}
        for ins, _, code, rptr, sva in _scan_text(m, md):
            if ins.mnemonic not in ('bl', 'blx'):
                continue
            for op in ins.operands:
                if op.type == capstone.arm.ARM_OP_IMM:
                    tv = op.imm
                    nm = sym.get(tv) or sym.get(tv & ~1) or sym.get(tv | 1)
                    if nm and not nm.startswith(m.name):
                        c[nm] += 1
                        sites.setdefault(nm, []).append(ins.address)
        for nm, n in c.most_common():
            ex = ' '.join(f"0x{a:08x}" for a in sites[nm][:8])
            print(f"  {n:3d}x {nm:48s} {ex}")


def _veneers(m):
    """Find all import veneers in m: VA -> (slot_va, import_name).
    Pattern: e59fc004 e59cc000 e12fff1c <slot_va>."""
    out = {}
    iat = getattr(m, 'iat', {})
    for nm, va, vsz, rptr, rsz in m.sections:
        if nm != '.text':
            continue
        d = m.d
        end = rptr + rsz - 16
        o = rptr
        while o < end:
            if (struct.unpack_from('<I', d, o)[0] == 0xe59fc004 and
                    struct.unpack_from('<I', d, o+4)[0] == 0xe59cc000 and
                    struct.unpack_from('<I', d, o+8)[0] == 0xe12fff1c):
                slot = struct.unpack_from('<I', d, o+12)[0]
                ven_va = m.base + va + (o - rptr)
                out[ven_va] = (slot, iat.get(slot, f"slot_0x{slot:08x}"))
                o += 16
            else:
                o += 4
    return out


def _raw_bl_targets(m):
    """Yield (site_va, target_va, is_bl) for every ARM B/BL in m's .text."""
    for nm, va, vsz, rptr, rsz in m.sections:
        if nm != '.text':
            continue
        d = m.d
        for i in range(0, rsz - 4, 4):
            w = struct.unpack_from('<I', d, rptr+i)[0]
            if (w & 0x0E000000) == 0x0A000000:
                imm = w & 0xFFFFFF
                if imm & 0x800000:
                    imm -= 0x1000000
                site = m.base + va + i
                yield site, site + 8 + (imm << 2), bool((w >> 24) & 1)


def cmd_impxref(mods, sym, args):
    """Find every call site to an import (by name substring) across all
    modules, resolving each module's local veneer first. Reliable raw-BL
    scan (no capstone linear desync)."""
    needle = args.name.lower()
    only = args.grep.lower() if args.grep else None
    for m in mods:
        if only and only not in m.name.lower():
            continue
        vens = {v: info for v, info in _veneers(m).items()
                if needle in info[1].lower()}
        if not vens:
            continue
        sites = {}
        for site, tgt, is_bl in _raw_bl_targets(m):
            if tgt in vens:
                sites.setdefault(vens[tgt][1], []).append((site, is_bl))
        for impnm, lst in sites.items():
            print(f"{m.name}: -> {impnm}  ({len(lst)} sites)")
            for site, is_bl in lst:
                print(f"    0x{site:08x} {'BL' if is_bl else 'B'}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('dis'); p.add_argument('va'); p.add_argument('length', nargs='?', default='256')
    p = sub.add_parser('modules'); p.add_argument('grep', nargs='?', default='')
    p = sub.add_parser('exports'); p.add_argument('grep')
    p = sub.add_parser('sym'); p.add_argument('grep')
    p = sub.add_parser('xref'); p.add_argument('va'); p.add_argument('grep', nargs='?', default='')
    p = sub.add_parser('calls'); p.add_argument('grep')
    p = sub.add_parser('impxref'); p.add_argument('name'); p.add_argument('grep', nargs='?', default='')
    args = ap.parse_args()
    mods = load_all()
    sym = build_symbols(mods)
    if args.cmd == 'dis':
        disasm(int(args.va, 0), int(str(args.length), 0), mods, sym)
    elif args.cmd == 'modules':
        cmd_modules(mods, sym, args)
    elif args.cmd == 'exports':
        cmd_exports(mods, sym, args)
    elif args.cmd == 'sym':
        cmd_sym(mods, sym, args)
    elif args.cmd == 'xref':
        cmd_xref(mods, sym, args)
    elif args.cmd == 'calls':
        cmd_calls(mods, sym, args)
    elif args.cmd == 'impxref':
        cmd_impxref(mods, sym, args)


if __name__ == '__main__':
    main()
