#!/usr/bin/env python3
"""Offline disasm + xref for any v4.5 XIP module; no device required.

Modules and their runtime ImageBase / file mapping (parsed from PE headers):
    gemstone.exe    base 0x00010000   va_to_off = va - 0x00010e00
    zhud_serv.dll   base 0x419b0000   va_to_off = va - 0x419b0e00
    xuidll.dll      base <parsed>
    compositor.exe  base 0x00010000

Code is A32 ARM (the THUMB machine flag in the PE header is not honoured by
the loader for these images, validated by disassembling known functions).

Usage:
    offline-disasm-any.py <module> dis  <VA> [len]      disassemble
    offline-disasm-any.py <module> str  <fileoff>       read wide string
    offline-disasm-any.py <module> xref <VA>            find u32-LE pointers to VA
    offline-disasm-any.py <module> findstr <text>       locate ascii/utf16le string -> VA

<module> is a short tag: gem | zhud | xui | comp | zam | ztouch | accel | keypad | core
"""
from __future__ import annotations
import struct, sys
from pathlib import Path
import capstone

from xip_paths import extracted_dir

_EXTRACTED = extracted_dir()
XIP = _EXTRACTED / "v4.5-eimgfs-out/xip"
NKXIP = _EXTRACTED / "v4.5-eimgfs-nk-out/xip"
MODS = {
    "gem":    XIP / "gemstone.exe",
    "zhud":   XIP / "zhud_serv.dll",
    "xui":    XIP / "xuidll.dll",
    "comp":   XIP / "compositor.exe",
    "zam":    XIP / "zam_serv.dll",
    "zdk":    XIP / "zdksystem.dll",
    "zmedia": XIP / "zmedia_serv.dll",
    "ztouch": NKXIP / "ZTouch.dll",
    "accel":  NKXIP / "zaccelerometer.dll",
    "keypad": NKXIP / "zkeypad.dll",
    "core":   NKXIP / "coredll.dll",
    "nvsrc":  NKXIP / "libnvsource_filter.dll",
    "nvaac":  NKXIP / "libnvaudio_transform_filter.dll",
    "nvarend":NKXIP / "libnvaudio_renderer.dll",
    "nvcp":   NKXIP / "libnvmm_contentpipe.dll",
    "nvparser":NKXIP / "libnvmm_parser.dll",
    "zie":    XIP / "zie.exe",
    "ziehooks":XIP / "ziehooks.dll",
    "znet":   XIP / "znet_serv.dll",
    "athsrvc":XIP / "athsrvc.dll",
    "wzcsapi":XIP / "wzcsapi.dll",
    "pm":     NKXIP / "pm.dll",
    "zcpu":   NKXIP / "zcpu.dll",
    "ar6k":   NKXIP / "ar6k_ndis_sdio.dll",
    "wzcsvc": NKXIP / "wzcsvc.dll",
    "ndis":   NKXIP / "ndis.dll",
    "nvpower":NKXIP / "libnvpower.dll",
    "zbatt":  NKXIP / "libzbatt.dll",
    "zbattlib":NKXIP / "libzbatt.dll",
    "battdrvr":XIP / "battdrvr.dll",
    "nvrmimpl":NKXIP / "libnvrm_impl.dll",
    "audcodec":NKXIP / "k.libnvodm_audiocodec.dll",
    "audmix":  NKXIP / "libnvddk_audiomixer_impl.dll",
    "sdbus":   NKXIP / "sdbus.dll",
    "busenum": NKXIP / "busenum.dll",
    "nvsdio":  NKXIP / "libnvsdio.dll",
    "nvddkmisc":NKXIP / "libnvddk_misc.dll",
    "nvrm":    NKXIP / "libnvrm.dll",
    "gwes":    NKXIP / "gwes.dll",
    "device":  NKXIP / "device.dll",
    "kcoredll":NKXIP / "k.coredll.dll",
    "pmu":     NKXIP / "libnvodm_pmu.dll",
    "sdmem":   NKXIP / "sdmemory.dll",
    "odmquery":NKXIP / "libnvodm_query.dll",
    "nvodmmisc":NKXIP / "libnvodm_misc.dll",
    "nvodmkq": NKXIP / "libnvodm_kernel_query.dll",
}

def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]

class Mod:
    def __init__(self, path: Path):
        self.path = path
        self.b = path.read_bytes()
        b = self.b
        pe = u32(b, 0x3c)
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
        for name, va0, vsz, raw, rawsz in self.secs:
            if va0 <= rva < va0 + max(vsz, rawsz):
                return raw + (rva - va0)
        return None
    def off_to_va(self, off):
        for name, va0, vsz, raw, rawsz in self.secs:
            if raw <= off < raw + rawsz:
                return self.base + va0 + (off - raw)
        return None

def disasm(m: Mod, va, n):
    off = m.va_to_off(va)
    if off is None:
        print(f"VA 0x{va:x} not mapped"); return
    raw = m.b[off:off+n]
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM); md.detail = True
    print(f"--- 0x{va:08x} ({n} bytes) in {m.path.name} ---")
    for ins in md.disasm(raw, va):
        line = f"  0x{ins.address:08x}  {ins.bytes.hex():8}  {ins.mnemonic:6} {ins.op_str}"
        note = ""
        if ins.mnemonic.startswith("ldr") and "[pc" in ins.op_str:
            for op in ins.operands:
                if op.type == capstone.arm.ARM_OP_MEM and op.mem.base == capstone.arm.ARM_REG_PC:
                    t = ins.address + 8 + op.mem.disp
                    to = m.va_to_off(t)
                    if to is not None:
                        v = u32(m.b, to)
                        note = f"  ; *0x{t:x} = 0x{v:08x}"
                        vo = m.va_to_off(v)
                        if vo is not None:
                            # show wide string if it looks like one
                            s = read_wstr(m, vo)
                            if s: note += f'  -> L"{s}"'
        elif ins.mnemonic in ("bl", "b", "blx") and ins.operands:
            op = ins.operands[0]
            if op.type == capstone.arm.ARM_OP_IMM:
                note = f"  ; -> sub_0x{op.imm:x}"
        print(line + note)

def read_wstr(m: Mod, off, maxlen=80):
    out = []
    i = off
    while i + 1 < len(m.b) and len(out) < maxlen:
        c = u16(m.b, i)
        if c == 0: break
        if c < 0x20 or c > 0x7e: return None if not out else "".join(out)
        out.append(chr(c)); i += 2
    return "".join(out) if out else None

def xref(m: Mod, va):
    needle = struct.pack("<I", va)
    i = m.b.find(needle); hits = []
    while i != -1:
        hits.append(i); i = m.b.find(needle, i+1)
    print(f"pointers to 0x{va:08x} in {m.path.name}: {len(hits)}")
    for off in hits:
        rva_va = m.off_to_va(off)
        print(f"  fileoff 0x{off:06x}" + (f"  (VA 0x{rva_va:08x})" if rva_va else "  (header/unmapped)"))

def findstr(m: Mod, text):
    res = []
    for tag, enc in (("A", text.encode("ascii")), ("W", text.encode("utf-16le"))):
        i = m.b.find(enc)
        while i != -1:
            va = m.off_to_va(i)
            res.append((tag, i, va)); i = m.b.find(enc, i+1)
    for tag, off, va in res:
        print(f"  {tag} fileoff 0x{off:06x}  VA 0x{va:08x}" if va else f"  {tag} fileoff 0x{off:06x}  (unmapped)")

def main():
    tag = sys.argv[1]; cmd = sys.argv[2]
    m = Mod(MODS[tag])
    if cmd == "dis":
        va = int(sys.argv[3], 0); n = int(sys.argv[4], 0) if len(sys.argv) > 4 else 128
        disasm(m, va, n)
    elif cmd == "str":
        off = int(sys.argv[3], 0); print(read_wstr(m, off))
    elif cmd == "xref":
        xref(m, int(sys.argv[3], 0))
    elif cmd == "findstr":
        findstr(m, sys.argv[3])

if __name__ == "__main__":
    main()
