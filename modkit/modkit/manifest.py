"""Manifest parsing + back-reference resolution for modkit.

A mod's directory contains:
  manifest.json           the action declarations
  <blob files>            referenced via "@/path/inside/mod"

Manifest schema (per the design doc):
  {
    "mod_id":     "...",
    "name":       "...",
    "version":    "...",
    "depends_on": [mod_id, ...],
    "actions":    [ { "type": "<capability_name>", ...action_args... }, ...]
  }

Action argument values may contain back-references:
  "@/path"       resolves to absolute path of a file inside the mod dir;
                 the action handler receives a Path object
  "$name"        resolves to a value previously assigned by an action
                 with "manifest_back_reference": "name"
"""
from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional
import json
import re


_BACK_REF_RE = re.compile(
    r"^\$([A-Za-z_][A-Za-z0-9_]*)"           # name
    r"(?:\s*([+\-])\s*(0x[0-9a-fA-F]+|\d+))?" # optional +/- offset (hex or dec)
    r"$"
)


class ManifestError(Exception):
    pass


# The one platform is the reserved id `lyra`. Identity + on-disk location are the
# whole taxonomy: a dir in mods\ is a feature mod, the platform's platform mods
# (mods-tab, wifi-power) live in platform\, and this id is the platform itself.
LYRA_PLATFORM_ID = "lyra"

# The feature-category vocabulary. Mirrored by the
# fixed Browse tabs in gem_mod_browse.cpp (CATS[], minus the "all" tab). build_feed
# rejects a feature mod whose category isn't one of these, so a mod can never ship with a
# category the Browse UI has no tab for. Matched case-insensitively. The platform (`lyra`,
# category Platform) is exempt; platform component mods aren't feed entries.
BROWSE_CATEGORIES = ("Appearance", "Media", "Network", "Utilities")


@dataclass
class Action:
    """One declared action inside a mod's manifest."""
    type: str                              # capability name (e.g. "patch_bytes")
    args: dict[str, Any]                   # raw args dict from manifest
    back_ref: Optional[str] = None         # if set, action result registered under this name
    mod_id: str = ""                       # filled in by Mod.from_dir
    index: int = 0                         # position within the mod's action list


@dataclass
class Mod:
    """A parsed, loaded mod (manifest + blob source paths)."""
    mod_id: str
    name: str
    version: str
    depends_on: list[str]
    actions: list[Action]                    # authored actions (declarative fields lowered on-device)
    source_dir: Path                        # filesystem path the mod was loaded from
    platform_files: list[str] = None         # lyra only: binaries shipped to \flash2\automation\ root
    experimental: bool = False               # author-declared unfinished; published with the flag set

    @property
    def is_platform(self) -> bool:
        """The one platform, the reserved id `lyra`: the platform binaries plus its
        component mods (the dirs under lyra/platform/), versioned and updated as one
        entity."""
        return self.mod_id == LYRA_PLATFORM_ID

    @staticmethod
    def from_dir(mod_dir: Path) -> "Mod":
        """Load a mod from its source directory."""
        manifest_path = mod_dir / "manifest.json"
        if not manifest_path.is_file():
            raise ManifestError(f"no manifest.json in {mod_dir}")

        try:
            data = json.loads(manifest_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            raise ManifestError(f"{manifest_path}: {e}")

        mod_id = data.get("mod_id")
        if not mod_id:
            raise ManifestError(f"{manifest_path}: missing 'mod_id'")

        actions_raw = data.get("actions") or []
        actions: list[Action] = []
        for i, raw in enumerate(actions_raw):
            if "type" not in raw:
                raise ManifestError(
                    f"{manifest_path}: action[{i}] missing 'type'")
            args = {k: v for k, v in raw.items()
                    if k not in ("type", "manifest_back_reference")}
            actions.append(Action(
                type=raw["type"],
                args=args,
                back_ref=raw.get("manifest_back_reference"),
                mod_id=mod_id,
                index=i,
            ))

        # Authored actions[] pass through verbatim. Declarative v2 fields
        # (requires/provides/settings/status_icons/daemons) are lowered into
        # capability actions by zuxhook's C runtime on-device, not here.
        return Mod(
            mod_id=mod_id,
            name=data.get("name", mod_id),
            version=data.get("version", "0.0.0"),
            depends_on=data.get("depends_on") or [],
            actions=actions,
            source_dir=mod_dir.resolve(),
            platform_files=data.get("platform_files") or [],
            experimental=bool(data.get("experimental", False)),
        )


def resolve_blob_ref(value: Any, mod: Mod) -> Any:
    """If `value` is a "@/path" string, return Path(mod.source_dir / path).
    Otherwise return value unchanged."""
    if isinstance(value, str) and value.startswith("@/"):
        rel = value[2:]
        full = mod.source_dir / rel
        if not full.is_file():
            raise ManifestError(
                f"{mod.mod_id}: blob ref {value!r} → {full} not found")
        return full
    return value


def resolve_back_ref(value: Any, scope: dict[str, Any], *,
                      strict: bool = True) -> Any:
    """Resolve a back-reference string against `scope`.

    Forms accepted:
      "$name"          → scope[name]
      "$name + 0xN"    → scope[name] + N  (hex offset)
      "$name + N"      → scope[name] + N  (decimal offset)
      "$name - 0xN"    → scope[name] - N
      "$name - N"      → scope[name] - N
    Whitespace around the operator is optional.

    Non-back-ref values pass through unchanged. With strict=False, an
    unknown name passes through as the original string so validation can
    run before apply-order has assigned it.

    Arithmetic requires the resolved base to be an int (the common case
    for kernel/heap VAs). Non-int bases with an offset raise.
    """
    if not (isinstance(value, str) and value.startswith("$")):
        return value
    m = _BACK_REF_RE.match(value)
    if m is None:
        raise ManifestError(f"malformed back-ref: {value!r}")
    name, op, offset_str = m.group(1), m.group(2), m.group(3)
    if name not in scope:
        if strict:
            raise ManifestError(f"unresolved back-ref: ${name}")
        return value   # leave full expression in place
    base = scope[name]
    if op is None:
        return base
    offset = int(offset_str, 16) if offset_str.startswith("0x") else int(offset_str)
    if not isinstance(base, int):
        raise ManifestError(
            f"back-ref arithmetic requires int base, got {type(base).__name__} "
            f"for ${name} in {value!r}")
    return base + offset if op == "+" else base - offset


def resolve_args(action: Action, mod: Mod, scope: dict[str, Any],
                  *, strict: bool = True) -> dict[str, Any]:
    """Walk action.args and resolve all blob/back refs.

    Returns a new dict with leaf strings starting with "@/" replaced by
    Path objects, and leaf strings starting with "$" replaced by the
    corresponding scope value. With strict=False (used during validation
    pass), unknown $refs pass through unresolved.
    """
    def walk(v):
        if isinstance(v, dict):
            return {k: walk(val) for k, val in v.items()}
        if isinstance(v, list):
            return [walk(x) for x in v]
        if isinstance(v, str):
            v = resolve_blob_ref(v, mod)
            if isinstance(v, str):
                v = resolve_back_ref(v, scope, strict=strict)
            return v
        return v
    return walk(action.args)
