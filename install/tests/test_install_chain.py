"""Build the page, decode it with its own JavaScript, run the stub on the result.

Covers the page/stub handoff, which neither component owns alone.
"""

import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_SP, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3)

HERE = pathlib.Path(__file__).resolve().parent
INSTALL = HERE.parent
LYRA = INSTALL.parent
LYRABOOT = LYRA / "src" / "lyraboot" / "bin" / "lyraboot.exe"
BOOT_PIN = LYRA / "src" / "lyraboot" / "boot_pin.h"

BASE, STACK, CORE = 0x0A000000, 0x0B000000, 0x40320000
IMPORTS = {0x40336B64: "CreateFileW", 0x40336BCC: "WriteFile", 0x4033226C: "CloseHandle",
           0x40331C14: "CreateProcessW", 0x403440A4: "ExitThread"}
HFILE = 0x00DEAD01
TARGET = "\\Flash2\\lyraboot.exe"

FAILURES = []


def check(label, cond, extra=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {label}" + (f"  ({extra})" if extra and not cond else ""))
    if not cond:
        FAILURES.append(label)


def read_wstr(uc, addr):
    out = b""
    for i in range(260):
        c = bytes(uc.mem_read(addr + i * 2, 2))
        if c == b"\0\0":
            break
        out += c
    return out.decode("utf-16-le")


def emulate(blob, stub_len, chunk):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(BASE, 0x100000)
    uc.mem_map(STACK, 0x10000)
    uc.mem_map(CORE, 0x40000)
    uc.mem_write(BASE, blob)
    for va in IMPORTS:
        uc.mem_write(va, struct.pack("<I", 0xE12FFF1E))
    uc.reg_write(UC_ARM_REG_SP, STACK + 0x8000)

    st = {"written": b"", "opened": None, "launched": None}

    def hook(uc, addr, size, _user):
        if addr not in IMPORTS:
            return
        name = IMPORTS[addr]
        a = [uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if name == "CreateFileW":
            st["opened"] = read_wstr(uc, a[0])
            uc.reg_write(UC_ARM_REG_R0, HFILE)
        elif name == "WriteFile":
            n = min(a[2], chunk)
            st["written"] += bytes(uc.mem_read(a[1], n))
            uc.mem_write(a[3], struct.pack("<I", n))
            uc.reg_write(UC_ARM_REG_R0, 1)
        elif name == "CloseHandle":
            uc.reg_write(UC_ARM_REG_R0, 1)
        elif name == "CreateProcessW":
            st["launched"] = read_wstr(uc, a[0])
            uc.reg_write(UC_ARM_REG_R0, 1)
        elif name == "ExitThread":
            uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(BASE, BASE + stub_len, timeout=60_000_000, count=50_000_000)
    return st


def read_pin():
    text = BOOT_PIN.read_text()
    pin = {}
    for macro in ("LYRA_PIN_VERSION", "LYRA_PIN_URL", "LYRA_PIN_SHA256"):
        m = re.search(r'^#define\s+%s\s+"([^"]*)"$' % macro, text, re.M)
        if not m:
            raise SystemExit(f"{BOOT_PIN} has no {macro}")
        pin[macro] = m.group(1)
    return pin


def main():
    if not LYRABOOT.exists():
        print(f"  SKIP  {LYRABOOT} not built (tools/win7-build.sh lyraboot)")
        return 0

    image = LYRABOOT.read_bytes()
    stub = bytes(json.loads((INSTALL / "stub" / "stub.json").read_text())["bytes"])
    pin = read_pin()
    VERSION = pin["LYRA_PIN_VERSION"]

    print("built bootstrap matches the committed pin")
    for macro, value in pin.items():
        check(f"binary carries {macro}", value.encode() in image, value)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        subprocess.run([sys.executable, str(INSTALL / "build-page.py"),
                        "--version", VERSION,
                        "--pin-url", pin["LYRA_PIN_URL"],
                        "--pin-sha", pin["LYRA_PIN_SHA256"],
                        "-o", str(tmp)],
                       check=True, stdout=subprocess.DEVNULL)

        page = tmp / "index.html"
        b64 = tmp / f"lyraboot-{VERSION}.b64"
        blob_path = tmp / "memory.bin"
        out = subprocess.run(["node", str(HERE / "reconstruct.js"), str(page), str(b64), str(blob_path)],
                             check=True, capture_output=True, text=True)
        info = json.loads(out.stdout)
        blob = blob_path.read_bytes()

    print("\npage decodes what it claims")
    check("reported byte count matches the stamped one",
          info["reportedBytes"] == info["expectedBytes"],
          f"{info['reportedBytes']} vs {info['expectedBytes']}")
    check("stub is carried whole", info["stubBytes"] == len(stub))
    check("memory image is an even number of bytes", len(blob) % 2 == 0, len(blob))

    print("\nmemory image is laid out the way the stub reads it")
    check("stub sits at the front", blob[:len(stub)] == stub)
    n, = struct.unpack_from("<I", blob, len(stub))
    check("length prefix is the image size", n == len(image), f"{n} vs {len(image)}")
    check("image follows the prefix intact", blob[len(stub) + 4:len(stub) + 4 + n] == image)

    print("\nstub run over the page's own output")
    for chunk, label in ((1 << 30, "one shot"), (7777, "awkward 7777-byte chunks")):
        st = emulate(blob, len(stub), chunk)
        check(f"{label}: writes lyraboot.exe byte-exact", st["written"] == image,
              f"{len(st['written'])} of {len(image)}, "
              f"{hashlib.sha256(st['written']).hexdigest()[:16]}")
        check(f"{label}: opens and launches {TARGET}",
              st["opened"] == TARGET and st["launched"] == TARGET,
              f"{st['opened']} / {st['launched']}")

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILURES: " + ", ".join(FAILURES))
        return 1
    print("page, stub and image agree end to end")
    return 0


if __name__ == "__main__":
    sys.exit(main())
