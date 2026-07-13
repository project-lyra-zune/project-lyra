"""Firmware v4.5 (Pavo): external symbol address table for class-blob assembly.

Mod class blobs reference these symbols by NAME at author-time. The
assembler resolves names to absolute VAs at build-time using this table.
Phase 2 then applies plant-VA-relative fixups against absolute targets
that are already baked into the reloc manifest.

If we ever target multiple firmware versions, we'd add `firmware_v33_abi.py`
etc. and let the modkit pick based on a manifest hint.

All addresses are gemstone.exe VAs (image_base 0x00010000) except where
noted; device-derived by reverse engineering.
"""
from __future__ import annotations


# ─── Class registration ABI ─────────────────────────────────────────────
# Descriptor field constants
DESC_DESTRUCTOR        = 0x0002acf4   # descriptor +0x14 (registration-shared)
DESC_FINALIZER         = 0x0002a7ac   # descriptor +0x1c (registration-shared)
GEMBASESCENE_NAME_PTR  = 0x00011be0   # L"GemBaseScene" wstring in gemstone RO
XUI_REGISTER_CLASS     = 0x0006b39c   # gemstone thunk → xuidll!XuiRegisterClass

# ─── Class factory/ctor/vtable ABI ──────────────────────────────────────
# (per docs/{start,setting}-scene.md, RE'd live on-device)
ALLOCATOR              = 0x00084ee4   # coredll ord 1095, "operator new"-style
PARENT_VTABLE_BASE     = 0x0002aca0   # GemBaseScene base vtable (vtable +0x00)
CLASS_DESTROY_SHARED   = 0x0008678c   # canonical OnDestroy (vtable +0x0c)
ICON_VTABLE_SLOT2_STUB = 0x0002acec   # element-controller slot2 "mov r0,#0; bx lr"

# ─── XUI helpers ────────────────────────────────────────────────────────
FIND_ELEMENT_CHAIN     = 0x0006afec   # used by OnInit's FindElement wstring chain
ID_NAV_STUB            = 0x0001c284   # id-based scene navigation entry point

# ─── Symbol table (name → VA), used by the assembler ──────────────────
# gemstone.exe is the default host. Symbols that live in a HOST PROCESS's
# image (allocator, vtable slots, OnDestroy, slot2 stub) differ per host and
# are overridden in EXTERNS_BY_HOST below; xuidll-resident symbols are
# XIP-fixed (same VA in every process) and need no per-host entry.
EXTERNS: dict[str, int] = {
    "descriptor_dtor":          DESC_DESTRUCTOR,
    "descriptor_finalizer":     DESC_FINALIZER,
    "GemBaseScene_name_ptr":    GEMBASESCENE_NAME_PTR,
    "XuiRegisterClass_thunk":   XUI_REGISTER_CLASS,

    "allocator":                ALLOCATOR,
    "parent_vtable":            PARENT_VTABLE_BASE,
    "class_ondestroy_shared":   CLASS_DESTROY_SHARED,
    "icon_vtable_slot2_stub":   ICON_VTABLE_SLOT2_STUB,

    "find_element_chain":       FIND_ELEMENT_CHAIN,
    "id_nav_stub":              ID_NAV_STUB,
}

# Per-host overrides for symbols resident in a host process's own image.
# "servicesd" = zhud_serv.dll (base 0x419b0000), the HUD scene host. Values
# RE'd from zhud's ZunePlayIcon (ctor 0x419d5348 → vtable 0x419b4c48 record0).
# See project memory `project_zos_status_icon_gemstone_zhud_abi`.
EXTERNS_BY_HOST: dict[str, dict[str, int]] = {
    "gemstone": {
        "allocator":              ALLOCATOR,
        "parent_vtable":          PARENT_VTABLE_BASE,
        "class_ondestroy_shared": CLASS_DESTROY_SHARED,
        "icon_vtable_slot2_stub": ICON_VTABLE_SLOT2_STUB,
    },
    "servicesd": {
        "allocator":              0x419d833c,
        "parent_vtable":          0x419b9dd0,
        "class_ondestroy_shared": 0x419cddc4,
        "icon_vtable_slot2_stub": 0x419b9e1c,
    },
}


def lookup(name: str, host: str = "gemstone") -> int:
    host_tbl = EXTERNS_BY_HOST.get(host)
    if host_tbl is not None and name in host_tbl:
        return host_tbl[name]
    if name in EXTERNS:
        return EXTERNS[name]
    raise KeyError(f"unknown extern symbol: {name!r} for host {host!r} "
                   f"(known: {sorted(EXTERNS)})")
