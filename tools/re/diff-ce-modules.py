#!/usr/bin/env python3
"""Compare two or more extracted CE ROM images at module granularity.

Takes the output directories of ce-romhdr-extract.py (each holding manifest.csv
and modules/) and reports, per module and data file, whether it is byte-identical
across the images. For entries that differ, it counts the differing bytes so a
handful of relocated pointers (same build, different ROM rebase) is distinguished
from a genuinely different module.

This is the byte-exact answer to "is the on-flash image the same build as this
firmware .bin", where a raw flat diff only gives a correlation because the two
lay records out at different offsets.

  diff-ce-modules.py flash:./flash nk:./nk rec:./rec
"""
import argparse
import csv
from pathlib import Path


def load(spec):
    label, _, path = spec.partition(":")
    d = Path(path or label)
    label = label if path else d.name
    rows = {}
    for r in csv.DictReader((d / "manifest.csv").open()):
        rows[(r["kind"], r["name"])] = r["sha256"]
    return label, d, rows


def byte_delta(dir_a, dir_b, kind, name):
    sub = "modules" if kind == "module" else "files"
    a = (dir_a / sub / name)
    b = (dir_b / sub / name)
    if not (a.exists() and b.exists()):
        return None
    ba, bb = a.read_bytes(), b.read_bytes()
    if len(ba) != len(bb):
        return ("sizediff", abs(len(ba) - len(bb)), max(len(ba), len(bb)))
    n = len(ba) or 1
    d = sum(1 for i in range(len(ba)) if ba[i] != bb[i])
    return ("diff", d, n)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("images", nargs="+", help="label:dir (or just dir) per extract")
    ap.add_argument("--threshold", type=float, default=1.0,
                    help="percent below which a diff is treated as a rebase fixup")
    args = ap.parse_args()

    imgs = [load(s) for s in args.images]
    labels = [x[0] for x in imgs]
    dirs = {x[0]: x[1] for x in imgs}
    maps = {x[0]: x[2] for x in imgs}
    base = labels[0]
    keys = sorted(set().union(*[set(m) for m in maps.values()]))

    identical = 0
    fixup = []
    real = []
    missing = []
    for k in keys:
        shas = {L: maps[L].get(k) for L in labels}
        if any(v is None for v in shas.values()):
            missing.append(k)
            continue
        if len(set(shas.values())) == 1:
            identical += 1
            continue
        # differs from base: measure against base for each other image
        worst = 0.0
        for L in labels[1:]:
            r = byte_delta(dirs[base], dirs[L], *k)
            if r and r[0] == "diff":
                worst = max(worst, 100 * r[1] / r[2])
            else:
                worst = 100.0
        (fixup if worst < args.threshold else real).append((k, worst))

    print(f"images: {', '.join(labels)}  (base = {base})")
    print(f"entries: {len(keys)}")
    print(f"  byte-identical across all:        {identical}")
    print(f"  differ < {args.threshold}% vs {base} (rebase fixup): {len(fixup)}")
    print(f"  differ >= {args.threshold}% (real):          {len(real)}")
    if missing:
        print(f"  present in only some images:      {len(missing)}  {[n for _, n in missing][:12]}")
    if real:
        print("\nsubstantively different modules:")
        for (kind, name), pct in sorted(real, key=lambda x: -x[1]):
            print(f"  {name:24} {pct:5.1f}%")
    return 0


if __name__ == "__main__":
    main()
