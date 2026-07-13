#!/usr/bin/env python3
"""Reverse-engineering helper for gemstone+xuidll code.

Reads function bytes from the live device (via zune_repl), disassembles
with capstone (ARM, little-endian), and annotates:
  - bl/b targets (and recognizes IAT thunks -> xuidll exports by name)
  - ldr [pc, #imm] literals (and reads the pointed-at value)
  - known function names loaded from a per-build symbols JSON

Usage:
    python3 tools/disasm/re-helper.py <hex_VA> [<length>]
    python3 tools/disasm/re-helper.py 0x1c3f4 96 --symbols gemstone-symbols.v4.5.json
"""
from __future__ import annotations
import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))

import capstone

from zune_repl import ZuneREPL


DEFAULT_SYMBOLS = Path(__file__).resolve().parent / "gemstone-symbols.v4.5.json"


def load_symbols(path: Path) -> dict[int, str]:
    """Load a {hex-string VA: name} JSON map into {int VA: name}. Missing file -> {}."""
    if not path or not path.exists():
        return {}
    raw = json.loads(path.read_text())
    return {int(k, 0): v for k, v in raw.items()}


def is_iat_thunk(va: int, raw: bytes, base: int) -> int | None:
    """Detect an IAT thunk like:
        ldr r12, [pc, #4]
        ldr r12, [r12]
        bx  r12
        .word <iat_slot_va>
    Returns the IAT slot VA, or None.
    """
    off = va - base
    if off < 0 or off + 16 > len(raw):
        return None
    w = struct.unpack_from("<4I", raw, off)
    if w[0] == 0xe59fc004 and w[1] == 0xe59cc000 and w[2] == 0xe12fff1c:
        return w[3]
    return None


def annotate_target(va: int, repl: ZuneREPL, pid: int,
                    block_bytes: bytes, block_base: int,
                    symbols: dict[int, str]) -> str:
    """Return a one-line annotation for a target VA."""
    if va in symbols:
        return symbols[va]
    # Is this an IAT thunk?
    slot = is_iat_thunk(va, block_bytes, block_base)
    if slot is None:
        # We may not have the bytes; fetch 16 from device
        try:
            tb = repl.read_proc_mem(pid, va, 16)
            slot = is_iat_thunk(va, tb, va)
        except Exception:
            slot = None
    if slot is not None:
        # The IAT slot holds a runtime VA into xuidll/coredll.
        try:
            tgt = struct.unpack("<I", repl.read_proc_mem(pid, slot, 4))[0]
            return f"thunk -> IAT[0x{slot:08x}] = 0x{tgt:08x}"
        except Exception:
            return f"thunk -> IAT[0x{slot:08x}]"
    return f"sub_0x{va:x}"


def disasm_block(repl: ZuneREPL, pid: int, va: int, length: int,
                 symbols: dict[int, str]):
    raw = repl.read_proc_mem(pid, va, length)
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.detail = True

    print(f'--- 0x{va:08x} ({length} bytes) ---')
    for ins in md.disasm(raw, va):
        line = f'  0x{ins.address:08x}  {ins.bytes.hex()}  {ins.mnemonic:6} {ins.op_str}'
        note = ""
        # LDR rX, [pc, #imm]: resolve literal
        if ins.mnemonic.startswith("ldr") and "[pc" in ins.op_str:
            # Capstone provides operand details
            for op in ins.operands:
                if op.type == capstone.arm.ARM_OP_MEM and op.mem.base == capstone.arm.ARM_REG_PC:
                    disp = op.mem.disp
                    t = ins.address + 8 + disp
                    off = t - va
                    if 0 <= off + 4 <= len(raw):
                        v = struct.unpack_from("<I", raw, off)[0]
                        note = f"  ; *0x{t:08x} = 0x{v:08x}"
                        if v in symbols:
                            note += f" ({symbols[v]})"
        # bl/b: resolve target
        elif ins.mnemonic in ("bl", "b", "blx") and ins.operands:
            op = ins.operands[0]
            if op.type == capstone.arm.ARM_OP_IMM:
                t = op.imm
                ann = annotate_target(t, repl, pid, raw, va, symbols)
                note = f"  ; -> {ann}"
        print(line + note)
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("va", type=lambda s: int(s, 0))
    ap.add_argument("length", nargs="?", default=128, type=lambda s: int(s, 0))
    ap.add_argument("--ip", default="127.0.0.1")
    ap.add_argument("--process", default="gemstone",
                    help="case-insensitive process name to disassemble in")
    ap.add_argument("--symbols", type=Path, default=DEFAULT_SYMBOLS,
                    help="JSON map of {hex VA: name} annotations")
    args = ap.parse_args()
    symbols = load_symbols(args.symbols)
    r = ZuneREPL(args.ip)
    target_pid = None
    for pid, name in r.list_processes():
        if args.process.lower() in name.lower():
            target_pid = pid
            break
    if target_pid is None:
        sys.exit(f"{args.process} not running")
    disasm_block(r, target_pid, args.va, args.length, symbols)


if __name__ == "__main__":
    main()
