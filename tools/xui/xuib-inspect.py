#!/usr/bin/env python3
"""XUIB (compiled XAML / .xur) structure inspector.

Parses Microsoft Avalon's compiled-XAML binary format. Same format used
by Xbox 360 dashboard .xur files.

XUIB header (16 bytes):
  +0x00..+0x03  Magic 'XUIB'
  +0x04..+0x07  Version (u32 BE), observed = 5
  +0x08..+0x0B  Reserved (zero)
  +0x0C..+0x0F  Section count + flags (u32 BE), observed 0x000C0000 = 12 sections?
                Actually: +0x0C..+0x0D is the SIZE of the section directory in bytes,
                and +0x0E..+0x0F is the section count (4 sections × 12 bytes = 48 = 0x30,
                but observed 0x0C suggests 12 bytes per section header)

Section directory (each entry 12 bytes):
  +0x00..+0x03  Type tag (4 ASCII chars: STRN, VECT, QUAT, CUST, DATA)
  +0x04..+0x07  Section offset (u32 BE, from file start)
  +0x08..+0x0B  Section size (u32 BE)

Section types observed:
  STRN: string table (length-prefixed UTF-16LE strings)
  VECT: vector data (3D positions, sizes as float triples)
  QUAT: quaternion data (rotations)
  CUST: custom data (per-element properties)
  DATA: main scene-graph data (element tree, brushes, animations)

Goal of this tool: dump structure + list strings + identify ARGB color
candidates within DATA section. Sets up a follow-up "find and modify"
tool.
"""
from __future__ import annotations
import argparse, struct, sys
from pathlib import Path


def parse_xuib(data: bytes) -> dict:
    """XUIB header layout:
      +0x00..+0x03  Magic 'XUIB'
      +0x04..+0x07  Version (u32 BE), observed = 5
      +0x08..+0x0B  Large-file flag (u32 BE): 0 = small, 1 = large/extended
      +0x0C..+0x0D  Bytes-per-section-header (u16 BE) = 0x000C (12)
      +0x0E..+0x11  File size (u32 BE): high u16 at 0x0E..0x0F, low at 0x10..0x11
      +0x12..+0x13  Section count (u16 BE)
      +0x14..       Section directory entries (12 bytes each):
                       4 (tag) + 4 (offset u32 BE) + 4 (size u32 BE)

    For LARGE files (flag = 1), there's an additional 40-byte "extra table"
    inserted between the XUIB header and the section directory:
      +0x14..        Extra table (section_count × 8 bytes each)
      +0x14+8N       Section directory begins here (after extra table)

    The exact purpose of the extra table is unknown; for now we simply
    skip past it. Each 8-byte entry contains two u32 BE values.
    """
    if data[:4] != b'XUIB':
        raise ValueError(f"not XUIB: {data[:4]!r}")
    version = struct.unpack_from('>I', data, 0x04)[0]
    large_flag = struct.unpack_from('>I', data, 0x08)[0]
    bytes_per_section = struct.unpack_from('>H', data, 0x0C)[0]
    file_size_field = struct.unpack_from('>I', data, 0x0E)[0]
    section_count = struct.unpack_from('>H', data, 0x12)[0]

    # Section directory location depends on large-file flag
    if large_flag:
        sd_off = 0x14 + section_count * 8
    else:
        sd_off = 0x14

    sections = []
    for i in range(section_count):
        sd = sd_off + i * bytes_per_section
        tag = data[sd:sd+4].decode('ascii', errors='replace')
        offset = struct.unpack_from('>I', data, sd + 4)[0]
        size = struct.unpack_from('>I', data, sd + 8)[0]
        sections.append({'tag': tag, 'offset': offset, 'size': size})

    return {
        'version': version,
        'large_flag': large_flag,
        'bytes_per_section': bytes_per_section,
        'file_size': file_size_field,
        'section_count': section_count,
        'sd_offset': sd_off,
        'sections': sections,
    }


def parse_strn(data: bytes, off: int, size: int) -> list[str]:
    """Parse STRN section: length-prefixed UTF-16BE strings.
    Each string: u16 BE length (in chars), then length*2 UTF-16BE bytes."""
    end = off + size
    p = off
    strings = []
    while p < end - 2:
        l = struct.unpack_from('>H', data, p)[0]
        if l == 0:
            p += 2
            continue
        if p + 2 + l*2 > end:
            break
        try:
            s = data[p+2:p+2+l*2].decode('utf-16-be')
            if all(0x20 <= ord(c) < 0x80 or c == '\\' for c in s):
                strings.append(s)
            else:
                strings.append(repr(s))  # fall back to repr for non-printable
        except:
            strings.append(f'<bad@{p:#x}>')
        p += 2 + l*2
    return strings


def find_argb_colors(data: bytes, off: int, size: int) -> list[tuple[int, int]]:
    """Find candidate ARGB color values in a section.
    Returns [(file_offset, argb_value)] for each."""
    end = off + size
    candidates = []
    for i in range(off, end - 4):
        val = struct.unpack_from('>I', data, i)[0]
        a = (val >> 24) & 0xFF
        r = (val >> 16) & 0xFF
        g = (val >> 8) & 0xFF
        b = val & 0xFF
        # Plausible color: alpha is 0xFF / 0x80 / 0x00 (fully opaque, semi, or fully transparent)
        # AND it's not a known non-color sentinel
        if a in (0xFF, 0x80, 0xC0, 0x40, 0x60, 0xA0, 0x20, 0x00):
            # Skip pure black with zero alpha (all zeros, could be padding)
            if val == 0:
                continue
            # Skip recognizable non-color patterns (UTF-16 ASCII bytes have low byte char + high zero)
            # An ASCII char's UTF-16 BE bytes look like 00 X1 00 X2, already filtered by alpha check
            candidates.append((i, val))
    return candidates


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--strings", action="store_true", help="Dump all strings")
    ap.add_argument("--colors", action="store_true", help="List ARGB color candidates in DATA section")
    ap.add_argument("--all", action="store_true", help="Show everything")
    args = ap.parse_args()

    if args.all:
        args.strings = args.colors = True

    path = Path(args.path)
    data = path.read_bytes()
    info = parse_xuib(data)
    print(f"{path.name}: {len(data)} bytes")
    print(f"  version: {info['version']}")
    print(f"  sections ({info['section_count']}, {info['bytes_per_section']}B each):")
    for s in info['sections']:
        sig = data[s['offset']:s['offset']+8].hex() if s['offset']+8 <= len(data) else ''
        print(f"    {s['tag']}  offset=0x{s['offset']:06X}  size=0x{s['size']:X} ({s['size']})")

    if args.strings:
        for s in info['sections']:
            if s['tag'] == 'STRN':
                strings = parse_strn(data, s['offset'], s['size'])
                print(f"\n  STRN ({len(strings)} strings):")
                for i, st in enumerate(strings):
                    print(f"    [{i:>3}] {st!r}")

    if args.colors:
        # Look in all sections except STRN
        for s in info['sections']:
            if s['tag'] in ('STRN',):
                continue
            colors = find_argb_colors(data, s['offset'], s['size'])
            if colors:
                # Bucket by value to show distinct colors
                from collections import Counter
                cnt = Counter(c[1] for c in colors)
                print(f"\n  Distinct ARGB candidates in {s['tag']} ({len(cnt)} distinct, {len(colors)} total):")
                for argb, n in cnt.most_common(30):
                    a = (argb >> 24) & 0xFF
                    r = (argb >> 16) & 0xFF
                    g = (argb >> 8) & 0xFF
                    b = argb & 0xFF
                    print(f"    0x{argb:08X}  A={a:>3} RGB=({r:>3},{g:>3},{b:>3})  ×{n}")


if __name__ == "__main__":
    main()
