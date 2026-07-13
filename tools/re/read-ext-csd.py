#!/usr/bin/env python3
"""Read the eMMC EXT_CSD register (Pavo v4.5), device-verified.

CMD8 SEND_EXT_CSD cannot be injected at the controller directly: the register map is
non-standard (index at base+0x28, arg at +0x2c, command at +0x10, not the SDHCI
+0x08/+0x0c) and the SD clock is gated at idle, ungated only through NvRm. So this drives
the driver's own read path instead: ungate the SDMMC clock via NvRmPowerModuleClockControl,
then NvDdkHsmmcRead with a hand-built CMD8 pCmd. That DMAs the 512-byte EXT_CSD into the
buffer, then returns NvError_NotImplemented from its block-read wrapper (CMD8 is not a block
command); the data lands before that, and a SEC_COUNT gate confirms it.

Addresses are firmware-specific to libnvddk_misc.dll on Pavo v4.5 (module base 0xc08e0000).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL

NK_PROC_LIST     = 0x80BEE010
CARVEOUT_WIN     = (0xd2e00800, 0x400)     # window that holds the geometry struct
CLKCTRL_THUNK    = 0xc08fab04              # NvRmPowerModuleClockControl import thunk
NVDDK_HSMMC_READ = 0xc08e51e4
SDMMC_MODULE_ID_CLASS = 0x11               # NvRmModuleID class, OR'd with (instance<<16)
SEC_COUNT_EXPECTED = 0x03c60000            # from the cached geometry; also the sanity gate

EXT_CSD_FIELDS = [  # (offset, name, unit_note)
    (192, "EXT_CSD_REV", "2 = revision 1.2 = eMMC 4.2 (predates boot/RPMB/GP partitions)"),
    (194, "CSD_STRUCTURE", ""),
    (196, "DEVICE_TYPE", "bitmask"),
    (185, "HS_TIMING", ""),
    (183, "BUS_WIDTH", ""),
    (226, "BOOT_SIZE_MULT", "x128 KiB per boot partition; 0 = unprovisioned"),
    (168, "RPMB_SIZE_MULT", "x128 KiB; 0 = unprovisioned"),
    (160, "PARTITIONING_SUPPORT", "bitmask; 0 = no v4.4 partitioning"),
    (155, "PARTITION_SETTING_COMPLETED", ""),
    (179, "PARTITION_CONFIG", ""),
    (177, "BOOT_BUS_CONDITIONS", ""),
]


def _u16z(b):
    end = len(b)
    for i in range(0, end - 1, 2):
        if b[i] == 0 and b[i + 1] == 0:
            end = i
            break
    return b[:end].decode("utf-16-le", "replace")


def find_nk(r):
    cur = struct.unpack("<I", r.kread(NK_PROC_LIST, 4))[0]
    seen = set()
    while cur and cur not in seen:
        seen.add(cur)
        blk = r.kread(cur, 0x40)
        nxt = struct.unpack_from("<I", blk, 0)[0]
        np = struct.unpack_from("<I", blk, 0x20)[0]
        if np and "nk" in _u16z(r.kread(np, 64)).lower():
            return cur
        cur = nxt
    raise RuntimeError("NK not found")


def find_geometry_struct(r):
    base, length = CARVEOUT_WIN
    win = r.kread(base, length)
    w = [struct.unpack_from("<I", win, i)[0] for i in range(0, len(win), 4)]
    for i in range(1, len(w) - 1):
        if w[i - 1] == 0 and w[i] == SEC_COUNT_EXPECTED and w[i + 1] == 0x400:
            return base + i * 4 - 0x3c
    raise RuntimeError("geometry struct signature not found; widen CARVEOUT_WIN")


def read_ext_csd(r):
    find_nk(r)  # validate NK reachable
    geom = find_geometry_struct(r)
    handle = struct.unpack("<I", r.kread(geom + 0x58, 4))[0]
    h = r.kread(handle, 0x40)
    hRm = struct.unpack_from("<I", h, 0)[0]
    inst = struct.unpack_from("<I", h, 4)[0]
    client = struct.unpack_from("<I", h, 0x30)[0]
    module_id = ((inst << 16) | SDMMC_MODULE_ID_CLASS) & 0xFFFFFFFF

    NVOS = 0xC088D2B0
    data_buf, _ = r.kcall(NVOS, 4096, 0, 0, 0)
    pcmd, _ = r.kcall(NVOS, 64, 0, 0, 0)

    ext = None
    for _ in range(8):
        r.kwrite(data_buf, b"\xEE" * 512)
        r.kwrite(pcmd, struct.pack("<8I", 8, 0, 1, 0, 0, 0, 0, 0))
        r.kcall(CLKCTRL_THUNK, hRm, module_id, client, 1)
        r.kcall(NVDDK_HSMMC_READ, handle, 512, data_buf, pcmd)
        r.kcall(CLKCTRL_THUNK, hRm, module_id, client, 0)
        cand = r.kread(data_buf, 512)
        if struct.unpack_from("<I", cand, 212)[0] == SEC_COUNT_EXPECTED:
            ext = cand
            break
    if ext is None:
        raise RuntimeError("could not obtain a sane EXT_CSD (SEC_COUNT gate failed)")
    return ext, dict(handle=handle, hRm=hRm, instance=inst, client=client, module_id=module_id)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("--out", default=None, help="write the raw 512-byte EXT_CSD")
    args = ap.parse_args()

    r = ZuneREPL(args.host, timeout=30.0)
    ext, meta = read_ext_csd(r)
    print(f"handle=0x{meta['handle']:08x} hRm=0x{meta['hRm']:08x} "
          f"moduleId=0x{meta['module_id']:08x} clientId=0x{meta['client']:08x}")
    sec = struct.unpack_from("<I", ext, 212)[0]
    print(f"SEC_COUNT[212] = 0x{sec:08x} = {sec:,} native sectors\n")
    for off, name, note in EXT_CSD_FIELDS:
        val = ext[off]
        print(f"  [{off:3}] {name:28} = {val}" + (f"   ({note})" if note else ""))
    if args.out:
        Path(args.out).write_bytes(ext)
        print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
