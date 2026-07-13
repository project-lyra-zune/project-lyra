#!/usr/bin/env python3
"""Read sector 0 of the raw eMMC via:
  CreateFile("DSK1:") -> raw stream-device handle (bypasses mspart)
  DeviceIoControl(h, IOCTL=4 [legacy DISK_IOCTL_READ], SG_REQ)
libnvemmc.dll's DSK_IOControl accepts integer code 4 with classic 24-byte SG_REQ.
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
    "MapPtrToProcess" : COREDLL_BASE + 0x00ff00,  # marshal slot-0 VA -> globally-addressable
    "OpenStore"       : COREDLL_BASE + 0x02a880,
    "OpenPartition"   : COREDLL_BASE + 0x02aa8c,
}
PSEUDO_HCURPROC = 0xFFFFFFFF  # GetCurrentProcess() pseudo-handle
LPTR = 0x40

GENERIC_READ       = 0x80000000
FILE_SHARE_READ    = 0x00000001
FILE_SHARE_WRITE   = 0x00000002
OPEN_EXISTING      = 3

# Pavo v4.5 CE 6 R3 disk IOCTLs (RE'd from FSDMGR_ReadDisk's inner helper)
DISK_IOCTL_READ_CE6  = 0x00075C08  # CTL_CODE(7, 0x702, BUF, READ) the actual read
DISK_IOCTL_WRITE_CE6 = 0x00079C0C  # CTL_CODE(7, 0x703, BUF, WRITE)
DISK_IOCTL_GETINFO   = 0x00071C00  # CTL_CODE(7, 0x700, BUF, ANY) confirmed working

NUM_SECTORS = 2
SECTOR_SIZE = 2048  # from STOREINFO.dwBytesPerSector
TOTAL = NUM_SECTORS * SECTOR_SIZE

_ap = argparse.ArgumentParser(description="Read sector 0 of the raw eMMC (DSK1:) via the Lyra listener.")
_ap.add_argument("host", nargs="?", default="192.168.0.100",
                 help="Zune IP over wifi, or 127.0.0.1 for the USB tunnel (default: 192.168.0.100)")
_args = _ap.parse_args()

r = ZuneREPL(_args.host)
nativeapp_pid = next(p for p, n in r.list_processes() if n.lower() == "nativeapp.exe")
print(f"nativeapp pid = 0x{nativeapp_pid:08x}")


def user_call(fn_va, a0=0, a1=0, a2=0, a3=0, a4=0, a5=0, a6=0, a7=0):
    status, body = r._invoke(SUB_USER_CALL,
        struct.pack("<9I", fn_va, a0, a1, a2, a3, a4, a5, a6, a7))
    if status != 0:
        raise RuntimeError(f"SUB_USER_CALL status=0x{status:08x}")
    return struct.unpack("<II", body[:8])


def alloc(size):
    buf, err = user_call(VA["LocalAlloc"], LPTR, size)
    if buf == 0:
        raise RuntimeError(f"LocalAlloc({size}) err=0x{err:08x}")
    return buf


def free(buf):
    user_call(VA["LocalFree"], buf)


def hexdump(data, n=None):
    if n: data = data[:n]
    for off in range(0, len(data), 16):
        row = data[off:off+16]
        h = ' '.join(f'{b:02x}' for b in row)
        a = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
        print(f"  +0x{off:04x}  {h:<48}  {a}")


h_dev = data_buf = sg_buf = br_buf = name_buf = 0
try:
    # CreateFile("DSK1:")
    name_buf = alloc(32)
    r.write_proc_mem(nativeapp_pid, name_buf, "DSK1:\x00".encode("utf-16-le"))
    print('[1] CreateFile("DSK1:", GENERIC_READ, SHARE_READ|WRITE, NULL, OPEN_EXISTING, 0, NULL)')
    h_dev, err = user_call(VA["CreateFileW"],
        name_buf,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        0,
        OPEN_EXISTING,
        0,
        0,
    )
    print(f"    h_dev=0x{h_dev:08x}  err=0x{err:08x}")
    if not h_dev or h_dev == 0xFFFFFFFF:
        print(f"    [FAIL] CreateFile rejected. DSK1: not exposed as a raw device, or perms wrong")
        sys.exit(1)

    # Build SG_REQ: marshal the data buffer pointer through MapPtrToProcess
    # so it's globally-addressable when the kernel-side driver dereferences it.
    data_buf = alloc(TOTAL)
    r.write_proc_mem(nativeapp_pid, data_buf, b"\xDD" * TOTAL)
    mapped_buf, mperr = user_call(VA["MapPtrToProcess"], data_buf, PSEUDO_HCURPROC)
    print(f"[2a] MapPtrToProcess(0x{data_buf:08x}) -> 0x{mapped_buf:08x}  err=0x{mperr:08x}")
    if mapped_buf == 0:
        print("    [FAIL] MapPtrToProcess returned NULL. Bailing")
        sys.exit(2)
    # CE 6 R3 SG_REQ: 20-byte header + 8-byte SG_BUF per buffer
    sg_req = struct.pack("<7I",
        0,            # +0x00 sr_start
        NUM_SECTORS,  # +0x04 sr_num_sec
        1,            # +0x08 sr_num_sg
        0xDEADBEEF,   # +0x0C sr_status (driver overwrites)
        0,            # +0x10 reserved (zero)
        mapped_buf,   # +0x14 sr_sglist[0].sb_buf
        TOTAL,        # +0x18 sr_sglist[0].sb_len
    )
    sg_buf = alloc(len(sg_req))
    r.write_proc_mem(nativeapp_pid, sg_buf, sg_req)
    br_buf = alloc(4)
    r.write_proc_mem(nativeapp_pid, br_buf, b"\x00" * 4)
    print(f"[2] data_buf=0x{data_buf:08x}  sg_buf=0x{sg_buf:08x}  br_buf=0x{br_buf:08x}")
    print(f"    SG_REQ: {sg_req.hex()}")

    # DeviceIoControl(h_dev, DISK_IOCTL_READ_CE6=0x75C08, sg_buf, 28, NULL, 0, br_buf, NULL)
    print(f"[3] DeviceIoControl(IOCTL=0x{DISK_IOCTL_READ_CE6:08x} / DISK_IOCTL_READ_CE6)")
    ok, err = user_call(VA["DeviceIoControl"],
        h_dev,
        DISK_IOCTL_READ_CE6,
        sg_buf, len(sg_req),
        0, 0,
        br_buf,
        0,
    )
    br = struct.unpack("<I", r.read_proc_mem(nativeapp_pid, br_buf, 4))[0]
    sr_status = struct.unpack_from("<I", r.read_proc_mem(nativeapp_pid, sg_buf, 28), 12)[0]
    print(f"    ok={ok}  err=0x{err:08x}  BytesReturned={br}  sr_status=0x{sr_status:08x}")

    data = r.read_proc_mem(nativeapp_pid, data_buf, TOTAL)
    if data == b"\xDD" * len(data):
        print(f"    [FAIL] buffer untouched")
    else:
        print(f"\n=== sector 0 of DSK1: (raw eMMC start), first 256 bytes ===")
        hexdump(data, 256)
        nz = sum(1 for b in data if b != 0)
        print(f"\n    non-zero bytes: {nz} / {len(data)}")
        with open("/tmp/dsk1-sector0.bin", "wb") as f:
            f.write(data)
        print(f"    wrote /tmp/dsk1-sector0.bin ({len(data)} bytes)")
        # Check for MBR signature at offset 0x1FE (in standard 512-byte sector)
        if data[0x1FE:0x200] == b"\x55\xAA":
            print(f"    [!] MBR magic 0x55AA at offset 0x1FE. MS-Partition table present")

finally:
    if h_dev and h_dev != 0xFFFFFFFF:
        user_call(VA["CloseHandle"], h_dev)
    for b in (data_buf, sg_buf, br_buf, name_buf):
        if b: free(b)
