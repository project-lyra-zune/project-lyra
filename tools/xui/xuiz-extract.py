#!/usr/bin/env python3
"""XUIZ container full extractor.

Format (Microsoft Avalon "X-UI Zip", same family as Xbox 360 .xzp):

  +0x00..+0x03   Magic 'XUIZ'
  +0x04..+0x07   Version (u32 BE) = 1
  +0x08..+0x0B   Total file size (u32 BE)
  +0x0C..+0x0F   Reserved (zero-filled)
  +0x10..+0x13   Pointer-from-self-+6 to content blob (u32 BE)
                 (i.e., content_blob_offset = 0x16 + this_value)
  +0x14..+0x15   Entry count (u16 BE)
  +0x16..        Entry list

Per-entry header, variable-length size encoding based on file size:

  size_u32 BE (4 bytes):  for entry 0 only (always uses fixed u32 form)
  size_u32 BE (4 bytes):  large files
  size_u24 BE (3 bytes):  medium files (>65535 bytes)
  size_u16 BE (2 bytes):  small files (<=65535 bytes)
  offset_u32 BE (4 bytes): relative to content blob base
  name_len u16 LE (2 bytes): UTF-16 char count
  name UTF-16LE (name_len * 2 bytes)
  align pad (0..3 bytes to 4-byte alignment)

Entry 0 is special: the container has a 6-byte extension before its
metadata, AND entry 0 always uses u32 size (regardless of value).

Subsequent entries pick size encoding by value-range heuristic; we
validate by checking offset continuity (offsets must be strictly
increasing) and name_len sanity.
"""
from __future__ import annotations
import argparse, struct, sys
from pathlib import Path


def parse_xuiz(path: Path) -> tuple[int, int, list[dict], bytes]:
    """Returns (content_base, entry_count, entries, raw_data)."""
    data = path.read_bytes()
    if data[:4] != b'XUIZ':
        raise ValueError(f"{path}: not XUIZ")

    total_size = struct.unpack_from('>I', data, 0x08)[0]
    if total_size != len(data):
        raise ValueError(f"size mismatch: header says {total_size}, file is {len(data)}")

    blob_ptr = struct.unpack_from('>I', data, 0x10)[0]
    entry_count = struct.unpack_from('>H', data, 0x14)[0]
    content_base = 0x16 + blob_ptr
    # Sanity check: blob should start with XUIB or PNG magic (or some
    # other recognized format); just verify it's within file bounds
    if content_base >= len(data):
        raise ValueError(f"content_base 0x{content_base:X} out of bounds")

    entries = []
    p = 0x16  # First entry header begins right after 6-byte extension

    # Entry 0 is special: fixed u32 size encoding
    is_first = True
    prev_offset = -1

    while len(entries) < entry_count and p < len(data) - 8:
        # Try size encodings in order of largeness for first entry,
        # else by value-range
        candidates = []
        for size_bytes in (4, 3, 2):
            if size_bytes == 4:
                size = struct.unpack_from('>I', data, p)[0]
            elif size_bytes == 3:
                size = (data[p] << 16) | (data[p+1] << 8) | data[p+2]
            else:  # 2
                size = struct.unpack_from('>H', data, p)[0]
            offset_p = p + size_bytes
            if offset_p + 4 + 2 > len(data):
                continue
            offset = struct.unpack_from('>I', data, offset_p)[0]
            name_len_p = offset_p + 4
            name_len = struct.unpack_from('<H', data, name_len_p)[0]
            if not (1 <= name_len <= 200):
                continue
            name_off = name_len_p + 2
            if name_off + name_len * 2 > len(data):
                continue
            # Quirk: the LAST entry's name field can overlap the content
            # blob's first byte by 1 (the last char's high zero-byte is
            # implicit, not stored). Detect by checking if the name field
            # extends into the content_base region.
            name_bytes = data[name_off:name_off + name_len * 2]
            if name_off + name_len * 2 > content_base and name_off < content_base:
                # Truncate to content_base, append implicit 0x00 high byte
                name_bytes = data[name_off:content_base] + b'\x00'
            try:
                name = name_bytes.decode('utf-16-le')
            except:
                continue
            # Validate: name must be printable ASCII or contain expected chars
            ok_chars = all(0x20 <= ord(c) < 0x80 or c == '\\' for c in name)
            if not ok_chars:
                continue
            # Offset must be strictly increasing AND offset + size must fit
            if offset <= prev_offset:
                continue
            if content_base + offset + size > len(data):
                continue
            candidates.append((size_bytes, size, offset, name_len, name, name_off))

        if not candidates:
            print(f"  Stopped at 0x{p:X}: no valid header candidate", file=sys.stderr)
            break

        # For entry 0, prefer 4-byte size. For others, prefer smallest valid encoding.
        if is_first:
            cand = next((c for c in candidates if c[0] == 4), candidates[0])
            is_first = False
        else:
            # Smallest size encoding wins (heuristic: the format uses minimum needed)
            cand = sorted(candidates, key=lambda c: c[0])[0]

        size_bytes, size, offset, name_len, name, name_off = cand
        entries.append({
            'header_off': p,
            'size_bytes': size_bytes,
            'size': size,
            'offset_field': offset,
            'name_len': name_len,
            'name_off': name_off,
            'name': name,
        })
        prev_offset = offset

        # Advance past header + name. Entries 1+ have a fixed 1-byte
        # trailing pad after the name. Entry 0 has no trailing pad
        # (it's a special case sandwiched in the container extension area).
        p_end = name_off + name_len * 2
        pad = 0 if len(entries) == 1 else 1   # just-added entry 0 means no pad after it
        p = p_end + pad

    return content_base, entry_count, entries, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--out", default=None, help="Output directory (default: <path>.extracted)")
    ap.add_argument("--list", action="store_true", help="List only (don't extract)")
    args = ap.parse_args()

    path = Path(args.path)
    base, count, entries, data = parse_xuiz(path)
    print(f"{path.name}: claims {count} entries, parsed {len(entries)}, content_base=0x{base:X}")

    out_dir = None
    if not args.list:
        out_dir = Path(args.out or str(path) + ".extracted")
        out_dir.mkdir(parents=True, exist_ok=True)
        print(f"Extracting to: {out_dir}")

    for e in entries:
        file_off = base + e['offset_field']
        content = data[file_off:file_off + e['size']]
        sig = content[:8].hex() if content else ""
        # PNG = 8950... XUIB = 5855 4942...
        sig_label = ''
        if content[:4] == b'\x89PNG':
            sig_label = 'PNG'
        elif content[:4] == b'XUIB':
            sig_label = 'XUIB'
        elif content[:3] == b'DDS':
            sig_label = 'DDS'
        print(f"  size=0x{e['size']:>6X} (sz_enc={e['size_bytes']}B) "
              f"off=0x{file_off:08X} {sig_label:5s} {e['name']!r}")
        if out_dir and content:
            safe_name = e['name'].replace('\\', '/').replace('\x00', '')
            out_path = out_dir / safe_name
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_bytes(content)


if __name__ == "__main__":
    main()
