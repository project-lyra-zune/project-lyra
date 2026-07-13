"""Host-side tests for the feed entry shape (modkit.build_feed._entry).

No device, no packaging: exercises the entry builder directly.
Run: `python3 -m unittest discover modkit/modkit/tests`.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from modkit.manifest import Mod
from modkit.package import Package
from modkit.build_feed import _entry


def _mod(td, **over):
    data = {"mod_id": "m", "name": "M", "version": "1.0.0", "author": "A",
            "category": "Media", "description": "d", **over}
    p = Path(td)
    (p / "manifest.json").write_text(json.dumps(data), encoding="utf-8")
    return Mod.from_dir(p), data


class FeedEntryTests(unittest.TestCase):
    def _entry_for(self, **over):
        with tempfile.TemporaryDirectory() as td:
            mod, raw = _mod(td, **over)
            pkg = Package(mod.mod_id, mod.version, Path("m-1.0.0.zmod"), 42, "deadbeef")
            return _entry(mod, raw, pkg, "https://repo.example")

    def test_experimental_true_carried_in_entry(self):
        self.assertTrue(self._entry_for(experimental=True)["experimental"])

    def test_experimental_defaults_false(self):
        self.assertFalse(self._entry_for()["experimental"])

    def test_entry_shape(self):
        e = self._entry_for()
        self.assertEqual(e["url"], "https://repo.example/mods/m-1.0.0.zmod")
        for k in ("mod_id", "experimental", "sha256", "size"):
            self.assertIn(k, e)
        self.assertNotIn("privileged", e)


if __name__ == "__main__":
    unittest.main()
