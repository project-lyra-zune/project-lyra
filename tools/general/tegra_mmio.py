"""Read Tegra SoC registers from the running device through the Lyra listener.

CE runs with the MMU on and does not statically map most peripheral apertures,
so a physical register address is not directly readable. This maps the physical
page with NvRmPhysicalMemMap (kernel forwarder) and copies registers out with
the u32-atomic kernel memcpy.

KMEMCPY_F faults on a 4-byte MMIO read of these controllers; 8-byte reads
succeed, so every register fetch reads an 8-byte aligned pair and selects the
wanted word.

Read-only. Nothing is written unless write32() is called explicitly.
"""
import struct

NVOSALLOC_THUNK  = 0xC088D2B0
NVRM_PHYSMAP_FWD = 0xC094ABF4
KMEMCPY_F        = 0x80072318


def _u32(b, o=0):
    return struct.unpack_from("<I", b, o)[0]


class TegraMmio:
    def __init__(self, repl):
        self.r = repl
        self.scr = self._alloc(0x40)
        self._pages = {}

    def _alloc(self, n):
        p, _ = self.r.kcall(NVOSALLOC_THUNK, n, 0, 0, 0)
        if not p:
            raise RuntimeError("NvOsAlloc failed")
        return p

    def _va(self, page_base):
        if page_base not in self._pages:
            out = self._alloc(0x40)
            self.r.kwrite(out, b"\x00\x00\x00\x00")
            rv, _ = self.r.kcall(NVRM_PHYSMAP_FWD, page_base, 0x1000, 3, out)
            if rv != 0:
                raise RuntimeError(f"NvRmPhysicalMemMap 0x{page_base:08x} rv=0x{rv:08x}")
            self._pages[page_base] = _u32(self.r.kread(out, 4))
        return self._pages[page_base]

    def read32(self, phys):
        page = phys & ~0xFFF
        off = phys - page
        self.r.kcall(KMEMCPY_F, self.scr, self._va(page) + (off & ~7), 8, 0, 0, 0)
        b = self.r.kread(self.scr, 8)
        return _u32(b, 4 if (off & 7) else 0)

    def read_block(self, phys, nbytes):
        """Read a word-aligned range as raw bytes. `phys` and `nbytes` must be
        4-byte aligned."""
        return b"".join(struct.pack("<I", self.read32(phys + o))
                        for o in range(0, nbytes, 4))

    def write32(self, phys, value):
        page = phys & ~0xFFF
        off = phys - page
        va = self._va(page) + (off & ~7)
        self.r.kcall(KMEMCPY_F, self.scr, va, 8, 0, 0, 0)
        lo, hi = struct.unpack("<II", self.r.kread(self.scr, 8))
        if off & 7:
            hi = value & 0xFFFFFFFF
        else:
            lo = value & 0xFFFFFFFF
        self.r.kwrite(self.scr, struct.pack("<II", lo, hi))
        self.r.kcall(KMEMCPY_F, va, self.scr, 8, 0, 0, 0)
