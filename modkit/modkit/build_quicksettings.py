#!/usr/bin/env python3
"""Build ModQuickSettingsScene: the HUD rich-row quick-settings list scene class.

A HUD scene class (parent HudActiveBaseScene), the rich-row replacement for the
flat ZuneContextMenu quick-toggle. Same blob shape as the gem-side
GemModsListContentScene (real OnInit binds the list, OnMessage answers the
msg=0xe data-source sub-codes), but built for servicesd/zhud:

  - host="servicesd"            → base VAs (allocator/parent_vtable/on_destroy)
                                   resolve to the zhud values, which match
                                   HudNetworkListScene's own factory + vtable.
  - indirect_allocator=True     → the zhud allocator (0x419d83…) is a FAR
                                   allocator (beyond ARM bl ±32MB from the
                                   planted scratch), so the factory reaches it
                                   via the pool+blx variant.
  - instance_size=0x17c         → mirrors HudNetworkListScene so the base's
                                   writes to +0x04..+0x178 stay inside our alloc.

Vtable slots 1/2 (OnMessage/OnInit) are extern_module fixups against zuxhook.dll,
resolved at plant time on device, so the blob does not need rebuilding after every
zuxhook rebuild. Run AFTER building zuxhook (validates the exports exist).
"""
from __future__ import annotations
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from modkit.classblob import ClassBlobBuilder
from modkit.classblob_apply import apply_fixups
from modkit.build_gemmod_manager import pe_export_rvas

ZUXHOOK_DLL = REPO / "src" / "zuxhook" / "bin" / "OpenZDK (ARMV4I)" / "Release" / "zuxhook.dll"
HOST = "servicesd"
INSTANCE_SIZE = 0x17c   # mirror HudNetworkListScene's instance


def build_blob():
    b = ClassBlobBuilder("ModQuickSettingsScene", instance_size=INSTANCE_SIZE, host=HOST)
    b.add_factory(ctor_label="ctor", indirect_allocator=True)
    b.add_ctor()
    b.add_vtable(on_message=("zuxhook.dll", "ModQuickSettingsScene_OnMessage"),
                 on_init=("zuxhook.dll", "ModQuickSettingsScene_OnInit"),
                 on_destroy="class_ondestroy_shared")
    return b.finish()


def main():
    if not ZUXHOOK_DLL.is_file():
        raise SystemExit(f"build zuxhook first: {ZUXHOOK_DLL} not found")
    exports = pe_export_rvas(ZUXHOOK_DLL)
    for nm in ("ModQuickSettingsScene_OnInit", "ModQuickSettingsScene_OnMessage"):
        if nm not in exports:
            raise SystemExit(f"missing export {nm}; rebuild zuxhook")
    print("exports ModQuickSettingsScene_OnInit/_OnMessage present")

    out_dir = REPO / "lyra" / "platform" / "mods-tab"
    blob = build_blob()
    _ = apply_fixups(blob.bytes_, blob.fixups, 0x000FF000)
    (out_dir / "class_quicksettings_zhud.bin").write_bytes(bytes(blob.bytes_))
    manifest = {
        "format": "zune-modkit-classblob/1",
        "name": blob.name,
        "host": HOST,
        "instance_size": blob.instance_size,
        "factory_offset": blob.exports["factory"],
        "exports": blob.exports,
        "fixups": [f.to_json() for f in blob.fixups],
    }
    (out_dir / "class_quicksettings_zhud.reloc.json").write_text(
        json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {out_dir / 'class_quicksettings_zhud.bin'} "
          f"({len(blob.bytes_)} bytes, {len(blob.fixups)} fixups)")


if __name__ == "__main__":
    main()
