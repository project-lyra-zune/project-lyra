#!/usr/bin/env python3
"""Touch-injection PoC: synthesize a tap in gemstone via XuiProcessMouseMessage.

Proves the device-validated mouse-message struct (notes/re-2026-05-28-touch-inject/
real-touch-path-RE.md) drives the XUI. Throwaway scaffolding: the real
injector is a gemstone-resident thread in the modkit mod; this just confirms the
struct + call land before that build.

Why a kernel-scratch struct (not VirtualAlloc / gemstone heap): xexec swaps TTBR0
to gemstone and runs the stub PL1 (kcall->helper-v4 is kernel-mode), so the stub
reaches gemstone user VAs (xuidll 0x418xxxxx) AND the global kernel mapping. A
struct in the helpers' kernel page is reachable, persistent, and never touches
gemstone's heap. VirtualAlloc would be wrong: it is a process-context syscall and
under xexec the current process is still nativeapp.

Usage: zune-touch-poc.py <ip> <x> <y>     (x,y = portrait pixels, 0..271 / 0..479)
"""
import sys, struct, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL
import lyra_debug_common as dbg

HOST           = 0x00010000      # [appObj+8], captured live
XUI_PROC_MOUSE = 0x4181f038      # xuidll!XuiProcessMouseMessage

# free area of the helpers' kernel scratch page (0x80015000); clear of
# KSCRATCH 0x800152D0 / KORIG 0x80015320 / helpers (~..0x800153c0)
STRUCT_VA = 0x80015800
POINT_VA  = 0x80015840

def read_back_stub(kva):
    # r0 = *(u32*)kva ; bx lr   (PL1 kernel-VA reachability probe, read-only)
    return struct.pack("<4I", 0xe59f0004, 0xe5900000, 0xe12fff1e, kva)

def call_stub(struct_va):
    # XuiProcessMouseMessage(HOST, struct_va)
    insns = [0xe52de004,  # push {lr}
             0xe59f000c,  # ldr r0,[pc,#0xc] -> HOST
             0xe59f100c,  # ldr r1,[pc,#0xc] -> struct_va
             0xe59fc00c,  # ldr ip,[pc,#0xc] -> XUI_PROC_MOUSE
             0xe12fff3c,  # blx ip
             0xe49df004]  # pop {pc}
    return b"".join(struct.pack("<I", w) for w in insns + [HOST, struct_va, XUI_PROC_MOUSE])

def mouse_struct(ptr_va, type_code):
    return struct.pack("<8I", 0x14, type_code, 0, 0x2c, ptr_va, 3, 1, 1)

def point_record(x, y):
    return struct.pack("<12I", 1, 1, 0, x & 0xffff, y & 0xffff, 0, 0, 0, 0, 0, 0, 0)

def main():
    if len(sys.argv) < 4:
        print(__doc__); return 1
    ip, x, y = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

    # proc-struct via NK walk (debug_common); ZuneREPL for kwrite/xexec/etc.
    dsock, _ = dbg.connect(ip, 1337, 6.0)
    _, proc = dbg.find_process(dsock, None, "gemstone.exe", 16, 64)
    dsock.close()
    target_proc, pid, base = proc["proc_ptr"], proc["id"], proc["base"]
    print(f"gemstone: pid=0x{pid:08x} base=0x{base:08x} proc_ptr=0x{target_proc:08x}")

    r = ZuneREPL(ip)

    # 1) verify the kernel struct scratch round-trips (our own memory)
    r.kwrite(STRUCT_VA, struct.pack("<I", 0xA5A5F00D))
    got = struct.unpack("<I", r.kread(STRUCT_VA, 4))[0]
    print(f"kernel scratch 0x{STRUCT_VA:08x} round-trip = 0x{got:08x} "
          f"({'OK' if got == 0xA5A5F00D else 'FAIL'})")
    if got != 0xA5A5F00D:
        print("kernel scratch unusable; aborting"); return 1

    # 2) borrow a gemstone private-heap page for the stub (XN-clear on CE6);
    #    xexec saves+restores it, so the borrow is transient
    borrow = None
    for mbi in r.virt_walk(pid, 0x10000, 0x01000000):
        if mbi.state == 0x1000 and mbi.type == 0x20000 and mbi.is_writable \
           and mbi.region_size >= 0x1000:
            borrow = mbi.base + max(mbi.region_size - 0x100, 0)
            print(f"borrow page = 0x{borrow:08x} "
                  f"(heap region 0x{mbi.base:08x} size 0x{mbi.region_size:x})")
            break
    if borrow is None:
        print("no private-heap borrow page found"); return 1

    # 3) PL1 reachability pre-test: stub reads STRUCT_VA, returns it (read-only)
    rv = r.xexec(target_proc, borrow, read_back_stub(STRUCT_VA)) & 0xffffffff
    print(f"PL1 read-back of 0x{STRUCT_VA:08x} = 0x{rv:08x} "
          f"({'reachable' if rv == 0xA5A5F00D else 'NOT reachable'})")
    if rv != 0xA5A5F00D:
        print("xexec stub cannot reach kernel VA; need gemstone-local buffer instead.")
        return 1

    # 4) stage the device-validated struct (DOWN) in kernel scratch
    r.kwrite(POINT_VA, point_record(x, y))
    r.kwrite(STRUCT_VA, mouse_struct(POINT_VA, 0x30))
    print(f"staged struct@0x{STRUCT_VA:08x} point@0x{POINT_VA:08x} coords=({x},{y})")

    stub = call_stub(STRUCT_VA)
    print(f"\n>>> INJECTING TAP at portrait ({x},{y}), watch the device <<<")
    rv = r.xexec(target_proc, borrow, stub) & 0xffffffff
    print(f"DOWN  XuiProcessMouseMessage rv=0x{rv:08x}")
    time.sleep(0.15)
    r.kwrite(STRUCT_VA + 4, struct.pack("<I", 0x31))  # type -> UP
    rv = r.xexec(target_proc, borrow, stub) & 0xffffffff
    print(f"UP    XuiProcessMouseMessage rv=0x{rv:08x}")

    # dump the struct after the call: SendMouseMessage fills the float coords +
    # the handled flag if a target consumed it
    after = r.kread(STRUCT_VA, 0x20)
    pr = r.kread(POINT_VA, 0x30)
    print("struct after:", after.hex())
    print("point  after:", pr.hex())
    print("\ndone; report what the device did.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
