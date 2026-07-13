#!/usr/bin/env python3
"""Rename / move a file on the device via Lyra opcode 17 (MoveFileW).

Wire format:
  Request 32-byte header:
    [0]=17, [1-4]=src_path_length (u32 LE, 1..256),
    [5-8]=dst_path_length (u32 LE, 1..256), [9-31] reserved
  Then src_path_length bytes (UTF-8 src) + dst_path_length bytes (UTF-8 dst)
  Response 32 bytes:
    [0]=17, [1-4]=GetLastError, [5]=ret flag, [6-31] reserved

CE 6's MoveFileW sometimes succeeds where DeleteFileW fails on a file
currently mapped via LoadLibrary; rename can unlink the directory
entry without invalidating an existing mapping. Useful for in-place
zuxhook.dll updates: rename the live file out of the way, write the
new bytes to the canonical path, reboot.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys

ERROR_ALREADY_EXISTS = 183
ERROR_FILE_NOT_FOUND = 2
ERROR_SHARING_VIOLATION = 0x20


def read_exact(sock: socket.socket, count: int) -> bytes:
    out = bytearray()
    while len(out) < count:
        chunk = sock.recv(count - len(out))
        if not chunk:
            raise RuntimeError(f"socket closed after {len(out)} of {count} bytes")
        out.extend(chunk)
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ip")
    parser.add_argument("src", help=r"existing device path, e.g. \flash2\automation\zuxhook.dll")
    parser.add_argument("dst", help=r"new device path, e.g. \flash2\automation\zuxhook.old.dll")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    src_bytes = args.src.encode("utf-8")
    dst_bytes = args.dst.encode("utf-8")
    if not src_bytes or len(src_bytes) > 256:
        parser.error(f"src UTF-8 length must be 1..256 (got {len(src_bytes)})")
    if not dst_bytes or len(dst_bytes) > 256:
        parser.error(f"dst UTF-8 length must be 1..256 (got {len(dst_bytes)})")

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        banner = read_exact(sock, len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")

        header = bytearray(32)
        header[0] = 17
        header[1:5] = struct.pack("<I", len(src_bytes))
        header[5:9] = struct.pack("<I", len(dst_bytes))
        sock.sendall(header)
        sock.sendall(src_bytes)
        sock.sendall(dst_bytes)

        resp = read_exact(sock, 32)
        if resp[0] != 17:
            if resp[0] == 0xFF:
                raise RuntimeError(
                    "daemon returned 0xFF unknown-command; payload doesn't "
                    "implement opcode 17 (src/nativeapp/src/rpc_server.cpp)."
                )
            raise RuntimeError(f"unexpected response opcode: 0x{resp[0]:02x}")
        err = struct.unpack("<I", resp[1:5])[0]
        ret = resp[5] != 0

        if ret:
            print(f"moved: {args.src!r} -> {args.dst!r}")
            return 0
        if err == ERROR_FILE_NOT_FOUND:
            print(f"FAIL: src not found {args.src!r}", file=sys.stderr)
            return 2
        if err == ERROR_ALREADY_EXISTS:
            print(f"FAIL: dst exists {args.dst!r} (ERROR_ALREADY_EXISTS 183)", file=sys.stderr)
            return 3
        if err == ERROR_SHARING_VIOLATION:
            print(f"FAIL: sharing violation on {args.src!r} (ERROR_SHARING_VIOLATION 0x20), "
                  f"file mapped in a way that resists rename too", file=sys.stderr)
            return 4
        print(f"FAIL: err=0x{err:08x} ({err})", file=sys.stderr)
        return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
