"""Reference implementation of Phase 2's fixup-applier in Python.

Phase 2 (on-device, in C) will do the same arithmetic against the
plant VA. Keeping this Python implementation lets us:
  - Round-trip-validate emitted blobs against live device bytes
  - Test class assembly without flashing the device
  - Serve as a literal spec for the C implementation
"""
from __future__ import annotations

import struct

from .classblob import Fixup, FixupKind


def apply_fixups(blob: bytes | bytearray, fixups: list[Fixup],
                  plant_va: int) -> bytes:
    """Apply all fixups against `blob` as if it were planted at `plant_va`.

    Returns the patched bytes (a copy of blob with fixups applied).
    """
    out = bytearray(blob)
    for fx in fixups:
        # extern_module fixups can only be resolved on-device via
        # GetProcAddress; skip them in the host-side applier (the binary
        # slot remains 0; the device-side fixup pass writes the real VA).
        if fx.extern_module is not None:
            continue
        if fx.kind == FixupKind.IMM8:
            if fx.value is None:
                raise ValueError(f"IMM8 fixup at {fx.at:#x} has no value")
            if not (0 <= fx.value <= 0xff):
                raise ValueError(f"IMM8 value {fx.value} out of range")
            out[fx.at] = fx.value
        elif fx.kind == FixupKind.BL:
            target_va = _resolve_target(fx, plant_va)
            instr_va = plant_va + fx.at
            rel = target_va - (instr_va + 8)
            if rel & 3:
                raise ValueError(f"BL target {target_va:#x} not 4-aligned "
                                  f"relative to instr {instr_va:#x}")
            off24 = (rel >> 2) & 0xffffff
            instr = struct.unpack_from("<I", out, fx.at)[0]
            instr = (instr & 0xff000000) | off24
            struct.pack_into("<I", out, fx.at, instr)
        elif fx.kind == FixupKind.WORD:
            target_va = _resolve_target(fx, plant_va)
            struct.pack_into("<I", out, fx.at, target_va & 0xffffffff)
        else:
            raise ValueError(f"unknown fixup kind {fx.kind!r}")
    return bytes(out)


def _resolve_target(fx: Fixup, plant_va: int) -> int:
    if fx.abs is not None:
        return fx.abs
    if fx.intern is not None:
        return plant_va + fx.intern
    raise ValueError(f"fixup at {fx.at:#x} has no resolvable target")
