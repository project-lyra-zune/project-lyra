#!/usr/bin/env python3
"""Read and hexdump a Tegra register range from the running device.

Maps the physical aperture through NvRmPhysicalMemMap and reads it with the
u32-atomic kernel memcpy (see general/tegra_mmio.py). Read-only.

Some apertures fault unless their module clock is ungated (for example the
UARTs and the FUSE controller when idle). CAR is at 0x60006000, APB_MISC at
0x70000000.

  read-tegra-mmio.py 127.0.0.1 --phys 0x70000000 --len 0x40
  read-tegra-mmio.py 127.0.0.1 --phys 0x7000f800 --len 0x40
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL, REPLError
from tegra_mmio import TegraMmio


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--phys", type=lambda x: int(x, 0), required=True,
                    help="physical base (4-byte aligned)")
    ap.add_argument("--len", type=lambda x: int(x, 0), default=0x40,
                    help="bytes to read (4-byte aligned)")
    args = ap.parse_args()

    r = ZuneREPL(args.host, port=args.port, timeout=25.0)
    m = TegraMmio(r)

    for row in range(0, args.len, 16):
        words = []
        for col in range(0, 16, 4):
            phys = args.phys + row + col
            if row + col >= args.len:
                break
            try:
                words.append(f"{m.read32(phys):08x}")
            except REPLError:
                words.append("FAULT---")
        print(f"  0x{args.phys + row:08x}: " + " ".join(words))
    return 0


if __name__ == "__main__":
    main()
