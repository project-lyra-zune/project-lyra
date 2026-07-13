#!/usr/bin/env python3
"""Read the reserved front region of the Zune HD eMMC user area.

The block driver maps CE's DSK1: store starting at native block 441,344:
a DSK1: logical-sector-0 read reaches the card as CMD18 with block address
0x0006BC00 (= 441,344), and 441,344 + CE's store size (62,866,432 native)
equals the card's EXT_CSD SEC_COUNT (63,307,776) exactly. So native blocks
0..441,343 (215.5 MiB) sit in front of the store and are never addressable
through DSK1:. That region holds the boot and recovery firmware: a replicated
signed header, a Tegra BCT, the encrypted bootloader, and Windows CE boot and
recovery images.

This reads that region by hooking NvDdkHsmmcSendCommand with a trampoline
(hsmmc_remap.asm, sibling in this directory) that, while armed,
subtracts a delta from a read command's block address so a normal DSK1: read
lands in the front region instead. The driver's own clocked DMA path does the
transfer. Read-only on the card; restores the hook on exit.

Throughput scales with nativeapp's REPL output cap (rpc_server.cpp op_40).
The tool probes the live cap and reads back in the largest pieces it allows.

  read-emmc-front.py 127.0.0.1 --block 0 --count 8 --dump 512
  read-emmc-front.py 127.0.0.1 --full --out /tmp/emmc-front.bin   # whole 215 MiB, resumable
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL

# NvDdkHsmmcSendCommand(r0=handle, r1=pCmd) prologue: push {r4,r5,lr}; mov r4,r1
PATCH_VA = 0xc08e47b4
ORIGINAL_BYTES = struct.pack("<2I", 0xe92d4030, 0xe1a04001)

# hsmmc_remap.bin: assembled on the Win7 VM (armasm), capstone-verified, PIC.
# Source of truth: hsmmc_remap.asm (sibling in this directory).
TRAMP = bytes.fromhex(
    "f0402de958709fe5006097e5000056e30f00000a004091e5120054e30c00001a"
    "045091e53c709fe5004097e5040055e10700003a30709fe5004097e5040055e1"
    "0300002a24709fe5004097e5045045e0045081e5f040bde830402de90140a0e1"
    "0cf09fe50054018004540180085401800c540180bc478ec0"
)

ARMED_SLOT = 0x80015400
LO_SLOT    = 0x80015404
HI_SLOT    = 0x80015408
DELTA_SLOT = 0x8001540c

STORE_BASE_BLK = 441_344          # native block where DSK1: logical sector 0 lands
FRONT_BLOCKS   = 441_344          # size of the reserved front region, in native blocks
NATIVE_PER_LOGICAL = 4            # 2048-byte logical sector = 4 native 512-byte blocks
SECTOR_SIZE    = 2048

NVOSALLOC_THUNK = 0xC088D2B0
NK_PROC_LIST    = 0x80BEE010

SUB_USER_CALL = 13
COREDLL_BASE  = 0x40320000
VA_LocalAlloc      = COREDLL_BASE + 0x01a7b0
VA_LocalFree       = COREDLL_BASE + 0x01a878
VA_CreateFileW     = COREDLL_BASE + 0x016b64
VA_CloseHandle     = COREDLL_BASE + 0x01226c
VA_DeviceIoControl = COREDLL_BASE + 0x016aa0
IOCTL_DISK_READ    = 0x00075C08
GENERIC_READ       = 0x80000000
FILE_SHARE_RW      = 0x00000003
OPEN_EXISTING      = 3
LPTR               = 0x40


def _u16z(b: bytes) -> str:
    end = len(b)
    for i in range(0, end - 1, 2):
        if b[i] == 0 and b[i + 1] == 0:
            end = i
            break
    return b[:end].decode("utf-16-le", "replace")


def find_nk(r: ZuneREPL) -> int:
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


def hexdump(data: bytes, base_off: int = 0, limit: int | None = None) -> None:
    if limit:
        data = data[:limit]
    for off in range(0, len(data), 16):
        row = data[off:off + 16]
        h = " ".join(f"{b:02x}" for b in row)
        a = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        print(f"  +0x{base_off + off:06x}  {h:<48}  {a}")


class Dev:
    def __init__(self, r: ZuneREPL):
        self.r = r
        self.pid = next(p for p, n in r.list_processes() if n.lower() == "nativeapp.exe")

    def uc(self, fn, *a):
        a = a + (0,) * (8 - len(a))
        st, body = self.r._invoke(SUB_USER_CALL, struct.pack("<9I", fn, *a))
        if st != 0:
            raise RuntimeError(f"SUB_USER_CALL status=0x{st:08x}")
        return struct.unpack("<II", body[:8])

    def alloc(self, sz):
        b, err = self.uc(VA_LocalAlloc, LPTR, sz)
        if not b:
            raise RuntimeError(f"LocalAlloc({sz}) err=0x{err:08x}")
        return b

    def free(self, b):
        if b:
            self.uc(VA_LocalFree, b)


def probe_readback_cap(dev: Dev, va: int) -> int:
    """Largest single read_proc_mem payload the live nativeapp returns."""
    got = len(dev.r.read_proc_mem(dev.pid, va, 0x40000))
    return got


def install_hook(r: ZuneREPL, nk: int) -> None:
    cur = r.kread(PATCH_VA, 8)
    if cur != ORIGINAL_BYTES:
        raise RuntimeError(f"SendCommand site not original ({cur.hex()}); a prior hook may be live")
    heap, _ = r.kcall(NVOSALLOC_THUNK, len(TRAMP) + 8, 0, 0, 0)
    if not heap:
        raise RuntimeError("NvOsAlloc for trampoline failed")
    r.kwrite(heap, TRAMP)
    pos = 0
    while pos < len(TRAMP):
        r.patch_target_code(nk, heap + pos, TRAMP[pos:pos + 64])
        pos += 64
    r.patch_target_code(nk, PATCH_VA, struct.pack("<II", 0xe51ff004, heap))


def uninstall_hook(r: ZuneREPL, nk: int) -> None:
    r.patch_target_code(nk, PATCH_VA, ORIGINAL_BYTES)


class FrontReader:
    """Owns the DSK1: handle + reusable buffers across many chunk reads."""

    def __init__(self, dev: Dev, count_logical: int, readback: int):
        self.dev = dev
        self.r = dev.r
        self.c = count_logical
        self.nbytes = count_logical * SECTOR_SIZE
        self.readback = readback
        self.name = dev.alloc(32)
        self.r.write_proc_mem(dev.pid, self.name, "DSK1:\x00".encode("utf-16-le"))
        h, err = dev.uc(VA_CreateFileW, self.name, GENERIC_READ, FILE_SHARE_RW, 0, OPEN_EXISTING, 0, 0)
        if not h or h == 0xFFFFFFFF:
            raise RuntimeError(f"CreateFile(DSK1:) failed err=0x{err:08x}")
        self.h = h
        self.data = dev.alloc(self.nbytes)
        self.sg = dev.alloc(28)
        self.br = dev.alloc(4)
        # LO/HI are constant for a fixed chunk size (always trigger logical [0, c)).
        self.r.kwrite(LO_SLOT, struct.pack("<I", STORE_BASE_BLK))
        self.r.kwrite(HI_SLOT, struct.pack("<I", STORE_BASE_BLK + self.c * NATIVE_PER_LOGICAL))
        self.r.write_proc_mem(dev.pid, self.sg,
                              struct.pack("<7I", 0, self.c, 1, 0, 0, self.data, self.nbytes))

    def read(self, block: int) -> bytes:
        """Read native blocks [block, block + c*4) from the front region."""
        self.r.kwrite(DELTA_SLOT, struct.pack("<I", (STORE_BASE_BLK - block) & 0xFFFFFFFF))
        self.r.kwrite(ARMED_SLOT, struct.pack("<I", 1))
        try:
            self.dev.uc(VA_DeviceIoControl, self.h, IOCTL_DISK_READ, self.sg, 28, 0, 0, self.br, 0)
        finally:
            self.r.kwrite(ARMED_SLOT, struct.pack("<I", 0))
        out = bytearray()
        while len(out) < self.nbytes:
            piece = self.r.read_proc_mem(self.dev.pid, self.data + len(out),
                                         min(self.readback, self.nbytes - len(out)))
            if not piece:
                break
            out += piece
        return bytes(out)

    def close(self):
        self.dev.uc(VA_CloseHandle, self.h)
        for b in (self.name, self.data, self.sg, self.br):
            self.dev.free(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("--block", type=lambda x: int(x, 0), default=0,
                    help="native (512-byte) start block within the front region [0, 441344)")
    ap.add_argument("--count", type=lambda x: int(x, 0), default=8,
                    help="logical sectors to read (each = 4 native blocks = 2048 B)")
    ap.add_argument("--full", action="store_true", help="dump the entire 215 MiB front region")
    ap.add_argument("--chunk", type=lambda x: int(x, 0), default=64,
                    help="logical sectors per hooked read (each = 2048 B)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--resume", action="store_true", help="continue an existing --out file")
    ap.add_argument("--dump", type=int, default=256, help="bytes to hexdump to stdout")
    args = ap.parse_args()

    r = ZuneREPL(args.host, timeout=60.0)
    nk = find_nk(r)
    dev = Dev(r)
    print(f"NK=0x{nk:08x}  nativeapp=0x{dev.pid:08x}")

    total_blocks = FRONT_BLOCKS if args.full else args.count * NATIVE_PER_LOGICAL
    start_block = 0 if args.full else args.block

    # Probe the live readback cap (256 KB after the nativeapp cap bump; 8188 before).
    probe = dev.alloc(0x40000)
    cap = probe_readback_cap(dev, probe)
    dev.free(probe)
    print(f"readback cap = {cap} bytes/call  ({'bumped' if cap > 8188 else 'default 8 KB'})")

    resume_at = 0
    out_f = None
    if args.out:
        p = Path(args.out)
        mode = "r+b" if (args.resume and p.exists()) else "wb"
        out_f = open(p, mode)
        if mode == "r+b":
            out_f.seek(0, 2)
            resume_at = out_f.tell()
            print(f"resuming at byte {resume_at} ({resume_at/1024/1024:.1f} MiB)")

    install_hook(r, nk)
    print("hook installed")
    fr = FrontReader(dev, args.chunk, cap)
    t0 = time.time()
    done_bytes = resume_at
    first_chunk = None
    try:
        blk = start_block + resume_at // 512
        end_block = start_block + total_blocks
        chunk_native = args.chunk * NATIVE_PER_LOGICAL
        i = 0
        while blk < end_block:
            n_native = min(chunk_native, end_block - blk)
            if n_native < chunk_native:
                # tail chunk shorter than the fixed buffer: fall back to a fresh reader
                fr.close()
                fr = FrontReader(dev, max(1, n_native // NATIVE_PER_LOGICAL), cap)
            data = fr.read(blk)
            if first_chunk is None:
                first_chunk = data
            if out_f:
                out_f.write(data)
            done_bytes += len(data)
            blk += n_native
            i += 1
            if args.full and i % 16 == 0:
                mb = done_bytes / 1024 / 1024
                rate = (done_bytes - resume_at) / max(1e-6, time.time() - t0) / 1024 / 1024
                eta = (FRONT_BLOCKS * 512 - done_bytes) / max(1e-6, rate * 1024 * 1024)
                print(f"  {mb:7.1f}/{FRONT_BLOCKS*512/1024/1024:.0f} MiB  {rate:5.2f} MiB/s  ETA {eta/60:4.1f} min",
                      flush=True)
    finally:
        fr.close()
        uninstall_hook(r, nk)
        if out_f:
            out_f.close()
        print("hook restored")

    if first_chunk is not None and args.dump:
        print(f"\n=== native block {start_block} (front region), first {args.dump} bytes ===")
        hexdump(first_chunk, start_block * 512, args.dump)
    if args.out:
        print(f"wrote {args.out} ({done_bytes} bytes)")


if __name__ == "__main__":
    main()
