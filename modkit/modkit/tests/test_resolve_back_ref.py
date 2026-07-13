"""Host-side tests for manifest.resolve_back_ref.

Arithmetic on back-refs is a pure parsing concern, no device required.
Run directly: `python3 modkit/modkit/tests/test_resolve_back_ref.py`.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from modkit.manifest import resolve_back_ref, ManifestError


class ResolveBackRefTests(unittest.TestCase):
    def setUp(self):
        self.scope = {
            "base": 0x80020000,
            "small": 16,
            "label": "not-an-int",
        }

    def test_literal_lookup(self):
        self.assertEqual(resolve_back_ref("$base", self.scope), 0x80020000)
        self.assertEqual(resolve_back_ref("$small", self.scope), 16)
        self.assertEqual(resolve_back_ref("$label", self.scope), "not-an-int")

    def test_passthrough_non_ref(self):
        self.assertEqual(resolve_back_ref(42, self.scope), 42)
        self.assertEqual(resolve_back_ref("plain", self.scope), "plain")
        self.assertEqual(resolve_back_ref("0xC088D2B0", self.scope), "0xC088D2B0")

    def test_add_hex_offset(self):
        self.assertEqual(resolve_back_ref("$base + 0x70", self.scope), 0x80020070)
        self.assertEqual(resolve_back_ref("$base+0x4", self.scope), 0x80020004)
        self.assertEqual(resolve_back_ref("$base+0xFF", self.scope), 0x800200FF)

    def test_add_decimal_offset(self):
        self.assertEqual(resolve_back_ref("$base + 16", self.scope), 0x80020010)
        self.assertEqual(resolve_back_ref("$small + 4", self.scope), 20)

    def test_subtract_offset(self):
        self.assertEqual(resolve_back_ref("$base - 0x10", self.scope), 0x8001FFF0)
        self.assertEqual(resolve_back_ref("$base - 4", self.scope), 0x8001FFFC)
        self.assertEqual(resolve_back_ref("$small - 0x4", self.scope), 12)

    def test_whitespace_variants(self):
        for expr, expected in [
            ("$base+0x10", 0x80020010),
            ("$base +0x10", 0x80020010),
            ("$base+ 0x10", 0x80020010),
            ("$base + 0x10", 0x80020010),
            ("$base  -  0x4", 0x8001FFFC),
        ]:
            with self.subTest(expr=expr):
                self.assertEqual(resolve_back_ref(expr, self.scope), expected)

    def test_unknown_strict_raises(self):
        with self.assertRaises(ManifestError):
            resolve_back_ref("$missing", self.scope)
        with self.assertRaises(ManifestError):
            resolve_back_ref("$missing + 0x4", self.scope)

    def test_unknown_nonstrict_passes_through(self):
        self.assertEqual(
            resolve_back_ref("$missing", self.scope, strict=False), "$missing")
        # The whole expression is preserved unresolved (not just the name)
        # so the apply-time resolver re-evaluates the offset on the bound base.
        self.assertEqual(
            resolve_back_ref("$missing + 0x4", self.scope, strict=False),
            "$missing + 0x4")

    def test_arithmetic_on_non_int_base_raises(self):
        with self.assertRaises(ManifestError):
            resolve_back_ref("$label + 0x4", self.scope)

    def test_malformed_back_ref_raises(self):
        for bad in ["$base + 0xZZ", "$base ++ 4", "$1bad", "$base + ",
                    "$base + 4 + 8"]:
            with self.subTest(expr=bad):
                with self.assertRaises(ManifestError):
                    resolve_back_ref(bad, self.scope)


if __name__ == "__main__":
    unittest.main()
