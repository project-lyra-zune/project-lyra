"""Host-side tests for modkit.capabilities.

Cross-checks the parse of the on-device CAPS / SUBSYSTEMS tables so drift in the source
layout, or a capability added/removed without the feed following, fails here.
"""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from modkit.capabilities import (
    platform_capabilities, platform_capability_ranges, _range_str,
    DECLARATIVE_CAPABILITIES)

_MANIFEST_C = (Path(__file__).resolve().parents[3]
               / "src" / "mod-runtime" / "mods_manifest.c")
# The lowering function that consumes each declarative field (mirrors lower_manifest_v2).
_LOWER_FN = {
    "settings":     "lower_settings",
    "status":       "lower_status",
    "status_icons": "lower_status_icons",
    "daemons":      "lower_daemons",
}
_SYNTH = re.compile(r'synth_action\s*\([^;]*?"(lyra\.[a-z_]+)"')


class PlatformCapabilitiesTests(unittest.TestCase):
    def setUp(self):
        self.caps = platform_capabilities()
        self.ranges = platform_capability_ranges()

    def test_wired_action_capabilities_present(self):
        for c in ("lyra.inject_settings_row", "lyra.register_setting", "lyra.add_status_icon",
                  "lyra.patch_bytes", "lyra.load_module", "lyra.gem_add_entry"):
            self.assertIn(c, self.ranges, c)

    def test_all_platform_names_namespaced(self):
        for name in self.ranges:
            self.assertTrue(name.startswith("lyra."), name)

    def test_subsystems_present(self):
        self.assertIn("lyra.wifi_awake", self.ranges)
        self.assertIn("lyra.volume_state", self.ranges)

    def test_sorted_and_deduped(self):
        self.assertEqual(self.caps, sorted(set(self.caps)))

    def test_nonempty(self):
        self.assertGreater(len(self.caps), 10)

    def test_baseline_ranges_are_1_1(self):
        # Every current capability is [1,1]; the range machinery is present but invisible.
        for name, (cur, min_compat) in self.ranges.items():
            self.assertEqual((cur, min_compat), (1, 1), name)

    def test_baseline_emits_bare_names(self):
        # At [1,1] a capability is a bare name; no '@' appears until a real advance.
        for c in self.caps:
            self.assertNotIn("@", c, c)

    def test_range_str_form(self):
        self.assertEqual(_range_str("x", 1, 1), "x")
        self.assertEqual(_range_str("x", 3, 1), "x@1:3")
        self.assertEqual(_range_str("x", 3, 2), "x@2:3")


def _static_fn_names(src: str) -> set[str]:
    return set(re.findall(r'^static\s+\w[\w\s\*]*?\b(\w+)\s*\(', src, re.M))


def _fn_body(src: str, name: str) -> str:
    """The brace-matched body of the definition of `name` (the occurrence whose
    parameter list is followed by '{', not a forward-decl or call site)."""
    for m in re.finditer(r'\b' + re.escape(name) + r'\s*\(', src):
        i = src.index('(', m.start())
        depth, j = 0, i
        while j < len(src):
            depth += (src[j] == '(') - (src[j] == ')')
            if depth == 0:
                break
            j += 1
        k = j + 1
        while k < len(src) and src[k] in ' \t\r\n':
            k += 1
        if k >= len(src) or src[k] != '{':
            continue
        depth, b = 0, k
        while b < len(src):
            depth += (src[b] == '{') - (src[b] == '}')
            if depth == 0:
                return src[k:b + 1]
            b += 1
    return ""


def _caps_reachable(src: str, name: str, fns: set[str], seen: set[str]) -> set[str]:
    """Every lyra.* synth_action capability emitted by `name`, following calls into
    same-file helpers (so status_icons reaches emit_status_visual's write_blob/register)."""
    if name in seen:
        return set()
    seen.add(name)
    body = _fn_body(src, name)
    caps = set(_SYNTH.findall(body))
    for other in fns:
        if other != name and re.search(r'\b' + re.escape(other) + r'\s*\(', body):
            caps |= _caps_reachable(src, other, fns, seen)
    return caps


class DeclarativeLoweringMirrorTests(unittest.TestCase):
    """Guard the host mirror of the on-device declarative lowering. The last regression in
    this area was a rename that missed synth_action; this fails if the C lowering and
    DECLARATIVE_CAPABILITIES drift apart."""

    def setUp(self):
        self.src = _MANIFEST_C.read_text("utf-8")
        self.fns = _static_fn_names(self.src)

    def test_lower_manifest_v2_dispatches_exactly_the_mapped_fields(self):
        body = _fn_body(self.src, "lower_manifest_v2")
        self.assertTrue(body)
        called = {f for f in self.fns if f.startswith("lower_")
                  and re.search(r'\b' + re.escape(f) + r'\s*\(', body)}
        self.assertEqual(called, set(_LOWER_FN.values()))

    def test_each_field_matches_the_c_lowering(self):
        for field, fn in _LOWER_FN.items():
            reached = _caps_reachable(self.src, fn, self.fns, set())
            self.assertEqual(reached, set(DECLARATIVE_CAPABILITIES[field]),
                             f"{field} -> {fn}")


if __name__ == "__main__":
    unittest.main()
