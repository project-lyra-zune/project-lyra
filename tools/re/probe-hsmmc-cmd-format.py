#!/usr/bin/env python3
"""Hook NvDdkHsmmcSendCommand and capture full 32-byte pCmd contents
on each call. Trigger reads of different sectors / sizes; diff the
captures to identify the command-index and argument fields in the
Hsmmc command structure.

64-byte trampoline (exactly at patch_target_code limit):
   0  push {r4-r11, lr}           save call site
   4  ldr  r4, [pc, #36]    -> handle_slot @48
   8  str  r0, [r4]
  12  ldr  r4, [pc, #32]    -> pcmd_slot @52
  16  str  r1, [r4]
  20  ldr  r4, [pc, #28]    -> capture_buf @56
  24  ldmia r1, {r5-r12}    load 8 words from pCmd
  28  stmia r4, {r5-r12}    store 8 words to capture buf
  32  pop  {r4-r11, lr}
  36  push {r4, r5, lr}      original instr 1
  40  mov  r4, r1            original instr 2
  44  ldr  pc, [pc, #8]     -> return_va @60
  48  handle_slot
  52  pcmd_slot
  56  capture_buf  (32 bytes, stored at a slot not inline)
  60  return_va
"""
from __future__ import annotations

import argparse
import struct
import sys

from zune_repl import ZuneREPL

PATCH_VA = 0xc08e47b4
ORIGINAL_BYTES = struct.pack("<2I", 0xe92d4030, 0xe1a04001)
PATCH_LEN = 8
RETURN_VA = PATCH_VA + PATCH_LEN

# Kernel scratch slots
HANDLE_SLOT  = 0x80015400
PCMD_SLOT    = 0x80015404
CAPTURE_BUF  = 0x80015408   # 32 bytes capacity (pCmd first 32 bytes)

NVOSALLOC_THUNK = 0xC088D2B0
NK_PROC_LIST = 0x80BEE010

SUB_USER_CALL = 13
COREDLL_BASE = 0x40320000
VA_LocalAlloc      = COREDLL_BASE + 0x01a7b0
VA_CreateFileW     = COREDLL_BASE + 0x016b64
VA_CloseHandle     = COREDLL_BASE + 0x01226c
VA_DeviceIoControl = COREDLL_BASE + 0x016aa0
IOCTL_DISK_READ = 0x00075C08


def _u16z(b):
    end = len(b)
    for i in range(0, end - 1, 2):
        if b[i] == 0 and b[i+1] == 0:
            end = i; break
    return b[:end].decode("utf-16-le", "replace")


def find_nk(r):
    cur = struct.unpack("<I", r.kread(NK_PROC_LIST, 4))[0]
    seen = set()
    while cur and cur not in seen:
        seen.add(cur)
        blk = r.kread(cur, 0x40)
        nxt = struct.unpack_from("<I", blk, 0)[0]
        np  = struct.unpack_from("<I", blk, 0x20)[0]
        if np:
            n = _u16z(r.kread(np, 64))
            if "nk" in n.lower(): return cur
        cur = nxt
    raise RuntimeError("NK not found")


def trampoline(return_va, handle_slot, pcmd_slot, capture_buf):
    insns = [
        0xe92d4ff0,  # push {r4-r11, lr}
        0xe59f4024,  # ldr r4, [pc, #36]  -> handle_slot
        0xe5840000,  # str r0, [r4]
        0xe59f4020,  # ldr r4, [pc, #32]  -> pcmd_slot
        0xe5841000,  # str r1, [r4]
        0xe59f401c,  # ldr r4, [pc, #28]  -> capture_buf
        0xe8911fe0,  # ldmia r1, {r5-r12}
        0xe8841fe0,  # stmia r4, {r5-r12}
        0xe8bd4ff0,  # pop {r4-r11, lr}
        0xe92d4030,  # push {r4, r5, lr}  (original 1)
        0xe1a04001,  # mov r4, r1         (original 2)
        0xe59ff008,  # ldr pc, [pc, #8]   -> return_va
    ]
    code = struct.pack("<12I", *insns)
    pool = struct.pack("<4I", handle_slot, pcmd_slot, capture_buf, return_va)
    assert len(code) == 48 and len(pool) == 16
    return code + pool


def trigger_read(r, nativeapp_pid, start_sector, num_sectors=1):
    """Issue a sector-N read on DSK1: to fire the hook."""
    def uc(fn, *args):
        while len(args) < 8: args = args + (0,)
        st, body = r._invoke(SUB_USER_CALL, struct.pack("<9I", fn, *args))
        return struct.unpack("<II", body[:8]) if st == 0 else (0, st)
    def alloc(sz):
        b, _ = uc(VA_LocalAlloc, 0x40, sz); return b

    name = alloc(32); r.write_proc_mem(nativeapp_pid, name, "DSK1:\x00".encode("utf-16-le"))
    h_dev, _ = uc(VA_CreateFileW, name, 0x80000000, 3, 0, 3, 0, 0)
    SEC = 2048
    data_buf = alloc(num_sectors * SEC)
    r.write_proc_mem(nativeapp_pid, data_buf, b"\xDD" * (num_sectors * SEC))
    sg = alloc(28)
    sg_req = struct.pack("<7I", start_sector, num_sectors, 1, 0, 0, data_buf, num_sectors * SEC)
    r.write_proc_mem(nativeapp_pid, sg, sg_req)
    br = alloc(4)
    uc(VA_DeviceIoControl, h_dev, IOCTL_DISK_READ, sg, 28, 0, 0, br, 0)
    uc(VA_CloseHandle, h_dev)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    r = ZuneREPL(args.host, timeout=30.0)
    nk = find_nk(r)
    print(f"NK proc: 0x{nk:08x}")

    current = r.kread(PATCH_VA, PATCH_LEN)
    if current != ORIGINAL_BYTES:
        print(f"Patch site not original ({current.hex()}); bailing")
        return 1

    heap, _ = r.kcall(NVOSALLOC_THUNK, 80, 0, 0, 0)
    print(f"Trampoline heap: 0x{heap:08x}")
    tramp = trampoline(RETURN_VA, HANDLE_SLOT, PCMD_SLOT, CAPTURE_BUF)
    assert len(tramp) == 64, f"trampoline {len(tramp)} bytes"
    r.kwrite(heap, tramp)
    r.patch_target_code(nk, heap, tramp)

    patch = struct.pack("<II", 0xe51ff004, heap)
    r.patch_target_code(nk, PATCH_VA, patch)
    print("hook installed")

    nativeapp_pid = next(p for p, n in r.list_processes() if n.lower() == "nativeapp.exe")

    samples = []
    triggers = [
        ("sector 0, 1 block",  0,   1),
        ("sector 0, 2 blocks", 0,   2),
        ("sector 100, 1 block", 100, 1),
        ("sector 1000, 1 block", 1000, 1),
        ("sector 1000, 4 blocks", 1000, 4),
    ]

    try:
        for label, start, n in triggers:
            r.kwrite(CAPTURE_BUF, b"\x00" * 32)
            r.kwrite(HANDLE_SLOT, b"\x00" * 8)
            print(f"\n--- {label} ---")
            try:
                trigger_read(r, nativeapp_pid, start, n)
            except Exception as e:
                print(f"  trigger failed: {e}")
                continue
            handle, pcmd_ptr = struct.unpack("<2I", r.kread(HANDLE_SLOT, 8))
            cap = r.kread(CAPTURE_BUF, 32)
            print(f"  handle=0x{handle:08x}  pcmd=0x{pcmd_ptr:08x}")
            print(f"  pCmd[0..32]: {cap.hex()}")
            words = struct.unpack("<8I", cap)
            for i, w in enumerate(words):
                print(f"    +0x{i*4:02x}: 0x{w:08x}")
            samples.append((label, start, n, words))
    finally:
        r.patch_target_code(nk, PATCH_VA, ORIGINAL_BYTES)
        print("\nhook restored")

    # Diff analysis
    print("\n=== Diff across samples ===")
    if len(samples) >= 2:
        for i in range(8):
            vals = [s[3][i] for s in samples]
            varies = len(set(vals)) > 1
            print(f"  +0x{i*4:02x}: {'VARIES' if varies else 'same   '}  {[f'{v:08x}' for v in vals]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
