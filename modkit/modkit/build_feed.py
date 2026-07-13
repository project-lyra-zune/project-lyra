"""Generate the repo feed (`feed.json`) + `.zmod` packages.

Packages and hashes each mod into the feed: a flat, `ensure_ascii`, fixed-key-order
index the device parses with `strstr` (no on-device JSON parser). Output:
`<out>/feed.json` plus `<out>/mods/<id>-<ver>.zmod`.
"""
from __future__ import annotations
import json
from pathlib import Path

from .manifest import Mod, BROWSE_CATEGORIES
from .package import package_mod, package_lyra


def _entry(mod: Mod, raw: dict, pkg, base_url: str) -> dict:
    # Fixed key order; the device parser anchors on these literals in sequence. There
    # is no kind field: the platform is the reserved id `lyra` and reposd routes on it.
    return {
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
