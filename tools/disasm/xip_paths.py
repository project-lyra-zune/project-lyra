"""Locate the extracted CE6 XIP firmware tree for the offline RE tools.

Single source of the firmwares/extracted path. Set LYRA_XIP_DIR to point at
the extracted dir that holds the v4.5-eimgfs-*/xip subtrees; otherwise walk up
from this file to the workspace dir that contains firmwares/extracted. The
firmware tree lives above project-lyra in the workspace, so a fixed relative
offset breaks whenever a tool is relocated; discovery does not.
"""
import os
from pathlib import Path


def extracted_dir():
    override = os.environ.get("LYRA_XIP_DIR")
    if override:
        return Path(override)
    here = Path(__file__).resolve()
    for parent in here.parents:
        candidate = parent / "firmwares" / "extracted"
        if candidate.is_dir():
            return candidate
    return here.parents[3] / "firmwares" / "extracted"
