#!/usr/bin/env python3
"""Find ARM `bl`/`bl<cond>` call sites that target a given VA in a v4.5 XIP module.

The sibling offline-disasm-any.py only finds u32-LE data pointers (xref), which
misses branch-encoded call sites. This scans .text for the bl encoding and, for
each hit, prints a small instruction window so the caller's argument setup
(r0/r1/...) is visible.

Usage:
    scan-bl-callers.py <module> <target_VA> [window_insns]
"""
from __future__ import annotations
import struct, sys
from pathlib import Path
import capstone

from xip_paths import extracted_dir

XIP = extracted_dir() / "v4.5-eimgfs-out/xip"
MODS = {
    "gem": XIP / "gemstone.exe",
    "zhud": XIP / "zhud_serv.dll",
    "xui": XIP / "xuidll.dll",
    "comp": XIP / "compositor.exe",
}

def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]

class Mod:
    def __init__(self, path):
        self.b = path.read_bytes(); self.path = path
        b = self.b; pe = u32(b, 0x3c)
        optsz = u16(b, pe + 0x14); opt = pe + 0x18
        self.base = u32(b, opt + 0x1c)
        nsec = u16(b, pe + 6); so = opt + optsz
        self.secs = []
        for i in range(nsec):
            o = so + i * 40
            name = b[o:o+8].rstrip(b"\0").decode("latin1")
            vsz = u32(b, o+8); va = u32(b, o+12); rawsz = u32(b, o+16); raw = u32(b, o+20)
            self.secs.append((name, va, vsz, raw, rawsz))
    def va_to_off(self, va):
        rva = va - self.base
        for _, va0, vsz, raw, rawsz in self.secs:
            if va0 <= rva < va0 + max(vsz, rawsz):
                return raw + (rva - va0)
        return None
    def text_spans(self):
        for name, va0, vsz, raw, rawsz in self.secs:
            if name.startswith(".text"):
                yield self.base + va0, raw, min(vsz, rawsz)

def sx24(x):
    return x - (1 << 24) if x & 0x800000 else x

def main():
    tag = sys.argv[1]; target = int(sys.argv[2], 0)
    win = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    m = Mod(MODS[tag])
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM); md.detail = True
    hits = []
    for va0, raw, size in m.text_spans():
        for i in range(0, size - 3, 4):
            w = u32(m.b, raw + i)
            if (w >> 24) & 0x0F != 0x0B:      # bits[27:24]==1011 -> bl
                continue
            if (w >> 28) == 0xF:               # 0xF cond = blx(imm), skip
                continue
            a = va0 + i
            tgt = a + 8 + (sx24(w & 0xFFFFFF) << 2)
            if tgt == target:
                hits.append(a)
    print(f"bl callers of 0x{target:08x} in {m.path.name}: {len(hits)}")
    for a in hits:
        start = a - win * 4
        off = m.va_to_off(start)
        raw = m.b[off:off + (win + 2) * 4]
        print(f"\n=== caller 0x{a:08x} ===")
        for ins in md.disasm(raw, start):
            mark = "  <== call" if ins.address == a else ""
            tnote = ""
            if ins.mnemonic in ("bl", "b") and ins.operands and ins.operands[0].type == capstone.arm.ARM_OP_IMM:
                tnote = f"  ; -> 0x{ins.operands[0].imm:x}"
            print(f"  0x{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}{tnote}{mark}")

if __name__ == "__main__":
    main()
