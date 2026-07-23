"""The platform's advertised capability set, parsed from the on-device C source so the
feed's `lyra` entry lists exactly what the packaged zuxhook provides.

The `CAPS` table in `src/mod-runtime/mods_manifest.c` (wired action capabilities, each with a
`[min_compat, cur]` window) and the `SUBSYSTEMS` table in `src/zuxhook/phase2/mods_phase2.c`
(each [1,1]). Each capability is emitted in the range form (`name` at `[1,1]`, else
`name@min_compat:cur`). The device gate checks a mod's footprint against this set;
`test_capabilities.py` cross-checks the parse against the source.
"""
from __future__ import annotations
import re
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
_CAPS_SRC = _REPO / "src" / "mod-runtime" / "mods_manifest.c"
_SUBS_SRC = _REPO / "src" / "zuxhook" / "phase2" / "mods_phase2.c"

_CAPS_BLOCK = re.compile(r"CAPS\[\]\s*=\s*\{(.*?)\};", re.S)
_CAPS_ENTRY = re.compile(r'\{\s*"([^"]+)"\s*,\s*\d+\s*,\s*(\d+)\s*,\s*(\d+)\s*\}')
_SUBS_BLOCK = re.compile(r"SUBSYSTEMS\[\]\s*=\s*\{(.*?)\};", re.S)
_SUBS_ENTRY = re.compile(r'\{\s*"([^"]+)"\s*,')


def _block(path: Path, block_re: re.Pattern) -> str:
    m = block_re.search(path.read_text("utf-8"))
    if not m:
        raise ValueError(f"{path.name}: capability table not found (source layout changed?)")
    return m.group(1)


def _range_str(name: str, cur: int, min_compat: int) -> str:
    return name if (cur == 1 and min_compat == 1) else f"{name}@{min_compat}:{cur}"


def platform_capability_ranges() -> dict[str, tuple[int, int]]:
    """Each advertised capability -> (cur, min_compat). A CAPS entry with cur 0 is not
    advertised and is excluded; subsystems are [1,1]."""
    out: dict[str, tuple[int, int]] = {}
    for name, cur, min_compat in _CAPS_ENTRY.findall(_block(_CAPS_SRC, _CAPS_BLOCK)):
        c, m = int(cur), int(min_compat)
        if c >= 1:
            out[name] = (c, m)
    for name in _SUBS_ENTRY.findall(_block(_SUBS_SRC, _SUBS_BLOCK)):
        out.setdefault(name, (1, 1))
    return out


def platform_capabilities() -> list[str]:
    """Every advertised capability in range form, sorted."""
    return sorted(_range_str(n, c, m) for n, (c, m) in platform_capability_ranges().items())


# The lyra.* capabilities each declarative manifest field lowers to on-device, mirroring
# synth_action in src/mod-runtime/mods_manifest.c (lower_settings/status/status_icons/daemons).
# An author writes the field, not the explicit action, so a footprint must count these.
# test_capabilities cross-checks this against the C lowering, so drift there fails a test.
DECLARATIVE_CAPABILITIES: dict[str, tuple[str, ...]] = {
    "settings":     ("lyra.register_setting",),
    "status":       ("lyra.register_status",),
    "status_icons": ("lyra.gem_add_entry_bytes", "lyra.add_status_icon",
                     "lyra.write_blob_bytes", "lyra.register_visuals"),
    "daemons":      ("lyra.spawn_daemon",),
}
