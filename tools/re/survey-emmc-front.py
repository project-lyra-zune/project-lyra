#!/usr/bin/env python3
"""Survey the reserved eMMC front region: sample small reads across native
blocks [0, 441344) and report Shannon entropy + first bytes at each offset,
to map its structure (encrypted blob vs erased/zero fill vs headers/code).
Installs the remap hook once, reads all sample points, restores the hook.
"""
from __future__ import annotations

import argparse
import importlib.util
import math
import sys
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "ref", str(Path(__file__).resolve().parent / "read-emmc-front.py"))
ref = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ref)


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    hist = [0] * 256
    for b in data:
        hist[b] += 1
    n = len(data)
    e = 0.0
    for c in hist:
        if c:
            p = c / n
            e -= p * math.log2(p)
    return e


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    args = ap.parse_args()

    r = ref.ZuneREPL(args.host, timeout=30.0)
    nk = ref.find_nk(r)
    dev = ref.Dev(r)
    print(f"NK=0x{nk:08x}  nativeapp=0x{dev.pid:08x}")
    print(f"front region: native blocks [0, {ref.FRONT_BLOCKS}) = {ref.FRONT_BLOCKS*512/1024/1024:.1f} MiB\n")

    samples = [0, 8, 64, 256, 1024, 2048, 4096, 8192, 16384, 32768,
               65536, 131072, 200000, 262144, 350000, 400000, 430000, 441320]

    ref.install_hook(r, nk)
    fr = ref.FrontReader(dev, 2, ref.probe_readback_cap(dev, dev.alloc(0x4000)))
    print("hook installed\n")
    print(f"{'native blk':>10} {'byte off':>12}  {'entropy':>7}  {'nz/4096':>8}  first 16 bytes")
    print("-" * 78)
    try:
        for blk in samples:
            data = fr.read(blk)
            e = entropy(data)
            nz = sum(1 for b in data if b != 0)
            print(f"{blk:>10} {blk*512:>12}  {e:>7.3f}  {nz:>4}/{len(data):<4} {data[:16].hex()}")
    finally:
        fr.close()
        ref.uninstall_hook(r, nk)
        print("\nhook restored")


if __name__ == "__main__":
    main()
