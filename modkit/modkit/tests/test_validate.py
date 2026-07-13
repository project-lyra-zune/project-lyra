"""Host-side tests for modkit.validate.structural_check.

Manifest validation is a pure host concern, no device required.
Run directly: `python3 modkit/modkit/tests/test_validate.py`, or with the
package suite: `python3 -m unittest discover modkit/modkit/tests`.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from modkit.manifest import Mod
from modkit.validate import structural_check, MAX_DESCRIPTION_LEN

_REPO = Path(__file__).resolve().parents[3]

_BASE = {
    "mod_id": "sample",
    "name": "Sample",
    "version": "1.0.0",
    "author": "Test",
    "category": "Utilities",
    "description": "A short description.",
}


def _check(overrides: dict) -> list[str]:
    """Build a Mod from _BASE + overrides in a temp dir and return its errors."""
    data = {**_BASE, **overrides}
    with tempfile.TemporaryDirectory() as td:
        p = Path(td)
        (p / "manifest.json").write_text(json.dumps(data), encoding="utf-8")
        mod = Mod.from_dir(p)
    return structural_check(mod, data)


class StructuralCheckTests(unittest.TestCase):
    def test_minimal_valid_mod_passes(self):
        self.assertEqual(_check({}), [])

    def test_description_over_limit_fails(self):
        errs = _check({"description": "x" * (MAX_DESCRIPTION_LEN + 1)})
        self.assertTrue(any("description" in e for e in errs), errs)

    def test_description_at_limit_passes(self):
        self.assertEqual(_check({"description": "x" * MAX_DESCRIPTION_LEN}), [])

    def test_unknown_top_level_key_fails(self):
        errs = _check({"kind": "feature"})
        self.assertTrue(any("unknown top-level key 'kind'" in e for e in errs), errs)

    def test_reserved_lyra_id_without_platform_files_fails(self):
        errs = _check({"mod_id": "lyra"})
        self.assertTrue(any("reserved platform id" in e for e in errs), errs)

    def test_experimental_must_be_bool(self):
        self.assertEqual(_check({"experimental": True}), [])
        self.assertEqual(_check({"experimental": False}), [])
        errs = _check({"experimental": "yes"})
        self.assertTrue(any("'experimental' must be a boolean" in e for e in errs), errs)

    def test_experimental_parses_onto_mod(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td)
            (p / "manifest.json").write_text(
                json.dumps({**_BASE, "experimental": True}), encoding="utf-8")
            self.assertTrue(Mod.from_dir(p).experimental)

    def test_duplicate_setting_id_fails(self):
        errs = _check({"settings": [
            {"id": "x", "type": "bool"}, {"id": "x", "type": "bool"}]})
        self.assertTrue(any("duplicate id 'x'" in e for e in errs), errs)

    def test_status_icon_source_must_resolve(self):
        errs = _check({"status_icons": [
            {"source": "setting/missing", "frames": ["@/a.png"], "image": "@/a.png"}]})
        self.assertTrue(any("no matching setting" in e for e in errs), errs)


class RepoManifestsTests(unittest.TestCase):
    """Every shipped manifest must pass the same check the packager enforces, so
    the feed can never publish a mod that `validate` rejects."""

    def _dirs(self):
        dirs = [d for d in sorted((_REPO / "mods").iterdir())
                if (d / "manifest.json").is_file()]
        dirs += [d for d in sorted((_REPO / "lyra" / "platform").iterdir())
                 if (d / "manifest.json").is_file()]
        dirs.append(_REPO / "lyra")   # the platform manifest itself
        return dirs

    def test_all_shipped_manifests_valid(self):
        for d in self._dirs():
            with self.subTest(mod=d.name):
                raw = json.loads((d / "manifest.json").read_text("utf-8"))
                self.assertEqual(structural_check(Mod.from_dir(d), raw), [])

    def test_youtube_marked_experimental(self):
        self.assertTrue(Mod.from_dir(_REPO / "mods" / "youtube").experimental)


if __name__ == "__main__":
    unittest.main()
