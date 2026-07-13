#!/usr/bin/env python3
"""Locate the NvDdkHsmmcBlockDev geometry struct in the SDMMC carveout by its field
signature and read the eMMC EXT_CSD SEC_COUNT it caches. The struct's base address is
reallocated per boot, so it is found by signature, not a fixed address.

Signature (relative to the struct base): [+0x38]=0, [+0x3c]=SEC_COUNT, [+0x40]=0x400.
SEC_COUNT is a JEDEC value in 512-byte units, so it fixes the native block size and the
raw user-area capacity independent of how CE partitions the device."""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lyra_debug_common import connect, read_kernel_u32

CE_LOGICAL_SECTORS = 15_716_608      # from the storage-manager store walk
CE_LOGICAL_BYTES_PER_SECTOR = 2048   # ditto
NATIVE_BLOCK = 512                   # JEDEC eMMC


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host", nargs="?", default="192.168.0.100",
                    help="Zune IP over wifi, or 127.0.0.1 for the USB tunnel")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0xd2e00000)
    ap.add_argument("--len", type=lambda x: int(x, 0), default=0x1000)
    args = ap.parse_args()

    sock, _ = connect(args.host, args.port, 5.0)
    n = args.len // 4
    words = [read_kernel_u32(sock, args.base + i * 4) for i in range(n)]

    hits = []
    for i in range(1, n - 1):
        if words[i - 1] == 0 and words[i + 1] == 0x400 and words[i] != 0:
            addr = args.base + i * 4
            hits.append((addr, words[i]))

    if not hits:
        print(f"no (0, X, 0x400) signature in {args.base:#x}..{args.base + args.len:#x}; widen --base/--len")
        return 1

    for addr, sec_count in hits:
        struct_base = addr - 0x3c
        raw_bytes = sec_count * NATIVE_BLOCK
        ce_native = CE_LOGICAL_SECTORS * (CE_LOGICAL_BYTES_PER_SECTOR // NATIVE_BLOCK)
        gap_native = sec_count - ce_native
        print(f"struct @ {struct_base:#010x}  SEC_COUNT @ {addr:#010x} = {sec_count:#010x} = {sec_count:,} native sectors")
        print(f"  raw user area = {raw_bytes:,} B = {raw_bytes / 1024**3:.3f} GiB")
        print(f"  CE exposes    = {ce_native:,} native ({CE_LOGICAL_SECTORS:,} x {CE_LOGICAL_BYTES_PER_SECTOR}) = "
              f"{ce_native * NATIVE_BLOCK / 1024**3:.3f} GiB")
        print(f"  gap           = {gap_native:,} native = {gap_native * NATIVE_BLOCK / 1024**2:.1f} MiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
