"""The set of files a mod deploys to the device.

A mod ships its manifest plus every file the manifest and its scene sources
reference (via @/…, @asset/…, @mod/… tokens). Authoring-only files (.xui sources,
.psd/.svg art) are never referenced, so they are excluded by construction, not by
an extension blocklist. Single source of truth for both over-wire deploy
(deploy.py) and release packaging.
"""
from __future__ import annotations
import re
from pathlib import Path

from .manifest import Mod

# A file reference: @/p, @asset/p, or @mod/p. @all (element selector) and $refs
# are not files, so they do not match.
_FILE_TOKEN = re.compile(r"@(?:asset/|mod/|/)[^\s<\"']+")


def _resolve(token: str, mod: Mod) -> Path:
    if token.startswith("@asset/"):
        return mod.source_dir / "assets" / token[len("@asset/"):]
    if token.startswith("@mod/"):
        return mod.source_dir / token[len("@mod/"):]
    return mod.source_dir / token[2:]  # @/


def _expand(path: Path) -> list[Path]:
    """A file resolves to itself; a directory to every file under it (recursive)."""
    if path.is_file():
        return [path]
    if path.is_dir():
        return [f for f in path.rglob("*") if f.is_file()]
    return []


def deployable_files(mod: Mod) -> set[Path]:
    """Absolute paths of the files this mod deploys: manifest.json plus every
    referenced blob and asset. A token may name a file or a directory (shipped
    recursively). The whole manifest is scanned (not just actions[]) so tokens in
    declarative fields (status_icons, settings) are caught; .xui sources are
    scanned for the assets their scenes pull in."""
    manifest = mod.source_dir / "manifest.json"
    texts = [manifest.read_text(encoding="utf-8", errors="ignore")]
    texts += [x.read_text(encoding="utf-8", errors="ignore")
              for x in mod.source_dir.rglob("*.xui")]

    files = {f for text in texts for t in _FILE_TOKEN.findall(text)
             for f in _expand(_resolve(t, mod))}
    files.add(manifest)
    return files
