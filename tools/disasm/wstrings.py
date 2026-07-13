#!/usr/bin/env python3
"""Dump UTF-16LE + ASCII printable strings from a binary with file offsets.

macOS `strings` has no `-e l`; this fills that gap for RE work on the
v4.5 XIP modules and the CE6 flat registry (`default.fdf`).

Usage:
    wstrings.py <file> [minlen] [substr]
        minlen  minimum run length in chars (default 4)
        substr  case-insensitive filter (only print matching strings)
"""
import sys, struct

def runs(b, minlen):
    out, n = [], len(b)
    # UTF-16LE: printable-ascii byte followed by 0x00
    i = 0; cur = []; start = 0
    while i + 1 < n:
        c, hi = b[i], b[i + 1]
        if hi == 0 and 0x20 <= c < 0x7e:
            if not cur: start = i
            cur.append(chr(c)); i += 2; continue
        if len(cur) >= minlen: out.append((start, 'W', ''.join(cur)))
        cur = []; i += 1
    if len(cur) >= minlen: out.append((start, 'W', ''.join(cur)))
    # ASCII
    i = 0; cur = []; start = 0
    while i < n:
        c = b[i]
        if 0x20 <= c < 0x7e:
            if not cur: start = i
            cur.append(chr(c)); i += 1; continue
        if len(cur) >= minlen: out.append((start, 'A', ''.join(cur)))
        cur = []; i += 1
    if len(cur) >= minlen: out.append((start, 'A', ''.join(cur)))
    return sorted(out)

def main():
    path = sys.argv[1]
    minlen = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    sub = sys.argv[3].lower() if len(sys.argv) > 3 else None
    b = open(path, 'rb').read()
    for off, tag, s in runs(b, minlen):
        if sub is None or sub in s.lower():
            print(f"  0x{off:06x} {tag} {s}")

if __name__ == "__main__":
    main()
