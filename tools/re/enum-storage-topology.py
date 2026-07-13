#!/usr/bin/env python3
"""Enumerate all stores + partitions on the Zune HD via the CE Storage
Manager API, called from nativeapp.exe user-mode context via the
SUB_USER_CALL plugin sub-opcode (13). Dumps the topology to JSON.

Storage stack (recap from recon-storage-stack.py):
  libnvemmc.dll (Tegra eMMC driver) -> exposes DSK_Read/IOControl
  zpartstream.dll                   -> Zune partition stream layer
  zbinfs.dll, zexfat.dll            -> Zune FS drivers (OS / media)
  mspart.dll                        -> MS-Partition (also available)
  filesys.dll + fsdmgr.dll          -> CE-standard FS stack

Strategy:
  1. LocalAlloc a STOREINFO/PARTINFO buffer in nativeapp's heap
  2. FindFirstStore + FindNextStore loop  (size=0xF0)
  3. For each store: OpenStore + FindFirstPartition + FindNextPartition
                     loop (size=0x128) though we use FindFirstPartition
                     against an FFS-search-handle returned by OpenStore.
  4. Output JSON
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL

SUB_USER_CALL = 13
COREDLL_BASE = 0x40320000

# Resolved from PE export table:
RVA = dict(
    LocalAlloc        = 0x01a7b0,
    LocalFree         = 0x01a878,
    FindFirstStoreW   = 0x02a980,
    FindNextStoreW    = 0x02a9ac,
    FindCloseStore    = 0x02a9cc,
    OpenStoreW        = 0x02a880,
    GetStoreInfo      = 0x02a9dc,
    FindFirstPartitionW = 0x02ab74,
    FindNextPartitionW  = 0x02ab90,
    FindClosePartition  = None,  # uses FindCloseStore
    GetPartitionInfo  = 0x02ab18,
    OpenPartition     = 0x02aa8c,
    CloseHandle       = 0x01226c,
    GetLastError      = 0x01011c,
)
VA = {k: COREDLL_BASE + v for k, v in RVA.items() if v is not None}

LPTR = 0x40
STOREINFO_SIZE = 0xF0   # from coredll disasm
PARTINFO_SIZE  = 0x128  # from coredll disasm


def utf16z(b: bytes) -> str:
    end = len(b)
    for i in range(0, end - 1, 2):
        if b[i] == 0 and b[i + 1] == 0:
            end = i
            break
    return b[:end].decode("utf-16-le", "replace")


class UserCaller:
    def __init__(self, repl: ZuneREPL):
        self.r = repl
        self.pid = next(p for p, n in repl.list_processes()
                         if n.lower() == "nativeapp.exe")

    def call(self, fn_va: int, a0=0, a1=0, a2=0, a3=0, a4=0, a5=0) -> tuple[int, int]:
        status, body = self.r._invoke(SUB_USER_CALL,
            struct.pack("<7I", fn_va, a0, a1, a2, a3, a4, a5))
        if status != 0:
            raise RuntimeError(f"SUB_USER_CALL status=0x{status:08x}")
        return struct.unpack("<II", body[:8])

    def alloc(self, size: int) -> int:
        buf, err = self.call(VA["LocalAlloc"], LPTR, size)
        if buf == 0:
            raise RuntimeError(f"LocalAlloc({size}) failed err=0x{err:08x}")
        return buf

    def free(self, buf: int):
        self.call(VA["LocalFree"], buf)

    def read_buf(self, buf: int, n: int) -> bytes:
        return self.r.read_proc_mem(self.pid, buf, n)

    def write_buf(self, buf: int, data: bytes):
        self.r.write_proc_mem(self.pid, buf, data)


def enum_partitions(uc: UserCaller, h_store: int) -> list[dict]:
    """FindFirstPartition + FindNextPartition loop. Returns partition list."""
    buf = uc.alloc(PARTINFO_SIZE)
    parts = []
    try:
        uc.write_buf(buf, struct.pack("<I", PARTINFO_SIZE) + b"\x00" * (PARTINFO_SIZE - 4))
        h_find, err = uc.call(VA["FindFirstPartitionW"], h_store, buf)
        if h_find in (0, 0xFFFFFFFF):
            return parts  # store may have no partitions
        raw = uc.read_buf(buf, PARTINFO_SIZE)
        parts.append(parse_partinfo(raw))
        while True:
            uc.write_buf(buf, struct.pack("<I", PARTINFO_SIZE) + b"\x00" * (PARTINFO_SIZE - 4))
            ok, err = uc.call(VA["FindNextPartitionW"], h_find, buf)
            if ok == 0:
                break
            raw = uc.read_buf(buf, PARTINFO_SIZE)
            parts.append(parse_partinfo(raw))
        uc.call(VA["FindCloseStore"], h_find)
    finally:
        uc.free(buf)
    return parts


def enum_stores(uc: UserCaller) -> list[dict]:
    """FindFirstStore + FindNextStore loop."""
    buf = uc.alloc(STOREINFO_SIZE)
    stores = []
    try:
        uc.write_buf(buf, struct.pack("<I", STOREINFO_SIZE) + b"\x00" * (STOREINFO_SIZE - 4))
        h, err = uc.call(VA["FindFirstStoreW"], buf)
        if h in (0, 0xFFFFFFFF):
            raise RuntimeError(f"FindFirstStoreW failed err=0x{err:08x}")

        raw = uc.read_buf(buf, STOREINFO_SIZE)
        stores.append(parse_storeinfo(raw))
        while True:
            uc.write_buf(buf, struct.pack("<I", STOREINFO_SIZE) + b"\x00" * (STOREINFO_SIZE - 4))
            ok, err = uc.call(VA["FindNextStoreW"], h, buf)
            if ok == 0:
                break
            raw = uc.read_buf(buf, STOREINFO_SIZE)
            stores.append(parse_storeinfo(raw))
        uc.call(VA["FindCloseStore"], h)
    finally:
        uc.free(buf)
    return stores


def parse_storeinfo(raw: bytes) -> dict:
    """CE 6 STOREINFO on Pavo (240 bytes, layout reverse-engineered):
      +0x000 DWORD     dwSize          (always 0xF0)
      +0x004 WCHAR[16] szDeviceName    e.g. 'DSK1:', 'RAMDisk'
      +0x024 ?
      +0x054 DWORD     dwDeviceClass   e.g. 1
      +0x058 DWORD     dwDeviceType    e.g. 2
      +0x05C DWORD     dwDeviceFlags   e.g. 0x50
      +0x060 WCHAR[32] szStoreName     e.g. 'eMMCFlashDisk', 'RAMDisk'
      +0x0A0 DWORD     dwBlockSize?    (varies)
      +0x0A4 DWORD     dwFreeBlocks?   (varies)
      +0x0A8 DWORD     ?
      +0x0AC DWORD     ?
      +0x0B0 SECTORNUM snNumSectors    (u64, eMMC total)
      +0x0B8 DWORD     dwBytesPerSector
      +0x0E4 DWORD     dwPartitionCount
      +0x0E8 DWORD     dwMountCount
    """
    out = {}
    out["dwSize"]            = struct.unpack_from("<I", raw, 0x000)[0]
    out["szDeviceName"]      = utf16z(raw[0x004:0x024])
    out["dwDeviceClass"]     = struct.unpack_from("<I", raw, 0x054)[0]
    out["dwDeviceType"]      = struct.unpack_from("<I", raw, 0x058)[0]
    out["dwDeviceFlags"]     = struct.unpack_from("<I", raw, 0x05C)[0]
    out["szStoreName"]       = utf16z(raw[0x060:0x0A0])
    out["snNumSectors"]      = struct.unpack_from("<Q", raw, 0x0B0)[0]
    out["dwBytesPerSector"]  = struct.unpack_from("<I", raw, 0x0B8)[0]
    out["dwPartitionCount"]  = struct.unpack_from("<I", raw, 0x0E4)[0]
    out["dwMountCount"]      = struct.unpack_from("<I", raw, 0x0E8)[0]
    bps = out["dwBytesPerSector"] or 512
    out["_total_bytes"]      = out["snNumSectors"] * bps
    out["_total_gib"]        = out["_total_bytes"] / (1024 ** 3)
    return out


def parse_partinfo(raw: bytes) -> dict:
    """CE 6 PARTINFO on Pavo (296 bytes):
      +0x000 DWORD     dwSize (= 0x128)
      +0x004 WCHAR[32] szPartitionName  'Part00' etc.
      +0x044 WCHAR[32] szFileSys        'zbinfs.dll' / 'zexfat.dll'
      +0x084 WCHAR[32] szVolumeName     'Flash' / 'Flash2' / 'ZBINFS'
      +0x108 SECTORNUM snNumSectors     (u64)
      +0x124 BYTE      bPartType        MS-partition type byte (0x07=exFAT/NTFS, 0x21=zbinfs)
    """
    out = {}
    out["dwSize"]          = struct.unpack_from("<I", raw, 0x000)[0]
    out["szPartitionName"] = utf16z(raw[0x004:0x044])
    out["szFileSys"]       = utf16z(raw[0x044:0x084])
    out["szVolumeName"]    = utf16z(raw[0x084:0x108])
    out["snNumSectors"]    = struct.unpack_from("<Q", raw, 0x108)[0]
    out["bPartType"]       = raw[0x124]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host", nargs="?", default="192.168.0.100",
                    help="Zune IP over wifi, or 127.0.0.1 for the USB tunnel (default: 192.168.0.100)")
    ap.add_argument("--out", default=None, help="write full JSON to file")
    args = ap.parse_args()

    repl = ZuneREPL(args.host)
    uc = UserCaller(repl)

    print(f"nativeapp pid: 0x{uc.pid:08x}")
    print(f"Enumerating stores ...\n")

    stores = enum_stores(uc)
    print(f"Found {len(stores)} store(s):\n")

    report = {"stores": []}
    for i, info in enumerate(stores):
        bps = info["dwBytesPerSector"] or 512
        print(f"Store [{i}]  {info['szDeviceName']!r}  ({info['szStoreName']!r})")
        print(f"  class=0x{info['dwDeviceClass']:08x}  type=0x{info['dwDeviceType']:08x}  flags=0x{info['dwDeviceFlags']:08x}")
        print(f"  total = {info['snNumSectors']:,} sectors × {bps} B = {info['_total_gib']:.3f} GiB")
        print(f"  dwPartitionCount = {info['dwPartitionCount']}, dwMountCount = {info['dwMountCount']}")

        name = info["szDeviceName"]
        if not name:
            report["stores"].append(info)
            continue

        name_w = (name + "\x00").encode("utf-16-le")
        nbuf = uc.alloc(len(name_w))
        try:
            uc.write_buf(nbuf, name_w)
            h_store, err = uc.call(VA["OpenStoreW"], nbuf)
            info["h_store"] = h_store
            info["openstore_err"] = err
            if h_store and h_store != 0xFFFFFFFF:
                parts = enum_partitions(uc, h_store)
                info["partitions"] = parts
                accounted = 0
                for j, p in enumerate(parts):
                    pbytes = p["snNumSectors"] * bps
                    accounted += pbytes
                    print(f"    Part [{j}]  name={p['szPartitionName']!r}  fs={p['szFileSys']!r}  vol={p['szVolumeName']!r}")
                    print(f"               sectors={p['snNumSectors']:,} = {pbytes / (1024**2):.1f} MiB  type=0x{p['bPartType']:02x}")
                slack = info["_total_bytes"] - accounted
                print(f"    --- accounted {accounted/(1024**3):.3f} GiB / total {info['_total_gib']:.3f} GiB --- slack {slack/(1024**2):.1f} MiB ({slack:,} bytes)")
                uc.call(VA["CloseHandle"], h_store)
        finally:
            uc.free(nbuf)

        report["stores"].append(info)
        print()

    if args.out:
        with open(args.out, "w") as f:
            json.dump(report, f, indent=2, default=str)
        print(f"\nWrote {args.out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
