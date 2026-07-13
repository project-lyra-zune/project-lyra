"""Mod build step: compile each `.xui` source to `.xur`, expanding deploy-path tokens.

Mods author portable path tokens in their `.xui` sources; this step resolves
them to the fixed on-device install path and runs XUIHelper to produce the
`.xur` the manifest deploys. Mod source stays location-agnostic; the build
owns the device layout. Run before `apply` (and, later, at `.zmod` package time).

The install root is fixed by convention: `apply._deploy_mod_dirs` always mirrors
a mod to `\\flash2\\automation\\mods\\<mod_id>\\`, so the absolute path is known
at build time from the manifest's `mod_id` alone.

Tokens (expanded to `file://`-qualified device paths, CE backslash form):
  @asset/<p>  ->  file://\\flash2\\automation\\mods\\<mod_id>\\assets\\<p>
  @mod/<p>    ->  file://\\flash2\\automation\\mods\\<mod_id>\\<p>
"""
from __future__ import annotations
import re
import subprocess
import tempfile
from pathlib import Path

from .manifest import Mod

_REPO = Path(__file__).resolve().parents[2]
_XUIHELPER = (_REPO / "modkit/xuihelper-zune/XUIHelper.CLI"
              / "bin/Release/net8.0/XUIHelper.CLI.dll")
_MOD_ROOT = r"\flash2\automation\mods"
_TOKEN = re.compile(r"@(asset|mod)/([^\s<\"']+)")


def expand_tokens(text: str, mod_id: str) -> tuple[str, int]:
    base = f"{_MOD_ROOT}\\{mod_id}"

    def repl(m: re.Match) -> str:
        kind, rel = m.group(1), m.group(2).replace("/", "\\")
        root = f"{base}\\assets" if kind == "asset" else base
        return f"file://{root}\\{rel}"

    return _TOKEN.subn(repl, text)


def build_mod(mod: Mod, *, emit=print) -> list[Path]:
    """Compile every `.xui` under the mod to a sibling `.xur`, expanding path
    tokens against the mod's fixed device install path. Returns the `.xur` written."""
    if not _XUIHELPER.exists():
        raise FileNotFoundError(
            f"XUIHelper CLI not built at {_XUIHELPER}; build it (Release) first")

    written: list[Path] = []
    for xui in sorted(mod.source_dir.rglob("*.xui")):
        expanded, n = expand_tokens(xui.read_text(), mod.mod_id)
        xur = xui.with_suffix(".xur")
        # XUIHelper rejects relative -o paths; feed it the expanded source via a
        # temp file and an absolute output path.
        with tempfile.NamedTemporaryFile("w", suffix=".xui", delete=False) as tf:
            tf.write(expanded)
            tmp = Path(tf.name)
        try:
            r = subprocess.run(
                ["dotnet", str(_XUIHELPER), "conv", "-s", str(tmp),
                 "-f", "xurv5", "-o", str(xur.resolve()), "-g", "V5"],
                capture_output=True, text=True)
        finally:
            tmp.unlink(missing_ok=True)
        if r.returncode != 0 or not xur.exists():
            raise RuntimeError(
                f"build {xui.name}: conv failed: "
                f"{(r.stdout + r.stderr).strip()}")
        emit(f"    built {xui.relative_to(mod.source_dir)} -> {xur.name}"
             + (f"  ({n} token{'s' if n != 1 else ''} expanded)" if n else ""))
        written.append(xur)
    return written
