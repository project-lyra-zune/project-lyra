#!/usr/bin/env python3
"""Read the Tegra AP16 chip ID and fuse cache from the running device.

Uses the TegraMmio primitive (NvRmPhysicalMemMap + u32-atomic KMEMCPY) to map
and read the fuse controller aperture (0x7000F800), which no runtime driver
maps. Read-only: no fuse or register is written.

  read-tegra-fuses.py 127.0.0.1
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL, REPLError
from tegra_mmio import TegraMmio

GP_HIDREV = 0x70000804
FUSE_BASE = 0x7000F800

# The fuse rows NvBlFuseGet reads for the Secure Boot Key / Device Key on this
# build (from ZBoot's fuse-controller register references). Read-locked rows
# read back as 0xFFFFFFFF from the OS.
KEY_BAND = (0x7000F9A4, 0x7000F9B8)

CHIP = {0x15: "AP15", 0x16: "AP16 (APX 2600)", 0x20: "T20"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--dump", action="store_true", help="print every fuse-cache word")
    args = ap.parse_args()

    r = ZuneREPL(args.host, port=args.port, timeout=25.0)
    m = TegraMmio(r)

    hidrev = m.read32(GP_HIDREV)
    chipid = (hidrev >> 8) & 0xFF
    print(f"GP_HIDREV (0x{GP_HIDREV:08x}) = 0x{hidrev:08x}")
    print(f"  chip 0x{chipid:02x} = {CHIP.get(chipid, 'unknown')}, "
          f"major rev {(hidrev >> 4) & 0xF}, minor rev {(hidrev >> 16) & 0xFF}")

    print("\nfuse cache (non-zero words):")
    key_words = []
    for phys in range(FUSE_BASE, 0x7000FA00, 4):
        try:
            v = m.read32(phys)
        except REPLError as e:
            print(f"  0x{phys:08x}: FAULT {e}")
            break
        if KEY_BAND[0] <= phys < KEY_BAND[1]:
            key_words.append(v)
        if v and (args.dump or phys < 0x7000F930 or phys >= 0x7000F9A0):
            print(f"  0x{phys:08x} (FUSE+0x{phys - FUSE_BASE:03x}) = 0x{v:08x}")

    print("\nSBK/DK private-key rows (0x7000F9A4..0x7000F9B4):")
    print("  " + " ".join(f"{w:08x}" for w in key_words))
    if key_words and all(w == 0xFFFFFFFF for w in key_words):
        print("  read-locked (all 0xFFFFFFFF): secure boot on, SBK not software-readable")
    elif key_words and all(w == 0 for w in key_words):
        print("  all zero: no SBK burned")
    else:
        print("  NON-TRIVIAL: key rows are software-readable")
    return 0


if __name__ == "__main__":
    main()
