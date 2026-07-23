"""Generate the repo feed (`feed.json`) + `.zmod` packages.

Packages and hashes each mod into a flat, `ensure_ascii` feed the device parses with
its JSON tokenizer (reposd/repo_feed.c, keyed by name, order-independent). Output:
`<out>/feed.json` plus `<out>/mods/<id>-<ver>.zmod`.
"""
from __future__ import annotations
import json
from pathlib import Path

from .manifest import Mod, BROWSE_CATEGORIES
from .package import package_mod, package_lyra
from .capabilities import (
    platform_capabilities, platform_capability_ranges, DECLARATIVE_CAPABILITIES)


def _split_point(s: str) -> tuple[str, int]:
    """A point capability string ('name' or 'name@r') into (name, revision). Bare is 1."""
    name, _, suf = s.partition("@")
    head = suf.split(":")[0]
    return (name, int(head)) if head.isdigit() and int(head) >= 1 else (name, 1)


def _capability_footprint(raw: dict) -> list[str]:
    """The platform capabilities a feature mod uses, as points, computed from its own
    manifest: every explicit action type; the capabilities each declarative field lowers to
    on-device (DECLARATIVE_CAPABILITIES); every subsystem a setting holds; and its top-level
    `requires` (mod dependencies and binary-reached capabilities). A platform capability is
    stamped with the platform's current revision at build time, so the device's min_compat
    floor can gate it; a mod-dependency keeps its author-declared revision. Entries merge per
    name to the higher revision, so a capability is never listed twice. Lyra checks this
    footprint against what it provides, so a mod declares what it does, never what it needs."""
    ranges = platform_capability_ranges()
    req: dict[str, int] = {}

    def note(s: str):
        name, rev = _split_point(s)
        req[name] = max(req.get(name, 1), rev)

    for a in raw.get("actions") or []:
        if a.get("type"):
            note(a["type"])
    for field, caps in DECLARATIVE_CAPABILITIES.items():
        if raw.get(field):
            for c in caps:
                note(c)
    for s in raw.get("settings") or []:
        for h in (s.get("holds") or []):
            note(h)
    for c in raw.get("requires") or []:
        note(c)

    out = []
    for name in sorted(req):
        rev = req[name]
        cr = ranges.get(name)
        if cr:
            rev = max(rev, cr[0])   # platform cap: stamp the current cur
        out.append(name if rev == 1 else f"{name}@{rev}")
    return out


def _entry(mod: Mod, raw: dict, pkg, base_url: str) -> dict:
    # Keyed by name on-device (reposd parses JSON, order-independent). No kind field:
    # the platform is the reserved id `lyra` and reposd routes on it.
    entry = {
        "mod_id": mod.mod_id,
        "name": mod.name,
        "version": mod.version,
        "author": raw.get("author", ""),
        "category": raw.get("category", "Other"),
        "description": raw.get("description", ""),
        "experimental": mod.experimental,
        "changelog": raw.get("changelog", ""),
        "size": pkg.size,
        "sha256": pkg.sha256,
        "url": f"{base_url.rstrip('/')}/mods/{pkg.path.name}",
        "art_url": raw.get("art_url", ""),
    }
    if mod.is_platform:
        # The platform advertises the capabilities it provides, from the on-device CAPS
        # source. The device gate checks a mod's footprint against this.
        entry["provides"] = platform_capabilities()
    else:
        # A feature mod carries its capability footprint (what it uses) and any capabilities
        # it provides to other mods, so the device can resolve mod-to-mod dependencies from
        # the feed. Omit either when empty.
        uses = _capability_footprint(raw)
        if uses:
            entry["uses"] = uses
        provides = raw.get("provides") or []
        if provides:
            entry["provides"] = provides
    return entry


def build_feed(mod_dirs, out_dir, base_url, *, build: bool = True,
               emit=print) -> Path:
    out_dir = Path(out_dir)
    pkg_dir = out_dir / "mods"
    entries = []
    for d in mod_dirs:
        mod = Mod.from_dir(Path(d))
        raw = json.loads((mod.source_dir / "manifest.json").read_text("utf-8"))
        emit(f"== Package {mod.mod_id} {mod.version} ==")
        if mod.is_platform:
            pkg = package_lyra(mod, pkg_dir, build=build, emit=emit)
        else:
            # A feature mod must declare a Browse category the UI has a tab for.
            cat = (raw.get("category") or "").strip()
            if cat.lower() not in {c.lower() for c in BROWSE_CATEGORIES}:
                raise ValueError(
                    f"{mod.mod_id}: category {cat or '(missing)'!r} is not a Browse "
                    f"category (one of {list(BROWSE_CATEGORIES)})")
            pkg = package_mod(mod, pkg_dir, build=build, emit=emit)
        entries.append(_entry(mod, raw, pkg, base_url))
        emit(f"    -> {pkg.path.name}  {pkg.size} B  sha256={pkg.sha256[:16]}…")

    feed_path = out_dir / "feed.json"
    feed_path.write_text(
        json.dumps({"feed_version": 1, "mods": entries},
                   indent=2, ensure_ascii=True),
        encoding="utf-8")
    emit(f"\nWrote {feed_path}  ({len(entries)} mod(s))")
    return feed_path
