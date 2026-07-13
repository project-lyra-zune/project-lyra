#!/usr/bin/env python3
"""Decompress a Windows CE B000FF image into its flat ROM memory layout.

Wire format:

    [0..6]    "B000FF\\n"           magic (7 bytes)
    [7..10]   start                 ROM base address (u32 LE)
    [11..14]  len                   total ROM byte count (u32 LE)
    records (until terminator):
      [..3]   addr                  load address for this block
      [4..7]  size                  block byte count
      [8..11] crc                   simple sum-checksum
      [..]    data                  `size` bytes
    EOF record:
      addr=0, size=entry_point, crc=0

The output file is the flat ROM image: every block placed at (addr - start)
inside a buffer of size `len`, gaps left as zero. Subsequent tooling
(ROMHDR walker, BinFS extractor) operates on this flat image.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


MAGIC = b"B000FF\n"


def decompress(data: bytes) -> tuple[int, int, int, bytes, list[dict]]:
    """Returns (start, length, entry_point, flat_image, records).

    `records` is a per-block summary for diagnostics: addr, size, crc, computed_crc,
    relative offset within the flat image.
    """
    if data[:7] != MAGIC:
        raise ValueError(f"missing B000FF magic; got {data[:7]!r}")

    start = struct.unpack("<I", data[7:11])[0]
    length = struct.unpack("<I", data[11:15])[0]
    flat = bytearray(length)
    records: list[dict] = []
    entry_point = 0

    offset = 15
    while offset < len(data):
        if offset + 12 > len(data):
            raise ValueError(f"truncated record header at offset {offset:#x}")
        addr, size, crc = struct.unpack("<III", data[offset:offset + 12])
        offset += 12
        if addr == 0 and crc == 0:
            entry_point = size
            break
        if offset + size > len(data):
            raise ValueError(
                f"record at {addr:#x} (size {size:#x}) extends past file end"
            )
        block = data[offset:offset + size]
        offset += size

        rel = addr - start
        if rel < 0 or rel + size > length:
            raise ValueError(
                f"record at {addr:#x} (size {size:#x}) doesn't fit in image "
                f"[{start:#x}, {start + length:#x})"
            )

        # B000FF "checksum" is a simple unsigned byte sum.
        computed = sum(block) & 0xFFFFFFFF
        flat[rel:rel + size] = block

        records.append({
            "addr": addr,
            "rel": rel,
            "size": size,
            "crc": crc,
            "computed_crc": computed,
            "ok": crc == computed,
        })

    return start, length, entry_point, bytes(flat), records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", type=Path, help="B000FF-wrapped .bin")
    parser.add_argument("output", type=Path, help="flat ROM image output")
    parser.add_argument("--summary", action="store_true",
                        help="print per-record summary to stderr")
    args = parser.parse_args()

    data = args.input.read_bytes()
    start, length, entry, flat, records = decompress(data)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(flat)

    bad = [r for r in records if not r["ok"]]

    print(f"input={args.input}")
    print(f"output={args.output}")
    print(f"start=0x{start:08x}")
    print(f"length={length} bytes")
    print(f"entry_point=0x{entry:08x}")
    print(f"records={len(records)}")
    print(f"crc_ok={len(records) - len(bad)}")
    print(f"crc_bad={len(bad)}")

    if args.summary:
        for i, r in enumerate(records[:30]):
            mark = "ok" if r["ok"] else "BAD"
            print(
                f"  #{i:3d} addr=0x{r['addr']:08x} rel=0x{r['rel']:08x} "
                f"size={r['size']:>8} crc={mark}",
                file=sys.stderr,
            )
        if len(records) > 30:
            print(f"  ... ({len(records) - 30} more)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
