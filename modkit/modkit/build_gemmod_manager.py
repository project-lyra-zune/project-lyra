#!/usr/bin/env python3
"""Build the mods-tab class blobs.

Validates that zuxhook.dll exports the four class entry points
(GemModHub_*, GemModManager_*, GemModsListContentScene_*, GemModDetail_*),
then emits four blobs whose vtable slots are extern_module fixups:

  lyra/platform/mods-tab/class_hub.bin      (GemModHub)
  lyra/platform/mods-tab/class.bin          (GemModManager: outer shell)
  lyra/platform/mods-tab/class_content.bin  (GemModsListContentScene: list content)
  lyra/platform/mods-tab/class_detail.bin   (GemModDetail: per-mod detail)

Vtable slots resolve at PLANT time on device via GetProcAddress on
zuxhook.dll, so blobs do NOT need rebuilding after every zuxhook
rebuild. The script reads the .dll only to verify the exports exist;
it does not bake any VAs, and does not query the device.

The shell+content split mirrors GemMarketplaceGamesScene +
GemMarketplaceGamesListContentScene + GemMarketplaceGamesDetailsContentScene.
Inter-mod-scene navigation
is name-based: the C++ classes call xuidll!XuiSceneCreate(L"gem://",
<scene>.xur, ...) + gemstone scene_navigate_wrapper directly, so no
scene-id table extension is needed.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from modkit.classblob import ClassBlobBuilder, Fixup, FixupKind
from modkit.classblob_apply import apply_fixups
from modkit.firmware_v45_abi import EXTERNS, lookup as resolve_extern


ZUXHOOK_DLL = REPO / "src" / "zuxhook" / "bin" / "OpenZDK (ARMV4I)" / "Release" / "zuxhook.dll"
ZUXHOOK_MODULE_NAME = "zuxhook.dll"


def pe_export_rvas(dll_path: Path) -> dict[str, int]:
    p = dll_path.read_bytes()
    e_lfanew = struct.unpack_from("<I", p, 0x3c)[0]
    coff = e_lfanew + 4
    opt  = coff + 20
    data_dir_off = opt + 96
    exp_rva, _ = struct.unpack_from("<II", p, data_dir_off)
    n_sec = struct.unpack_from("<H", p, coff + 2)[0]
    sect_off = opt + struct.unpack_from("<H", p, coff + 16)[0]
    sections = []
    for i in range(n_sec):
        s = sect_off + i * 40
        vsize, vaddr, _rsize, raddr = struct.unpack_from("<IIII", p, s + 8)
        sections.append((vaddr, vsize, raddr))

    def rva_to_off(rva: int) -> int:
        for va, vs, ra in sections:
            if va <= rva < va + vs:
                return rva - va + ra
        raise KeyError(f"RVA 0x{rva:x} unmapped")

    exp_off = rva_to_off(exp_rva)
    n_names = struct.unpack_from("<I", p, exp_off + 0x18)[0]
    addr_off = rva_to_off(struct.unpack_from("<I", p, exp_off + 0x1c)[0])
    name_off = rva_to_off(struct.unpack_from("<I", p, exp_off + 0x20)[0])
    ord_off  = rva_to_off(struct.unpack_from("<I", p, exp_off + 0x24)[0])
    out: dict[str, int] = {}
    for i in range(n_names):
        name_rva = struct.unpack_from("<I", p, name_off + i * 4)[0]
        ordinal  = struct.unpack_from("<H", p, ord_off + i * 2)[0]
        fn_rva   = struct.unpack_from("<I", p, addr_off + ordinal * 4)[0]
        o = rva_to_off(name_rva)
        name = ""
        while p[o] != 0:
            name += chr(p[o]); o += 1
        out[name] = fn_rva
    return out


# Instance-field offsets must match GemModManagerInstance in
# gem_mod_manager.cpp. Manager keeps breadcrumb at +0x08 so XuiScene
# base's msg=0xe/sub=1 dispatch finds it; all class-private state lives
# past +0x28 to avoid being trampled by base writes to +0x0c..+0x18.
MGR_OFF_INSTALLED_LABEL_ID = 0x48
MGR_OFF_ARCHIVED_LABEL_ID  = 0x4c
MGR_OFF_UPDATES_LABEL_ID   = 0x50


def _ext(symbol: str) -> tuple[str, str]:
    """Shorthand for an extern_module fixup against zuxhook.dll."""
    return (ZUXHOOK_MODULE_NAME, symbol)


def build_manager_blob(installed_label_id: int, archived_label_id: int,
                       updates_label_id: int):
    """GemModManager: outer shell. Instance layout uses GemModHub's
    device-validated shape (breadcrumb at +0x08 so XuiScene base
    tail-call gives us back-nav; class-private state past +0x28 so
    base writes to +0x0c..+0x18 don't trample it). instance_size=0x58."""
    b = ClassBlobBuilder("GemModManager", instance_size=0x58)
    b.add_factory(ctor_label="ctor")
    b.add_ctor(extra_init={
        MGR_OFF_INSTALLED_LABEL_ID: installed_label_id,
        MGR_OFF_ARCHIVED_LABEL_ID:  archived_label_id,
        MGR_OFF_UPDATES_LABEL_ID:   updates_label_id,
    })
    b.add_vtable(on_message=_ext("GemModManager_OnMessage"),
                  on_init=_ext("GemModManager_OnInit"),
                  on_destroy="class_ondestroy_shared")
    return b.finish()


def build_content_blob():
    """GemModsListContentScene: canonical sibling leaf list-content scene
    (same shape as GemLibraryListContentScene, GemMarketplaceGamesListContent
    Scene, GemInboxListContentScene). Loaded into GemModManager's frame
    when a twist tab commits; manager calls XuiSceneCreate with
    L"ManageModsContent.xur". instance_size=0x50."""
    b = ClassBlobBuilder("GemModsListContentScene", instance_size=0x50)
    b.add_factory(ctor_label="ctor")
    b.add_ctor(extra_init={})
    b.add_vtable(on_message=_ext("GemModsListContentScene_OnMessage"),
                  on_init=_ext("GemModsListContentScene_OnInit"),
                  on_destroy="class_ondestroy_shared")
    return b.finish()


def build_detail_blob():
    """GemModDetail: the unified per-mod detail scene (ManageModDetail.xur). Keyed
    by mod_id (set by the list before navigating), it looks the mod up in both
    ModScan and the reposd feed and shows state-appropriate actions. instance_size
    0x60: five buttons (enable / disable / delete / install / update) plus
    title/status/version/author/description and a local_idx, past the 0x50 the
    three-button version used."""
    b = ClassBlobBuilder("GemModDetail", instance_size=0x60)
    b.add_factory(ctor_label="ctor")
    b.add_ctor(extra_init={})
    b.add_vtable(on_message=_ext("GemModDetail_OnMessage"),
                  on_init=_ext("GemModDetail_OnInit"),
                  on_destroy="class_ondestroy_shared")
    return b.finish()


def build_browse_blob():
    """GemModBrowse: the Browse host, a category twist (BrowseMods.xur). Content-swap
    host like GemModManager (parent GemLibraryBaseScene). instance_size=0x50."""
    b = ClassBlobBuilder("GemModBrowse", instance_size=0x50)
    b.add_factory(ctor_label="ctor")
    b.add_ctor(extra_init={})
    b.add_vtable(on_message=_ext("GemModBrowse_OnMessage"),
                  on_init=_ext("GemModBrowse_OnInit"),
                  on_destroy="class_ondestroy_shared")
    return b.finish()


def build_browse_list_blob():
    """GemModBrowseList: a category's catalog list (BrowseModsContent.xur). Leaf list
    (parent GemBaseScene); a row tap opens the unified detail. instance_size=0x50."""
    b = ClassBlobBuilder("GemModBrowseList", instance_size=0x50)
    b.add_factory(ctor_label="ctor")
    b.add_ctor(extra_init={})
    b.add_vtable(on_message=_ext("GemModBrowseList_OnMessage"),
                  on_init=_ext("GemModBrowseList_OnInit"),
                  on_destroy="class_ondestroy_shared")
    return b.finish()


def build_hub_blob():
    """GemModHub: the mods landing hub. Button taps in OnMessage
    dispatch to scenes by name (XuiSceneCreate + scene_navigate)."""
    b = ClassBlobBuilder("GemModHub", instance_size=0x40)
    b.add_factory(ctor_label="ctor")
    b.add_ctor(extra_init={})
    b.add_vtable(on_message=_ext("GemModHub_OnMessage"),
                  on_init=_ext("GemModHub_OnInit"),
                  on_destroy="class_ondestroy_shared")
    return b.finish()


def main():
    import argparse
    ap = argparse.ArgumentParser(
        description="Rebuild lyra/platform/mods-tab/class*.bin + class*.reloc.json. "
                    "Vtable slots are extern_module fixups, resolved at "
                    "plant time on device; no device query needed, and "
                    "blobs do not require rebuilding after every zuxhook "
                    "rebuild.")
    # Strings.xus IDs assigned by Phase 1's xus_add_string in manifest order.
    # Existing claimedCount=637 → "mods" goes at 637, "installed" at 638,
    # "archived" at 639. Override if the manifest action ordering changes.
    # "updates" is NOT appended: the native Strings.xus already has an exact
    # lowercase "updates" at index 263, so the updates tab reuses it.
    ap.add_argument("--installed-label-id", type=lambda s: int(s, 0),
                    default=638,
                    help="Strings.xus index for the 'installed' tab label "
                         "(default 638, per manifest action order)")
    ap.add_argument("--archived-label-id", type=lambda s: int(s, 0),
                    default=639,
                    help="Strings.xus index for the 'archived' tab label "
                         "(default 639)")
    ap.add_argument("--updates-label-id", type=lambda s: int(s, 0),
                    default=263,
                    help="Strings.xus index for the 'updates' tab label "
                         "(default 263, the native lowercase 'updates' string)")
    args = ap.parse_args()

    # Validate that zuxhook.dll exports all the symbols we'll reference.
    # The actual VAs aren't baked into the blob (they're resolved on
    # device), but if a name is missing the device-side fixup pass would
    # fail at plant time. Catching that here keeps the failure local.
    print(f"validating exports in {ZUXHOOK_DLL.relative_to(REPO)}")
    exports = pe_export_rvas(ZUXHOOK_DLL)
    required = (
        "GemModManager_OnInit", "GemModManager_OnMessage",
        "GemModHub_OnInit", "GemModHub_OnMessage",
        "GemModsListContentScene_OnInit", "GemModsListContentScene_OnMessage",
        "GemModDetail_OnInit", "GemModDetail_OnMessage",
        "GemModBrowse_OnInit", "GemModBrowse_OnMessage",
        "GemModBrowseList_OnInit", "GemModBrowseList_OnMessage",
    )
    for nm in required:
        if nm not in exports:
            raise SystemExit(f"missing export: {nm}")
    print(f"  all {len(required)} required exports present")

    print()
    print("manager tab labels:")
    print(f"  installed_label_id = {args.installed_label_id}")
    print(f"  updates_label_id   = {args.updates_label_id}")
    print(f"  archived_label_id  = {args.archived_label_id}")

    out_dir = REPO / "lyra" / "platform" / "mods-tab"
    import json

    def _write_named(blob, bin_name, reloc_name):
        # Host-side pre-applier skips extern_module fixups; it still
        # validates intern + abs fixups can be resolved cleanly.
        _ = apply_fixups(blob.bytes_, blob.fixups, 0x000FF000)
        bin_path = out_dir / bin_name
        reloc_path = out_dir / reloc_name
        bin_path.write_bytes(bytes(blob.bytes_))
        manifest_dict = {
            "format": "zune-modkit-classblob/1",
            "name": blob.name,
            "instance_size": blob.instance_size,
            "factory_offset": blob.exports["factory"],
            "exports": blob.exports,
            "fixups": [f.to_json() for f in blob.fixups],
        }
        reloc_path.write_text(json.dumps(manifest_dict, indent=2) + "\n")
        print(f"wrote {bin_path} ({len(blob.bytes_)} bytes, "
              f"{len(blob.fixups)} fixups, {blob.name})")

    print()

    # GemModManager → class.bin (outer shell)
    mgr_blob = build_manager_blob(installed_label_id=args.installed_label_id,
                                    archived_label_id=args.archived_label_id,
                                    updates_label_id=args.updates_label_id)
    _ = apply_fixups(mgr_blob.bytes_, mgr_blob.fixups, 0x000FF000)
    mgr_blob.write(out_dir)
    print(f"wrote {out_dir / 'class.bin'} ({len(mgr_blob.bytes_)} bytes, "
          f"{len(mgr_blob.fixups)} fixups, GemModManager)")

    # GemModHub → class_hub.bin
    hub_blob = build_hub_blob()
    _write_named(hub_blob, "class_hub.bin", "class_hub.reloc.json")

    # GemModsListContentScene → class_content.bin
    content_blob = build_content_blob()
    _write_named(content_blob, "class_content.bin", "class_content.reloc.json")

    # GemModDetail → class_detail.bin
    detail_blob = build_detail_blob()
    _write_named(detail_blob, "class_detail.bin", "class_detail.reloc.json")

    # GemModBrowse → class_browse.bin
    browse_blob = build_browse_blob()
    _write_named(browse_blob, "class_browse.bin", "class_browse.reloc.json")

    # GemModBrowseList → class_browse_list.bin
    browse_list_blob = build_browse_list_blob()
    _write_named(browse_list_blob, "class_browse_list.bin", "class_browse_list.reloc.json")


if __name__ == "__main__":
    main()
