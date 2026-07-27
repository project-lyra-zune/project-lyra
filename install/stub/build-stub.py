#!/usr/bin/env python3
"""Assemble stub.S into stub.json, and verify the result before it can ship."""

import json
import pathlib
import shutil
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
SRC = HERE / "stub.S"
OUT_JSON = HERE / "stub.json"

EXPECTED_IMPORTS = {
    0x40336B64: "CreateFileW",
    0x40336BCC: "WriteFile",
    0x4033226C: "CloseHandle",
    0x40331C14: "CreateProcessW",
    0x403440A4: "ExitThread",
}

TARGET_PATH = "\\Flash2\\lyraboot.exe"


def assemble(tmp_o: pathlib.Path) -> None:
    cc = shutil.which("clang")
    if not cc:
        sys.exit("clang not found; it is the ARM assembler this script uses")
    subprocess.run(
        [cc, "-target", "armv5te-none-eabi", "-c", str(SRC), "-o", str(tmp_o)],
        check=True,
    )


def elf_section(obj: bytes, want: str) -> bytes:
    """Return one section's bytes from a little-endian 32-bit ELF object."""
    if obj[:4] != b"\x7fELF" or obj[4] != 1 or obj[5] != 1:
        sys.exit("assembler did not produce a 32-bit little-endian ELF")
    e_shoff, = struct.unpack_from("<I", obj, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", obj, 0x2E)

    def hdr(i):
        base = e_shoff + i * e_shentsize
        name, _typ, _flags, _addr, off, size = struct.unpack_from("<IIIIII", obj, base)
        return name, off, size

    _, str_off, _ = hdr(e_shstrndx)
    for i in range(e_shnum):
        name_off, off, size = hdr(i)
        end = obj.index(b"\0", str_off + name_off)
        if obj[str_off + name_off:end].decode() == want:
            return obj[off:off + size]
    sys.exit(f"no {want} section in the assembled object")


def verify(blob: bytes) -> None:
    try:
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    except ImportError:
        sys.exit("capstone is required to verify the stub before it ships")

    if len(blob) % 4:
        sys.exit(f"stub is {len(blob)} bytes, not word-aligned; the image would be misread")

    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    insns = list(md.disasm(blob, 0))
    if not insns:
        sys.exit("stub did not disassemble as ARM")

    text = [f"{i.mnemonic} {i.op_str}" for i in insns]

    found = set()
    for off in range(0, len(blob) - 3, 4):
        word, = struct.unpack_from("<I", blob, off)
        if word in EXPECTED_IMPORTS:
            found.add(word)
    missing = set(EXPECTED_IMPORTS) - found
    if missing:
        sys.exit("literal pool missing: " + ", ".join(EXPECTED_IMPORTS[m] for m in missing))

    if sum(1 for t in text if t.startswith("bx ")) != len(EXPECTED_IMPORTS):
        sys.exit(f"expected {len(EXPECTED_IMPORTS)} bx call sites, got {sum(1 for t in text if t.startswith('bx '))}")

    wanted = (TARGET_PATH + "\0").encode("utf-16-le")
    if blob.count(wanted) != 1:
        sys.exit(f"expected exactly one NUL-terminated {TARGET_PATH!r} in the stub, "
                 f"found {blob.count(wanted)}")

    print(f"stub: {len(blob)} bytes, {len(insns)} instructions")
    print(f"      writes and launches {TARGET_PATH}")
    print("      imports: " + ", ".join(EXPECTED_IMPORTS[v] for v in sorted(found)))


def main() -> None:
    tmp_o = HERE / "stub.o"
    try:
        assemble(tmp_o)
        blob = elf_section(tmp_o.read_bytes(), ".text")
    finally:
        tmp_o.unlink(missing_ok=True)

    verify(blob)
    OUT_JSON.write_text(json.dumps({
        "bytes": list(blob),
        "path": TARGET_PATH,
    }, indent=1) + "\n")
    print(f"      wrote {OUT_JSON.relative_to(HERE.parent.parent)}")


if __name__ == "__main__":
    main()
