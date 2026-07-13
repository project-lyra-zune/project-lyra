#!/usr/bin/env python3
"""Read a range of raw eMMC logical sectors from DSK1: over the Lyra listener.

Reads [--start, --start+--count) logical sectors (2048 B each on Pavo v4.5)
via CreateFile("DSK1:") + DeviceIoControl(0x75C08, SG_REQ), in chunks, and
concatenates to an output file. Read-only.

  read-raw-emmc-range.py 192.168.0.100 --start 0 --count 64 --out /tmp/lo.bin
"""
import sys, struct, argparse
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL

SUB_USER_CALL = 13
COREDLL_BASE = 0x40320000
VA = {
    "LocalAlloc"      : COREDLL_BASE + 0x01a7b0,
    "LocalFree"       : COREDLL_BASE + 0x01a878,
    "CreateFileW"     : COREDLL_BASE + 0x016b64,
    "CloseHandle"     : COREDLL_BASE + 0x01226c,
    "DeviceIoControl" : COREDLL_BASE + 0x016aa0,
    "MapPtrToProcess" : COREDLL_BASE + 0x00ff00,
}
LPTR = 0x40
GENERIC_READ     = 0x80000000
FILE_SHARE_RW    = 0x00000003
OPEN_EXISTING    = 3
IOCTL_DISK_READ  = 0x00075C08          # CTL_CODE(7,0x702,BUF,READ) on Pavo v4.5
SECTOR_SIZE      = 2048                 # STOREINFO.dwBytesPerSector


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host", nargs="?", default="192.168.0.100",
                    help="Zune IP over wifi, or 127.0.0.1 for the USB tunnel")
    ap.add_argument("--start", type=lambda x: int(x, 0), default=0, help="start logical sector")
    ap.add_argument("--count", type=lambda x: int(x, 0), default=16, help="logical sector count")
    ap.add_argument("--chunk", type=lambda x: int(x, 0), default=8, help="sectors per read")
    ap.add_argument("--out", default="/tmp/emmc-range.bin")
    args = ap.parse_args()

    r = ZuneREPL(args.host, timeout=20.0)
    npid = next(p for p, n in r.list_processes() if n.lower() == "nativeapp.exe")
    print(f"nativeapp pid = 0x{npid:08x}  reading sectors [{args.start}, {args.start+args.count}) "
          f"= {args.count*SECTOR_SIZE} bytes")

    def uc(fn, *a):
        a = a + (0,) * (8 - len(a))
        st, body = r._invoke(SUB_USER_CALL, struct.pack("<9I", fn, *a))
        if st != 0:
            raise RuntimeError(f"SUB_USER_CALL status=0x{st:08x}")
        return struct.unpack("<II", body[:8])

    def alloc(sz):
        b, err = uc(VA["LocalAlloc"], LPTR, sz)
        if not b:
            raise RuntimeError(f"LocalAlloc({sz}) err=0x{err:08x}")
        return b

    name = alloc(32)
    r.write_proc_mem(npid, name, "DSK1:\x00".encode("utf-16-le"))
    h_dev, err = uc(VA["CreateFileW"], name, GENERIC_READ, FILE_SHARE_RW, 0, OPEN_EXISTING, 0, 0)
    if not h_dev or h_dev == 0xFFFFFFFF:
        raise RuntimeError(f"CreateFile(DSK1:) failed err=0x{err:08x}")

    chunk = args.chunk
    cap = chunk * SECTOR_SIZE
    data_buf = alloc(cap)
    sg_buf = alloc(28)
    br_buf = alloc(4)
    out = bytearray()
    try:
        mapped, _ = uc(VA["MapPtrToProcess"], data_buf, 0xFFFFFFFF)
        sec = args.start
        while sec < args.start + args.count:
            n = min(chunk, args.start + args.count - sec)
            r.write_proc_mem(npid, sg_buf,
                struct.pack("<7I", sec, n, 1, 0, 0, mapped, n * SECTOR_SIZE))
            ok, e = uc(VA["DeviceIoControl"], h_dev, IOCTL_DISK_READ, sg_buf, 28, 0, 0, br_buf, 0)
            # read_proc_mem responses cap at ~0x1FFC bytes; pull the sector buffer back in 4 KB pieces
            want = n * SECTOR_SIZE
            got = 0
            while got < want:
                piece = r.read_proc_mem(npid, data_buf + got, min(0x1000, want - got))
                if not piece:
                    break
                out += piece
                got += len(piece)
            sec += n
    finally:
        uc(VA["CloseHandle"], h_dev)
        for b in (data_buf, sg_buf, br_buf, name):
            uc(VA["LocalFree"], b)

    Path(args.out).write_bytes(out)
    nz = sum(1 for b in out if b != 0)
    print(f"wrote {args.out} ({len(out)} bytes, non-zero {nz})")


if __name__ == "__main__":
    main()
