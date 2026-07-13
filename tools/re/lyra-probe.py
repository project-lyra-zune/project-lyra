#!/usr/bin/env python3
"""Probe a target process's modules + exports via Lyra opcode 19.

Wire format:
  Request 32-byte header: [0]=19, [1-4]=target_pid (u32 LE), [5-31] reserved
  Response 32 bytes: [0]=19, [1-4]=GetLastError, [5]=ret flag, [6-31] reserved

The daemon runs in nativeapp.exe (separate from the UI host, kernel-
privileged after hax()). It enumerates target_pid's modules via
CreateToolhelp32Snapshot and walks each module's PE export directory
under SEH; faults skip the offending module instead of taking the
process down. Output goes to:

  \\flash2\\automation\\probe-<pid>.log

Use lyra-read-file.py to retrieve the log after probe.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys


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
    parser.add_argument("pid", type=lambda s: int(s, 0),
                        help="target pid (decimal or 0x-hex). Get via lyra-list-processes.py")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="socket timeout; probes can take a while for large module sets")
    args = parser.parse_args()

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        banner = read_exact(sock, len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")

        header = bytearray(32)
        header[0] = 19
        header[1:5] = struct.pack("<I", args.pid)
        sock.sendall(header)

        resp = read_exact(sock, 32)
        if resp[0] != 19:
            if resp[0] == 0xFF:
                raise RuntimeError(
                    "daemon returned 0xFF unknown-command; payload doesn't "
                    "implement opcode 19 (src/nativeapp/src/rpc_server.cpp)."
                )
            raise RuntimeError(f"unexpected response opcode: 0x{resp[0]:02x}")
        err = struct.unpack("<I", resp[1:5])[0]
        ret = resp[5] != 0

        if ret:
            print(f"probed pid={args.pid} -> \\flash2\\automation\\probe-{args.pid}.log")
            print(f"retrieve with: lyra-read-file.py {args.ip} '\\flash2\\automation\\probe-{args.pid}.log' --output-dir <dir>")
            return 0
        print(f"FAIL: probe pid={args.pid} err=0x{err:08x} ({err})", file=sys.stderr)
        return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
