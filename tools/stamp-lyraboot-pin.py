#!/usr/bin/env python3
"""Rewrite src/lyraboot/boot_pin.h from a generated feed.

Prints "changed" when it rewrote the header, meaning lyraboot must be rebuilt.
"""

import argparse
import json
import pathlib
import re
import sys

PLATFORM_ID = "lyra"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", type=pathlib.Path, required=True)
    ap.add_argument("--header", type=pathlib.Path, required=True)
    ap.add_argument("--base-url", required=True,
                    help="plaintext host serving the bundle, e.g. http://install.zune.moe")
    args = ap.parse_args()

    feed = json.loads(args.feed.read_text())
    row = next((m for m in feed.get("mods", []) if m.get("mod_id") == PLATFORM_ID), None)
    if not row:
        sys.exit(f"no '{PLATFORM_ID}' row in {args.feed}; the platform bundle was not published")

    version = row["version"]
    sha = row["sha256"]
    url = f"{args.base_url.rstrip('/')}/{PLATFORM_ID}-{version}.zmod"

    text = args.header.read_text()
    updated = text
    for macro, value in (("LYRA_PIN_VERSION", version),
                         ("LYRA_PIN_URL", url),
                         ("LYRA_PIN_SHA256", sha)):
        pattern = re.compile(r'^(#define\s+%s\s+)"[^"]*"$' % macro, re.M)
        if not pattern.search(updated):
            sys.exit(f"{args.header} has no {macro} to stamp")
        updated = pattern.sub(lambda m: f'{m.group(1)}"{value}"', updated)

    if updated == text:
        print(f"unchanged  {version} {sha[:16]}")
        return 0

    args.header.write_text(updated)
    print(f"changed    {version} {sha[:16]}  {url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
