#!/usr/bin/env python3
"""Recursively remove a directory (tree) on the device.

The nativeapp daemon has DeleteFileW (rmfile, opcode 14) and CreateDirectoryW
(mkdir, opcode 13) but no RemoveDirectory opcode, so directory removal drives
coredll RemoveDirectoryW via the ZuneREPL kcall path (opcode 40). RemoveDirectoryW
only removes an EMPTY directory, so a non-empty dir has its files deleted
(DeleteFileW, also via kcall) and its subdirectories recursed first, then the dir
itself is removed. Children are enumerated with the streaming lsdir tool; an empty
dir reports ERROR_NO_MORE_FILES (18), which is treated as "no children".

Coredll addresses are Pavo v4.5 (image base 0x40320000); pass --coredll-base for
another firmware. RemoveDirectoryW = base+0x1400c, DeleteFileW = base+0x14044.

Usage:
  lyra-rmdir.py <ip> <device_dir> [<device_dir> ...] [--dry-run]
  lyra-rmdir.py 192.168.0.100 '\\flash2\\automation\\mods\\modicon-test'
"""
from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys

from zune_repl import ZuneREPL

KSCRATCH             = 0x800152D0   # kernel scratch the path is staged into
RVA_REMOVEDIRECTORYW = 0x1400C
RVA_DELETEFILEW      = 0x14044
_LSDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lyra-lsdir.py")


def lsdir(ip, path, port, timeout):
    """Direct children of `path` as ([(name, is_dir), ...], ok).

    ok is False only on an unexpected enumeration error; an empty directory
    (ERROR_NO_MORE_FILES / code=18) returns ([], True) so removal can proceed.
    """
    res = subprocess.run(
        [sys.executable, _LSDIR, ip, path, "--port", str(port), "--timeout", str(timeout)],
        capture_output=True, text=True, timeout=timeout + 15,
    )
    if res.returncode != 0:
        if "code=18" in (res.stderr + res.stdout):
            return [], True
        sys.stderr.write(res.stderr)
        return [], False

    lines = res.stdout.splitlines()
    header = next((i for i, l in enumerate(lines) if l.startswith("query,is_dir,name,")), None)
    if header is None:
        return [], True
    out = []
    for row in csv.DictReader(lines[header:]):
        name = (row.get("name") or "").strip()
        if name:
            out.append((name, (row.get("is_dir") or "") == "yes"))
    return out, True


def _kcall_path(repl, fn_addr, path):
    repl.kwrite(KSCRATCH, path.encode("utf-16-le") + b"\x00\x00")
    return repl.kcall(fn_addr, KSCRATCH)   # (ret, GetLastError)


def rmtree(repl, ip, path, *, port, timeout, dry, removedir, deletefile, depth=0):
    pad = "  " * depth
    children, ok = lsdir(ip, path, port, timeout)
    if not ok:
        print(f"{pad}SKIP  {path} (could not enumerate)")
        return False

    all_ok = True
    for name, is_dir in children:
        child = path.rstrip("\\") + "\\" + name
        if is_dir:
            all_ok &= rmtree(repl, ip, child, port=port, timeout=timeout, dry=dry,
                             removedir=removedir, deletefile=deletefile, depth=depth + 1)
        elif dry:
            print(f"{pad}  rm    {child}")
        else:
            ret, err = _kcall_path(repl, deletefile, child)
            print(f"{pad}  {'rm   ' if ret else 'FAIL '} {child}" + ("" if ret else f" (err {err})"))
            all_ok &= bool(ret)

    if dry:
        print(f"{pad}rmdir {path}")
        return all_ok
    ret, err = _kcall_path(repl, removedir, path)
    print(f"{pad}{'rmdir' if ret else 'FAIL '} {path}" + ("" if ret else f" (err {err})"))
    return all_ok and bool(ret)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ip", help="Zune HD IP address")
    ap.add_argument("paths", nargs="+", help=r"device directory, e.g. \flash2\automation\mods\foo")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--coredll-base", type=lambda s: int(s, 0), default=0x40320000,
                    help="coredll image base (Pavo v4.5 = 0x40320000)")
    ap.add_argument("--dry-run", action="store_true", help="print what would be removed, change nothing")
    args = ap.parse_args()

    removedir  = args.coredll_base + RVA_REMOVEDIRECTORYW
    deletefile = args.coredll_base + RVA_DELETEFILEW
    repl = None if args.dry_run else ZuneREPL(args.ip, port=args.port, timeout=args.timeout)

    rc = 0
    for p in args.paths:
        if not rmtree(repl, args.ip, p.rstrip("\\"), port=args.port, timeout=args.timeout,
                      dry=args.dry_run, removedir=removedir, deletefile=deletefile):
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
