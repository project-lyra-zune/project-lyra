"""Emulate the assembled stub and check what it does to the outside world."""

import json
import pathlib
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_SP, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3)

STUB_JSON = pathlib.Path(__file__).resolve().parent.parent / "stub.json"

BASE, STACK, CORE = 0x0A000000, 0x0B000000, 0x40320000
IMPORTS = {
    0x40336B64: "CreateFileW",
    0x40336BCC: "WriteFile",
    0x4033226C: "CloseHandle",
    0x40331C14: "CreateProcessW",
    0x403440A4: "ExitThread",
}
HFILE = 0x00DEAD01
BX_LR = 0xE12FFF1E
TARGET = "\\Flash2\\lyraboot.exe"


def read_wstr(uc, addr, cap=260):
    out = b""
    for i in range(cap):
        c = bytes(uc.mem_read(addr + i * 2, 2))
        if c == b"\0\0":
            break
        out += c
    return out.decode("utf-16-le")


def run(image, chunk_limit, fail_open=False, fail_write_at=None):
    stub = bytes(json.loads(STUB_JSON.read_text())["bytes"])
    blob = stub + struct.pack("<I", len(image)) + image

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(BASE, 0x100000)
    uc.mem_map(STACK, 0x10000)
    uc.mem_map(CORE, 0x40000)
    uc.mem_write(BASE, blob)
    for va in IMPORTS:
        uc.mem_write(va, struct.pack("<I", BX_LR))
    uc.reg_write(UC_ARM_REG_SP, STACK + 0x8000)

    st = {"written": b"", "launched": None, "closed": False, "calls": [], "opened": None}

    def hook(uc, addr, size, _user):
        if addr not in IMPORTS:
            return
        name = IMPORTS[addr]
        st["calls"].append(name)
        sp = uc.reg_read(UC_ARM_REG_SP)
        a = [uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]

        if name == "CreateFileW":
            st["opened"] = read_wstr(uc, a[0])
            disposition = struct.unpack("<I", uc.mem_read(sp, 4))[0]
            assert a[1] == 0x40000000, f"desired access {a[1]:#x}, expected GENERIC_WRITE"
            assert disposition == 2, f"disposition {disposition}, expected CREATE_ALWAYS"
            uc.reg_write(UC_ARM_REG_R0, 0xFFFFFFFF if fail_open else HFILE)
        elif name == "WriteFile":
            assert a[0] == HFILE, "WriteFile got a handle CreateFileW never returned"
            if fail_write_at is not None and len(st["written"]) >= fail_write_at:
                uc.reg_write(UC_ARM_REG_R0, 0)
                return
            n = min(a[2], chunk_limit)
            st["written"] += bytes(uc.mem_read(a[1], n))
            uc.mem_write(a[3], struct.pack("<I", n))
            uc.reg_write(UC_ARM_REG_R0, 1)
        elif name == "CloseHandle":
            assert a[0] == HFILE
            st["closed"] = True
            uc.reg_write(UC_ARM_REG_R0, 1)
        elif name == "CreateProcessW":
            st["launched"] = read_wstr(uc, a[0])
            uc.reg_write(UC_ARM_REG_R0, 1)
        elif name == "ExitThread":
            uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook)
    try:
        uc.emu_start(BASE, BASE + len(stub), timeout=10_000_000, count=2_000_000)
    except UcError as e:
        st["error"] = str(e)
    return st


FAILURES = []


def check(label, cond, extra=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {label}" + (f"  ({extra})" if extra and not cond else ""))
    if not cond:
        FAILURES.append(label)


def main():
    image = bytes((i * 7 + 3) & 0xFF for i in range(55296))
    full_order = ["CreateFileW", "WriteFile", "CloseHandle", "CreateProcessW", "ExitThread"]

    print("one-shot write")
    r = run(image, 1 << 30)
    check("opens the target path", r["opened"] == TARGET, r["opened"])
    check("image lands byte-exact", r["written"] == image, f"{len(r['written'])}/{len(image)}")
    check("closes the handle", r["closed"])
    check("launches the path it wrote", r["launched"] == TARGET, r["launched"])
    check("call order", r["calls"] == full_order, r["calls"])

    print("\npartial writes, 4096-byte chunks")
    r = run(image, 4096)
    check("image lands byte-exact", r["written"] == image, f"{len(r['written'])}/{len(image)}")
    check("loops once per chunk", r["calls"].count("WriteFile") == 14, r["calls"].count("WriteFile"))
    check("launches", r["launched"] == TARGET)

    print("\npathological one-byte writes")
    small = image[:3000]
    r = run(small, 1)
    check("image lands byte-exact", r["written"] == small, f"{len(r['written'])}/{len(small)}")
    check("one call per byte", r["calls"].count("WriteFile") == len(small))

    print("\nCreateFileW fails")
    r = run(image, 1 << 30, fail_open=True)
    check("writes nothing", r["written"] == b"")
    check("launches nothing", r["launched"] is None, r["launched"])
    check("still exits cleanly", r["calls"][-1] == "ExitThread", r["calls"])

    print("\nWriteFile fails midway")
    r = run(image, 4096, fail_write_at=8192)
    check("stops at the failure", len(r["written"]) == 8192, len(r["written"]))
    check("still closes the handle", r["closed"])
    check("does not spin", r["calls"][-1] == "ExitThread", r["calls"][-3:])

    print("\nempty image")
    r = run(b"", 1 << 30)
    check("skips WriteFile entirely", "WriteFile" not in r["calls"], r["calls"])
    check("still launches", r["launched"] == TARGET)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILURES: " + ", ".join(FAILURES))
        return 1
    print("all stub emulation tests pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
