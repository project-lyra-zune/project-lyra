#!/usr/bin/env python3
"""Delete a file on the device via Lyra opcode 14 (DeleteFileW).

Wire format mirrors opcode 13:
  Request 32-byte header: [0]=14, [1-4]=path_length (u32 LE, 1..256), [5-31] reserved
  Then path_length bytes (UTF-8 path).
  Response 32 bytes: [0]=14, [1-4]=GetLastError, [5]=ret flag, [6-31] reserved.

ERROR_FILE_NOT_FOUND (2) is treated as idempotent success here.

Note: DeleteFileW on a file currently mapped via LoadLibrary may fail with
ERROR_SHARING_VIOLATION (0x20). CE 6's behaviour with mapped images is
restrictive; the host caller should be prepared for that case.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys

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
    parser.add_argument("path", help=r"device file to delete, e.g. \flash2\automation\zuxhook.dll")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()

    path_bytes = args.path.encode("utf-8")
    if not path_bytes or len(path_bytes) > 256:
        parser.error(f"path UTF-8 length must be 1..256 (got {len(path_bytes)})")

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        banner = read_exact(sock, len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")

        header = bytearray(32)
        header[0] = 14
        header[1:5] = struct.pack("<I", len(path_bytes))
        sock.sendall(header)
        sock.sendall(path_bytes)

        resp = read_exact(sock, 32)
        if resp[0] != 14:
            if resp[0] == 0xFF:
                raise RuntimeError(
                    "daemon returned 0xFF unknown-command; payload doesn't "
                    "implement opcode 14 (src/nativeapp/src/rpc_server.cpp)."
                )
            raise RuntimeError(f"unexpected response opcode: 0x{resp[0]:02x}")
        err = struct.unpack("<I", resp[1:5])[0]
        ret = resp[5] != 0

        if ret:
            print(f"deleted: {args.path!r}")
            return 0
        if err == ERROR_FILE_NOT_FOUND:
            print(f"already gone (idempotent): {args.path!r}")
            return 0
        if err == ERROR_SHARING_VIOLATION:
            print(f"FAIL: {args.path!r} is currently mapped/in-use "
                  f"(ERROR_SHARING_VIOLATION 0x20)", file=sys.stderr)
            return 2
        print(f"FAIL: {args.path!r}  err=0x{err:08x} ({err})", file=sys.stderr)
        return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
