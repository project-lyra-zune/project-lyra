"""Host-side tests for the feed entry shape (modkit.build_feed._entry).

No device, no packaging: exercises the entry builder directly.
Run: `python3 -m unittest discover modkit/modkit/tests`.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

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

    def test_no_built_against(self):
        self.assertNotIn("built_against", self._entry_for())

    def test_no_uses_when_nothing_used(self):
        self.assertNotIn("uses", self._entry_for())

    def test_platform_cap_stamped_with_cur(self):
        # A platform cap at cur>1 is stamped with its current revision at build time.
        with patch("modkit.build_feed.platform_capability_ranges",
                   return_value={"lyra.inject_settings_row": (3, 1)}):
            e = self._entry_for(actions=[{"type": "lyra.inject_settings_row"}])
        self.assertEqual(e["uses"], ["lyra.inject_settings_row@3"])

    def test_footprint_merges_duplicate_name(self):
        # The same capability from an action and from requires collapses to one entry.
        e = self._entry_for(actions=[{"type": "lyra.patch_bytes"}],
                            requires=["lyra.patch_bytes"])
        self.assertEqual(e["uses"], ["lyra.patch_bytes"])

    def test_mod_dependency_keeps_author_revision(self):
        # A non-platform (mod-dependency) capability keeps its author-declared revision.
        e = self._entry_for(requires=["fx.reverb@2"])
        self.assertEqual(e["uses"], ["fx.reverb@2"])

    def test_footprint_from_actions_and_holds_and_requires(self):
        e = self._entry_for(
            requires=["lyra.wifi_awake"],
            actions=[{"type": "lyra.inject_settings_row"}, {"type": "lyra.patch_bytes"}],
            settings=[{"id": "x", "type": "bool", "holds": ["lyra.volume_state"]}])
        self.assertEqual(e["uses"], sorted(
            ["lyra.wifi_awake", "lyra.inject_settings_row", "lyra.patch_bytes",
             "lyra.volume_state", "lyra.register_setting"]))

    def test_settings_lowers_to_register_setting(self):
        e = self._entry_for(settings=[{"id": "x", "type": "bool"}])
        self.assertIn("lyra.register_setting", e["uses"])

    def test_status_lowers_to_register_status(self):
        e = self._entry_for(status=[{"id": "s"}])
        self.assertEqual(e["uses"], ["lyra.register_status"])

    def test_daemons_lowers_to_spawn_daemon(self):
        e = self._entry_for(daemons=[{"binary": "d.exe"}])
        self.assertEqual(e["uses"], ["lyra.spawn_daemon"])

    def test_status_icons_lowers_to_full_visual_set(self):
        e = self._entry_for(status_icons=[{"source": "status/s", "frames": ["@a.png"]}])
        self.assertEqual(e["uses"], sorted(
            ["lyra.gem_add_entry_bytes", "lyra.add_status_icon",
             "lyra.write_blob_bytes", "lyra.register_visuals"]))

    def test_declarative_field_present_but_empty_adds_nothing(self):
        # An empty array is not a use; only a populated field lowers.
        self.assertNotIn("uses", self._entry_for(settings=[], status=[], daemons=[]))

    def test_full_declarative_mod_footprint(self):
        # A screencast-shaped mod: every declarative field plus a hold and a mod dependency.
        e = self._entry_for(
            settings=[{"id": "on", "type": "bool", "holds": ["lyra.wifi_awake"]}],
            status=[{"id": "cast"}],
            status_icons=[{"source": "status/cast", "frames": ["@a.png"]}],
            daemons=[{"binary": "castd.exe"}],
            requires=["fx.reverb@2"])
        self.assertEqual(e["uses"], sorted(
            ["lyra.register_setting", "lyra.wifi_awake", "lyra.register_status",
             "lyra.gem_add_entry_bytes", "lyra.add_status_icon", "lyra.write_blob_bytes",
             "lyra.register_visuals", "lyra.spawn_daemon", "fx.reverb@2"]))

    def test_no_legacy_compat_fields(self):
        e = self._entry_for(requires=["lyra.wifi_awake"])
        self.assertNotIn("requires", e)
        self.assertNotIn("min_lyra", e)

    def test_provides_emitted_for_feature_mod(self):
        e = self._entry_for(provides=["fx.reverb@1:3"])
        self.assertEqual(e["provides"], ["fx.reverb@1:3"])

    def test_no_provides_when_none(self):
        self.assertNotIn("provides", self._entry_for())


if __name__ == "__main__":
    unittest.main()
