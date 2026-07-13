#!/usr/bin/env python3
"""Bake the installer splash assets to raw BGRA blobs the on-device UI loads directly.
Run from this directory; outputs *.bgra next to it.
"""

import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
FONT = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"

MASK_WIDTH = 240
LABEL_HEIGHT = 30
LABEL_PAD = 8

# Ordering is the InstallStage enum in install_ui.cpp; keep in sync.
STAGES = [
    ("prepare", "Preparing..."),
    ("loader", "Installing loader"),
    ("daemon", "Installing daemon"),
    ("mods", "Installing mods"),
    ("done", "Rebooting..."),
]


def write_bgra(path: Path, alpha: Image.Image) -> None:
    """Store `alpha` (mode L) as white BGRA with coverage in the alpha channel."""
    w, h = alpha.size
    white = Image.new("L", (w, h), 255)
    bgra = Image.merge("RGBA", (white, white, white, alpha))  # B=G=R=255, A=coverage
    with path.open("wb") as f:
        f.write(struct.pack("<HH", w, h))
        f.write(bgra.tobytes())
    print(f"  {path.name}: {w}x{h} ({4 + w * h * 4} bytes)")


def bake_mask() -> None:
    png = HERE / "_mask.png"
    subprocess.run(
        ["rsvg-convert", "-w", str(MASK_WIDTH), "-o", str(png), str(HERE / "lyra.svg")],
        check=True,
    )
    img = Image.open(png).convert("RGBA")
    write_bgra(HERE / "lyra_mask.bgra", img.getchannel("A"))
    png.unlink()


def bake_label(slug: str, text: str) -> None:
    font = ImageFont.truetype(FONT, LABEL_HEIGHT - 2 * LABEL_PAD)
    tmp = Image.new("L", (1, 1), 0)
    box = ImageDraw.Draw(tmp).textbbox((0, 0), text, font=font)
    w = (box[2] - box[0]) + 2 * LABEL_PAD
    img = Image.new("L", (w, LABEL_HEIGHT), 0)
    ImageDraw.Draw(img).text((LABEL_PAD - box[0], LABEL_PAD - box[1]), text, fill=255, font=font)
    write_bgra(HERE / f"status_{slug}.bgra", img)


def main() -> None:
    if not Path(FONT).exists():
        sys.exit(f"font not found: {FONT}")
    print("baking installer assets:")
    bake_mask()
    for slug, text in STAGES:
        bake_label(slug, text)
    # Solid fill for the reboot countdown bar (the quad is scaled at draw time).
    write_bgra(HERE / "bar.bgra", Image.new("L", (16, 8), 255))


if __name__ == "__main__":
    main()
