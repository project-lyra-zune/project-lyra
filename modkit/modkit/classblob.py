"""Class-blob format + assembler for mod-shipped XUI scene classes.

A *class blob* is the bytes the on-device `register_xui_class` capability
plants into gemstone scratch to bring a custom C++ scene class to life
without compiling C++ on the device.

The blob contains, concatenated:
  - factory  (76 bytes: allocates an instance, calls ctor)
  - ctor     (~52 bytes: stores vtable ptr at instance+0, zeroes rest)
  - vtable   (≥16 bytes: record 0 = (parent_vt, OnMsg, OnInit, OnDestroy))
  - any handler bodies (OnMessage, OnInit, per-row select, …)

Each blob ships with a sidecar JSON describing:
  - instance_size
  - factory_offset (the byte the class descriptor's +0x18 field points at)
  - the list of plant-VA-relative fixups Phase 2 must apply

External symbol references (e.g. `bl <allocator>`) are pre-resolved by
this assembler to absolute VAs and emitted as `abs`-target fixups. The
on-device runtime never sees a symbol name, only `(at, kind, abs)` or
`(at, kind, intern)` tuples. Re-targeting to a different firmware is
a rebuild, not a runtime concern.

Fixup kinds:
  IMM8        : write low byte at `plant_va + at`; for `mov rN, #imm` etc.
  BL          : encode 24-bit signed offset into bl/b instruction at `plant_va + at`
                  target = abs (extern) or plant_va + intern (internal label)
  WORD        : write a 32-bit value at `plant_va + at`
                  value = abs (extern) or plant_va + intern (internal label)

Authoring a class: see `ClassBlob.builder()` (used by the per-mod build
scripts, e.g. `build_quicksettings.py` and `build_gemmod_manager.py`).
"""
from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field, asdict
from enum import Enum
from pathlib import Path
from typing import Union

from .firmware_v45_abi import EXTERNS, lookup as resolve_extern


class FixupKind(str, Enum):
    IMM8 = "imm8"
    BL = "bl"
    WORD = "word"


@dataclass
class Fixup:
    """One plant-VA-relative fixup applied by Phase 2 at register-time.

    Exactly one of `abs` / `intern` / `value` / `extern_module` is set:
      - IMM8 uses `value` (a 0..255 literal)
      - BL / WORD use one of:
          * `abs`: absolute target VA, baked at build time (XIP gemstone)
          * `intern`: byte offset inside this same blob
          * `extern_module = (module, symbol)`, resolved at PLANT time by
            GetProcAddress on the named module. Use for DLLs whose load
            base shifts per build (zuxhook). Eliminates the "rebuild every
            class blob after every zuxhook rebuild" tax.
    """
    at: int
    kind: FixupKind
    abs: int | None = None
    intern: int | None = None
    value: int | None = None
    extern_module: tuple[str, str] | None = None

    def to_json(self) -> dict:
        d: dict = {"at": self.at, "kind": self.kind.value}
        if self.kind == FixupKind.IMM8:
            d["value"] = self.value
        elif self.extern_module is not None:
            d["extern_module"] = {
                "module": self.extern_module[0],
                "symbol": self.extern_module[1],
            }
        elif self.abs is not None:
            d["abs"] = self.abs
        elif self.intern is not None:
            d["intern"] = self.intern
        else:
            raise ValueError(f"fixup at {self.at:#x} has no target")
        return d


@dataclass
class ClassBlob:
    """A fully-assembled class blob ready to write to disk.

    bytes_ holds concatenated machine code + data. `exports` names the
    important entry points inside the blob (the descriptor's +0x18 field
    will be plant_va + exports["factory"]). `fixups` lists everything
    Phase 2 must patch up when it plants the blob at a runtime VA.
    """
    name: str
    instance_size: int
    bytes_: bytearray
    exports: dict[str, int] = field(default_factory=dict)
    fixups: list[Fixup] = field(default_factory=list)

    # ─── emit ──────────────────────────────────────────────────────────
    def write(self, out_dir: Path):
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "class.bin").write_bytes(bytes(self.bytes_))
        manifest = {
            "format": "zune-modkit-classblob/1",
            "name": self.name,
            "instance_size": self.instance_size,
            "factory_offset": self.exports["factory"],
            "exports": self.exports,
            "fixups": [f.to_json() for f in self.fixups],
        }
        (out_dir / "class.reloc.json").write_text(
            json.dumps(manifest, indent=2) + "\n")


# ─── ARM encoding helpers ───────────────────────────────────────────────

def arm_bl_offset(at: int, target_va_within_blob_image: int) -> int:
    """Compute the 24-bit signed offset for `bl target` at `at` (within
    a hypothetical plant_va of 0). Phase 2 re-encodes with the actual
    plant_va, but emitting a 0-plant offset here lets us write the
    instruction bytes upfront and the fixup-applier just XORs in the
    correct value.
    """
    rel = target_va_within_blob_image - (at + 8)
    return (rel >> 2) & 0xffffff


def arm_pack(*words: int) -> bytes:
    return b"".join(struct.pack("<I", w & 0xffffffff) for w in words)


# ─── Factory template ───────────────────────────────────────────────────

# The 76-byte factory shared across all 90 gemstone v4.5 classes.
# Parameters: instance_size (IMM8 at +0x10), allocator (BL at +0x18),
# ctor entry (BL at +0x24).
FACTORY_TEMPLATE_WORDS: tuple[int, ...] = (
    0xe92d4030,   # +0x00  push {r4, r5, lr}
    0xe1a04001,   # +0x04  mov  r4, r1            ; r4 = out ptr
    0xe1a05000,   # +0x08  mov  r5, r0            ; r5 = caller ctx
    0xe3a03000,   # +0x0c  mov  r3, #0
    0xe3a00000,   # +0x10  mov  r0, #IMM8         ; ← instance_size fixup
    0xe5843000,   # +0x14  str  r3, [r4]          ; *out = NULL
    0xeb000000,   # +0x18  bl   allocator         ; ← BL fixup
    0xe3500000,   # +0x1c  cmp  r0, #0
    0x0a000001,   # +0x20  beq  +0x28             ; if alloc failed
    0xeb000000,   # +0x24  bl   ctor              ; ← BL fixup
    0xea000000,   # +0x28  b    +0x30             ; jump past failure
    0xe3a00000,   # +0x2c  mov  r0, #0
    0xe3500000,   # +0x30  cmp  r0, #0
    0x03a0313a,   # +0x34  moveq r3, #0x8000000E  ; HRESULT base
    0x03830807,   # +0x38  orreq r0, r3, #0x70000 ; → 0x8007000E (E_OUTOFMEMORY)
    0x15805004,   # +0x3c  strne r5, [r0, #4]     ; instance+4 = caller ctx
    0x15840000,   # +0x40  strne r0, [r4]         ; *out = instance
    0x13a00000,   # +0x44  movne r0, #0           ; S_OK
    0xe8bd8030,   # +0x48  pop  {r4, r5, pc}
)
assert len(FACTORY_TEMPLATE_WORDS) == 0x4c // 4   # 76 bytes


# Position-independent factory: the allocator is reached via an absolute
# pointer loaded from a literal pool + `blx`, not a relative `bl`. Required
# when the planted blob's scratch region is NOT within ±32MB of the host's
# allocator, e.g. registering in servicesd, whose zhud allocator (0x419d…)
# is ~1GB from the low-VA scratch region. Functionally identical to the
# `bl` factory otherwise (alloc → ctor → fill out-ptr / E_OUTOFMEMORY).
FACTORY_INDIRECT_WORDS: tuple[int, ...] = (
    0xe92d4030,   # +0x00  push {r4, r5, lr}
    0xe1a04001,   # +0x04  mov  r4, r1            ; r4 = out ptr
    0xe1a05000,   # +0x08  mov  r5, r0            ; r5 = caller ctx
    0xe3a03000,   # +0x0c  mov  r3, #0
    0xe3a00000,   # +0x10  mov  r0, #IMM8         ; ← instance_size fixup
    0xe5843000,   # +0x14  str  r3, [r4]          ; *out = NULL
    0xe59fc030,   # +0x18  ldr  ip, [pc, #0x30]   ; ip = allocator (pool @ +0x50)
    0xe12fff3c,   # +0x1c  blx  ip                ; ← WORD fixup at +0x50
    0xe3500000,   # +0x20  cmp  r0, #0
    0x0a000001,   # +0x24  beq  +0x30             ; if alloc failed
    0xeb000000,   # +0x28  bl   ctor              ; ← BL fixup (intern)
    0xea000000,   # +0x2c  b    +0x34             ; jump past failure
    0xe3a00000,   # +0x30  mov  r0, #0
    0xe3500000,   # +0x34  cmp  r0, #0
    0x03a0313a,   # +0x38  moveq r3, #0x8000000E  ; HRESULT base
    0x03830807,   # +0x3c  orreq r0, r3, #0x70000 ; → 0x8007000E (E_OUTOFMEMORY)
    0x15805004,   # +0x40  strne r5, [r0, #4]     ; instance+4 = caller ctx
    0x15840000,   # +0x44  strne r0, [r4]         ; *out = instance
    0x13a00000,   # +0x48  movne r0, #0           ; S_OK
    0xe8bd8030,   # +0x4c  pop  {r4, r5, pc}
    0x00000000,   # +0x50  pool: allocator VA     ; ← WORD abs fixup
)
assert len(FACTORY_INDIRECT_WORDS) == 0x54 // 4   # 84 bytes


# Position-independent factory for instances larger than an imm8 (>0xff). The
# alloc size is loaded from a literal pool word (baked at build time) instead of
# `mov r0, #IMM8`, and the allocator is reached via pool+blx like FACTORY_INDIRECT.
# Needed for HUD scene classes that mirror a native scene's instance footprint
# (e.g. HudNetworkListScene = 0x17c) so the base's high-offset writes stay inside
# our allocation. No new fixup kind: the size pool word is a baked literal.
FACTORY_INDIRECT_LARGE_WORDS: tuple[int, ...] = (
    0xe92d4030,   # +0x00  push {r4, r5, lr}
    0xe1a04001,   # +0x04  mov  r4, r1            ; r4 = out ptr
    0xe1a05000,   # +0x08  mov  r5, r0            ; r5 = caller ctx
    0xe3a03000,   # +0x0c  mov  r3, #0
    0xe59f003c,   # +0x10  ldr  r0, [pc, #0x3c]   ; r0 = instance_size (pool @ +0x54)
    0xe5843000,   # +0x14  str  r3, [r4]          ; *out = NULL
    0xe59fc030,   # +0x18  ldr  ip, [pc, #0x30]   ; ip = allocator (pool @ +0x50)
    0xe12fff3c,   # +0x1c  blx  ip                ; ← WORD abs fixup at +0x50
    0xe3500000,   # +0x20  cmp  r0, #0
    0x0a000001,   # +0x24  beq  +0x30
    0xeb000000,   # +0x28  bl   ctor              ; ← BL fixup (intern)
    0xea000000,   # +0x2c  b    +0x34
    0xe3a00000,   # +0x30  mov  r0, #0
    0xe3500000,   # +0x34  cmp  r0, #0
    0x03a0313a,   # +0x38  moveq r3, #0x8000000E
    0x03830807,   # +0x3c  orreq r0, r3, #0x70000 ; → 0x8007000E (E_OUTOFMEMORY)
    0x15805004,   # +0x40  strne r5, [r0, #4]
    0x15840000,   # +0x44  strne r0, [r4]
    0x13a00000,   # +0x48  movne r0, #0           ; S_OK
    0xe8bd8030,   # +0x4c  pop  {r4, r5, pc}
    0x00000000,   # +0x50  pool: allocator VA     ; ← WORD abs fixup
    0x00000000,   # +0x54  pool: instance_size    ; baked literal (no fixup)
)
assert len(FACTORY_INDIRECT_LARGE_WORDS) == 0x58 // 4   # 88 bytes


def emit_factory(at: int, instance_size: int,
                  ctor_offset_within_blob: int,
                  host: str = "gemstone",
                  indirect_allocator: bool = False) -> tuple[bytes, list[Fixup]]:
    """Emit a factory at offset `at` within the blob.

    Stages fixups for (instance_size, allocator, ctor). `host` selects which
    host process's allocator VA is baked. `indirect_allocator=True` emits the
    position-independent variant (allocator via pool+blx) for far-allocator
    hosts; otherwise the firmware-identical `bl`-allocator template.
    """
    if not (0 <= instance_size <= 0xffffffff):
        raise ValueError(f"instance_size {instance_size} out of range")
    if instance_size > 0xff:
        # imm8 can't hold the alloc size, so load it from a baked pool word.
        # Only the indirect (far-allocator) variant is defined for this, which
        # is what the large instances (HUD scenes) need anyway.
        if not indirect_allocator:
            raise ValueError(
                f"instance_size {instance_size} > 0xff requires "
                f"indirect_allocator (large pool-word-size factory)")
        words = list(FACTORY_INDIRECT_LARGE_WORDS)
        words[0x54 // 4] = instance_size & 0xffffffff   # bake size into the pool
        body = arm_pack(*words)
        fixups = [
            Fixup(at=at + 0x28, kind=FixupKind.BL,
                  intern=ctor_offset_within_blob),
            Fixup(at=at + 0x50, kind=FixupKind.WORD,
                  abs=resolve_extern("allocator", host)),
        ]
        return body, fixups
    if indirect_allocator:
        body = arm_pack(*FACTORY_INDIRECT_WORDS)
        fixups = [
            Fixup(at=at + 0x10, kind=FixupKind.IMM8, value=instance_size),
            Fixup(at=at + 0x28, kind=FixupKind.BL,
                  intern=ctor_offset_within_blob),
            Fixup(at=at + 0x50, kind=FixupKind.WORD,
                  abs=resolve_extern("allocator", host)),
        ]
        return body, fixups
    body = arm_pack(*FACTORY_TEMPLATE_WORDS)
    fixups = [
        Fixup(at=at + 0x10, kind=FixupKind.IMM8, value=instance_size),
        Fixup(at=at + 0x18, kind=FixupKind.BL,
              abs=resolve_extern("allocator", host)),
        Fixup(at=at + 0x24, kind=FixupKind.BL,
              intern=ctor_offset_within_blob),
    ]
    return body, fixups


# ─── Ctor template ──────────────────────────────────────────────────────

def emit_ctor(at: int, instance_size: int, vtable_offset_within_blob: int,
              extra_init: dict[int, int] | None = None,
              host: str = "gemstone") -> tuple[bytes, list[Fixup]]:
    """Emit a constructor at offset `at`.

    Generates `*(instance+0) = vtable; for each word offset N in instance:
    *(instance+N) = 0` (with optional non-zero `extra_init` overrides).

    extra_init maps `{instance_offset: word_value}` for slots that should
    hold non-zero constants (e.g. Settings's instance+0x18 = 0x17480).
    `word_value` is either an int (literal) or a str (extern symbol name).

    Returns (bytes, fixups).
    """
    extra_init = extra_init or {}
    if instance_size % 4 != 0:
        raise ValueError(f"instance_size {instance_size} not 4-aligned")
    if vtable_offset_within_blob < 0:
        raise ValueError("vtable_offset_within_blob must be non-negative")

    # Strategy: load r3 = vtable from a literal pool word; r1 = 0; then
    # store r3 at instance+0 and r1 at all other instance words. Any
    # extra_init slot loads a fresh pool word.
    #
    # Layout we'll emit:
    #   +0x00  ldr r3, [pc, #pool_offs_for_vtable]
    #   +0x04  mov r1, #0
    #   +0x08  str r3, [r0]
    #   +0x0c  [for each instance word other than +0x00 and any extra_init slot:
    #            str r1, [r0, #N]    (one word each)]
    #   +0xXX  [for each extra_init slot:
    #            ldr rN, [pc, #pool_offs_for_value]
    #            str rN, [r0, #slot]]
    #   +0xXX  bx lr
    #   +0xXX  pool words (vtable + each extra_init value)

    code_words: list[int] = []
    pool_fixups_pending: list[tuple[int, str, object]] = []
    # (code_word_index, "intern"|"abs", target: int for intern offset / extern name)
    # Use ONLY caller-save registers; the ctor has no prologue/epilogue,
    # so any callee-save reg (r4-r11) we touch would be returned clobbered
    # to the factory. The factory holds the caller context (scene handle)
    # in r5 and stores it into instance+4 after we return; clobbering r5
    # leaves scene_handle = 0 and crashes the engine's first
    # XuiElementGetDescendantById call from OnInit. r2 and r3 are pure
    # scratch; r12 (ip) is also caller-save by AAPCS.
    extra_regs = [2, 3, 12]

    # +0x00: ldr r3, [pc, #?], pool entry for vtable. Patched after layout.
    LDR_R3_PC = 0xe59f3000
    code_words.append(LDR_R3_PC)
    pool_fixups_pending.append((0, "intern_blob_offset", vtable_offset_within_blob))

    # +0x04: mov r1, #0
    code_words.append(0xe3a01000)
    # +0x08: str r3, [r0]
    code_words.append(0xe5803000)

    # Zero each instance word except slot 0 and extra_init slots.
    used = {0, *extra_init.keys()}
    zero_slots = [off for off in range(0, instance_size, 4) if off not in used]
    for off in zero_slots:
        # str r1, [r0, #off]
        code_words.append(0xe5801000 | (off & 0xfff))

    # Handle extra_init slots: load each value into a scratch reg, store.
    for i, (slot, val) in enumerate(extra_init.items()):
        reg = extra_regs[i % len(extra_regs)]
        # ldr rR, [pc, #?]
        code_words.append(0xe59f0000 | (reg << 12))
        pool_fixups_pending.append((len(code_words) - 1, "value", val))
        # str rR, [r0, #slot]. Base opcode is `str r0, [r0, #0]`; OR-in
        # `reg` directly into Rt without colliding with a pre-baked Rt
        # value (the earlier `0xe5801000` template had Rt=1 baked in,
        # which corrupted slots whose register bit pattern OR'd with 1
        # produced a different register, e.g. reg=4 yielded Rt=5).
        code_words.append(0xe5800000 | (reg << 12) | (slot & 0xfff))

    # bx lr
    code_words.append(0xe12fff1e)

    # Pool starts here. Compute byte offset of pool start within the ctor.
    pool_start_idx = len(code_words)

    # For each pending fixup, emit a pool word and patch the LDR's offset.
    # LDR pc-relative immediate = pool_va - (instr_va + 8). With at=0 plant,
    # instr_va_within_blob = at_ctor + (instr_idx * 4),
    # pool_va_within_blob = at_ctor + (pool_idx * 4).
    # offset = (pool_idx - instr_idx) * 4 - 8.
    fixups: list[Fixup] = []
    pool_words: list[int] = []
    next_pool_idx = pool_start_idx
    for instr_idx, target_kind, target in pool_fixups_pending:
        pool_idx = next_pool_idx
        next_pool_idx += 1
        offset = (pool_idx - instr_idx) * 4 - 8
        if not (0 <= offset <= 0xfff):
            raise ValueError(
                f"ctor: ldr pc-relative offset {offset} out of range; "
                f"ctor too large?")
        code_words[instr_idx] |= offset
        # The pool word itself: place 0 in the bytes, then a fixup writes
        # the real value at plant-time.
        pool_words.append(0)
        word_byte_offset_within_blob = at + (pool_idx * 4)
        if target_kind == "intern_blob_offset":
            fixups.append(Fixup(at=word_byte_offset_within_blob,
                                 kind=FixupKind.WORD,
                                 intern=target))
        elif target_kind == "value":
            # `val` is either int (literal, no fixup needed; bake into pool)
            # or str (extern name → fixup)
            if isinstance(target, int):
                pool_words[-1] = target & 0xffffffff
            elif isinstance(target, str):
                fixups.append(Fixup(at=word_byte_offset_within_blob,
                                     kind=FixupKind.WORD,
                                     abs=resolve_extern(target, host)))
            else:
                raise TypeError(f"bad extra_init value: {target!r}")

    body = arm_pack(*code_words, *pool_words)
    return body, fixups


# ─── Vtable record 0 ────────────────────────────────────────────────────

def emit_vtable_record0(at: int, on_message_offset: int,
                          on_init_offset: int,
                          on_destroy: Union[int, str] = "class_ondestroy_shared",
                          host: str = "gemstone"
                          ) -> tuple[bytes, list[Fixup]]:
    """Emit the 16-byte vtable record 0.

    parent_vtable is always the shared GemBaseScene base (extern); the
    other three slots reference intra-blob offsets unless `on_destroy`
    is given as an extern name (default: reuse the canonical shared
    destroy at 0x8678c). `host` selects the host process's VAs.
    """
    body = arm_pack(0, 0, 0, 0)
    fixups = [
        Fixup(at=at + 0x00, kind=FixupKind.WORD,
              abs=resolve_extern("parent_vtable", host)),
        Fixup(at=at + 0x04, kind=FixupKind.WORD, intern=on_message_offset),
        Fixup(at=at + 0x08, kind=FixupKind.WORD, intern=on_init_offset),
    ]
    if isinstance(on_destroy, str):
        fixups.append(Fixup(at=at + 0x0c, kind=FixupKind.WORD,
                             abs=resolve_extern(on_destroy, host)))
    else:
        fixups.append(Fixup(at=at + 0x0c, kind=FixupKind.WORD,
                             intern=on_destroy))
    return body, fixups


# ─── ClassBlob builder ──────────────────────────────────────────────────

class ClassBlobBuilder:
    """Stateful builder for assembling a class blob piece-by-piece.

    Each `add_*` call appends bytes and records the export name (for the
    factory/ctor/vtable, used to fill in the runtime descriptor's +0x18
    slot and the ctor's vtable load). Use `.finish()` to get the
    `ClassBlob`.
    """
    def __init__(self, name: str, instance_size: int, host: str = "gemstone"):
        self.name = name
        self.instance_size = instance_size
        self.host = host
        self.bytes_ = bytearray()
        self.exports: dict[str, int] = {}
        self.fixups: list[Fixup] = []
        # Forward-reference table: name → offset (filled when added)
        self._labels: dict[str, int] = {}
        # Forward fixups awaiting label resolution
        self._pending: list[tuple[Fixup, str]] = []   # (fixup, intern_label_name)

    def cursor(self) -> int:
        return len(self.bytes_)

    def add_factory(self, ctor_label: str = "ctor",
                    indirect_allocator: bool = False) -> None:
        at = self.cursor()
        body, fx = emit_factory(at, self.instance_size,
                                 ctor_offset_within_blob=0, host=self.host,
                                 indirect_allocator=indirect_allocator)
        # Override the intern ctor fixup to be label-resolved later.
        for f in fx:
            if f.kind == FixupKind.BL and f.intern == 0:
                self._pending.append((f, ctor_label))
                f.intern = None        # will be filled at finish()
                break
        self.bytes_.extend(body)
        self.fixups.extend(fx)
        self.exports["factory"] = at

    def add_ctor(self, extra_init: dict[int, int] | None = None,
                  vtable_label: str = "vtable") -> None:
        at = self.cursor()
        body, fx = emit_ctor(at, self.instance_size,
                              vtable_offset_within_blob=0,
                              extra_init=extra_init, host=self.host)
        for f in fx:
            if f.kind == FixupKind.WORD and f.intern == 0:
                self._pending.append((f, vtable_label))
                f.intern = None
                break
        self.bytes_.extend(body)
        self.fixups.extend(fx)
        self.exports["ctor"] = at
        self._labels["ctor"] = at

    def add_vtable(self,
                    on_message: Union[str, int, tuple[str, str]] = "OnMessage",
                    on_init:    Union[str, int, tuple[str, str]] = "OnInit",
                    on_destroy: Union[str, int, tuple[str, str]] = "class_ondestroy_shared"
                    ) -> None:
        """Add vtable record 0.

        Each slot accepts:
          - a `str` naming an internal label inside this blob (deferred
            label, resolved at .finish())
          - an `int` (absolute VA, e.g. a fixed XIP gemstone address);
            becomes an extern abs fixup directly
          - a `(module, symbol)` tuple; the symbol is resolved by
            GetProcAddress on the named module at plant time on device.
            Use this for zuxhook exports so blobs don't need rebuilding
            after every zuxhook rebuild.
        The `on_destroy` slot additionally accepts an extern symbol name
        from `firmware_v45_abi.EXTERNS` (default: `class_ondestroy_shared`).
        """
        at = self.cursor()
        assert at % 4 == 0
        # Emit with placeholder body; we replace fixups below.
        body = arm_pack(0, 0, 0, 0)
        self.bytes_.extend(body)

        # parent_vtable: always the shared GemBaseScene base (per host)
        self.fixups.append(Fixup(at=at + 0x00, kind=FixupKind.WORD,
                                   abs=resolve_extern("parent_vtable", self.host)))
        # OnMessage / OnInit: defer to internal label, abs VA, or extern_module
        self._add_vtable_slot(at + 0x04, on_message)
        self._add_vtable_slot(at + 0x08, on_init)
        # OnDestroy: accepts intern label, abs VA, extern symbol name, or extern_module
        if isinstance(on_destroy, str) and on_destroy in EXTERNS:
            self.fixups.append(Fixup(at=at + 0x0c, kind=FixupKind.WORD,
                                       abs=resolve_extern(on_destroy, self.host)))
        else:
            self._add_vtable_slot(at + 0x0c, on_destroy)

        self.exports["vtable"] = at
        self._labels["vtable"] = at

    def _add_vtable_slot(self, slot_at: int,
                          target: Union[str, int, tuple[str, str]]) -> None:
        if isinstance(target, tuple):
            if (len(target) != 2 or
                not isinstance(target[0], str) or
                not isinstance(target[1], str)):
                raise TypeError(
                    f"extern_module slot must be (module, symbol) tuple of "
                    f"two strings, got {target!r}")
            self.fixups.append(Fixup(at=slot_at, kind=FixupKind.WORD,
                                       extern_module=target))
        elif isinstance(target, int):
            self.fixups.append(Fixup(at=slot_at, kind=FixupKind.WORD,
                                       abs=target))
        elif isinstance(target, str):
            fx = Fixup(at=slot_at, kind=FixupKind.WORD, intern=None)
            self._pending.append((fx, target))
            self.fixups.append(fx)
        else:
            raise TypeError(f"vtable slot target must be str (label), "
                             f"int (abs VA), or (module, symbol) tuple; "
                             f"got {type(target)}")

    def add_code(self, label: str, words: list[int]) -> int:
        """Append raw ARM words under `label`. Returns the offset.

        Fixups internal to this code body must be added via .add_fixup().
        """
        at = self.cursor()
        self.bytes_.extend(arm_pack(*words))
        self._labels[label] = at
        self.exports[label] = at
        return at

    def add_fixup(self, fx: Fixup) -> None:
        self.fixups.append(fx)

    def finish(self) -> ClassBlob:
        # Resolve deferred label references
        for f, label_name in self._pending:
            if label_name not in self._labels:
                raise ValueError(f"unresolved label {label_name!r}")
            f.intern = self._labels[label_name]
        return ClassBlob(name=self.name,
                          instance_size=self.instance_size,
                          bytes_=self.bytes_,
                          exports=self.exports,
                          fixups=self.fixups)
