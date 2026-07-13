"""Package a built mod into a distributable `.zmod` (standard ZIP).

Zips exactly `payload.deployable_files(mod)` (the set `deploy.py` mirrors to the
device), pathed to the on-device layout, after building `.xui`->`.xur`. Deterministic
so an unchanged mod hashes identically.
"""
from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
import hashlib
import json
import zipfile

from .manifest import Mod
from .build import build_mod
from .payload import deployable_files
from .validate import structural_check

_REPO = Path(__file__).resolve().parents[2]


def _require_valid(mod: Mod) -> None:
    """Refuse to package a mod whose manifest fails the same structural check
    `validate` runs, so the feed can never publish what `validate` rejects."""
    raw = json.loads((mod.source_dir / "manifest.json").read_text("utf-8"))
    errs = structural_check(mod, raw)
    if errs:
        raise ValueError(
            f"{mod.mod_id}: manifest fails validation:\n  " + "\n  ".join(errs))
# Phase 1 output layout: each platform binary lands at src/<component>/<_BIN>/<file>,
# where the component dir is the binary's stem (zuxhook.dll -> src/zuxhook, ...). The
# same paths build-release.sh passes as --zuxhook / --nativeapp.
_PLATFORM_BIN = "bin/OpenZDK (ARMV4I)/Release"


def _platform_file_source(name: str) -> Path:
    return _REPO / "src" / Path(name).stem / _PLATFORM_BIN / name


@dataclass
class Package:
    mod_id: str
    version: str
    path: Path        # the written .zmod
    size: int         # byte length of the .zmod
    sha256: str       # hex digest of the .zmod bytes


def package_mod(mod: Mod, out_dir: Path, *, build: bool = True,
                emit=print) -> Package:
    """Build the mod (unless build=False) and write
    `<mod_id>-<version>.zmod` into out_dir. Returns its size + sha256 for the
    feed. Entries are pathed relative to the mod dir (the on-device layout)."""
    _require_valid(mod)
    if build:
        build_mod(mod, emit=emit)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    zpath = out_dir / f"{mod.mod_id}-{mod.version}.zmod"

    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(deployable_files(mod)):
            arc = f.relative_to(mod.source_dir).as_posix()
            _zip_write(z, arc, f.read_bytes(), emit)

    data = zpath.read_bytes()
    return Package(mod.mod_id, mod.version, zpath, len(data),
                   hashlib.sha256(data).hexdigest())


def _zip_write(z: zipfile.ZipFile, arc: str, data: bytes, emit) -> None:
    info = zipfile.ZipInfo(arc, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)
    emit(f"    + {arc}")


def package_lyra(mod: Mod, out_dir: Path, *, build: bool = True,
                 emit=print) -> Package:
    """Package the `lyra` platform bundle as `lyra-<version>.zmod`.

    Unlike a mod .zmod (rooted at the mod dir, extracted into
    \\flash2\\automation\\mods\\<id>\\), this is rooted at \\flash2\\automation\\
    itself: platform binaries at the top level, each component mod (the dirs under
    lyra/platform/) under platform/<id>/, plus a lyra.json version marker. reposd's
    platform-apply extracts it into the automation root with the same primitive.
    """
    _require_valid(mod)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    zpath = out_dir / f"{mod.mod_id}-{mod.version}.zmod"

    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        # Platform binaries -> \flash2\automation\<file>, read from their Phase 1 build
        # output (not staged copies in lyra/, which stays pure source: just manifest.json).
        for name in sorted(mod.platform_files):
            src = _platform_file_source(name)
            if not src.is_file():
                raise FileNotFoundError(
                    f"lyra platform_file {name!r} not built at {src}; "
                    f"run the Phase 1 build first")
            _zip_write(z, name, src.read_bytes(), emit)
        # Component mods -> \flash2\automation\platform\<id>\... The platform's components
        # are exactly the dirs under lyra/platform/ (structural, no manifest list).
        platform_dir = mod.source_dir / "platform"
        for comp_dir in sorted(p for p in platform_dir.iterdir()
                               if (p / "manifest.json").is_file()):
            component = Mod.from_dir(comp_dir)
            _require_valid(component)
            if build:
                build_mod(component, emit=emit)
            for f in sorted(deployable_files(component)):
                arc = f"platform/{component.mod_id}/" + f.relative_to(component.source_dir).as_posix()
                _zip_write(z, arc, f.read_bytes(), emit)
        # \flash2\automation\lyra.json: the platform's self-describing marker, shipped
        # verbatim from lyra/manifest.json (its committed source, the same role a mod's
        # manifest.json plays). reposd reads the version; the scanner reads
        # name/author/description so the Lyra row shows full metadata without a feed entry.
        _zip_write(z, "lyra.json", (mod.source_dir / "manifest.json").read_bytes(), emit)

    data = zpath.read_bytes()
    return Package(mod.mod_id, mod.version, zpath, len(data),
                   hashlib.sha256(data).hexdigest())
