#!/usr/bin/env python3
"""Trigger zuxhook's in-host plugin loader via Lyra opcode 18.

Wire format:
  Request 32-byte header: [0]=18, [1-31] reserved
  Response 32 bytes: [0]=18, [1-4]=GetLastError, [5]=ret flag, [6-31] reserved

The daemon SetEvent's the named kernel event L"zune-zuxhook-trigger".
Each zuxhook instance currently waiting on that event (one per host
process that loaded zuxhook.dll, typically compositor.exe and
gemstone.exe) wakes and:

  1. LoadLibraryW(L"\\flash2\\automation\\plugin.dll")
  2. GetProcAddress(h, L"Activate")
  3. Calls Activate() under SEH
  4. FreeLibrary
  5. Appends result to \\flash2\\automation\\plugin-result-<pid>.log

The plugin runs INSIDE the host process (compositor or gemstone), so
it has direct access to that process's loaded modules, including
xuidll.dll's Xui APIs when the host is gemstone.exe. A plugin crash
inside gemstone takes down the UI; remove plugin.dll via opcode 17
(MoveFileW) or opcode 14 (DeleteFileW) before next boot to recover.

Workflow:
  1. lyra-write-file.py 192.168.0.100 my-plugin.dll '\\flash2\\automation\\plugin.dll'
  2. lyra-plugin-trigger.py 192.168.0.100
  3. lyra-read-file.py 192.168.0.100 '\\flash2\\automation\\plugin-result-<pid>.log' --output-dir <dir>
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
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        banner = read_exact(sock, len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")

        header = bytearray(32)
        header[0] = 18
        sock.sendall(header)

        resp = read_exact(sock, 32)
        if resp[0] != 18:
            if resp[0] == 0xFF:
                raise RuntimeError(
                    "daemon returned 0xFF unknown-command; payload doesn't "
                    "implement opcode 18 (src/nativeapp/src/rpc_server.cpp)."
                )
            raise RuntimeError(f"unexpected response opcode: 0x{resp[0]:02x}")
        err = struct.unpack("<I", resp[1:5])[0]
        ret = resp[5] != 0

        if ret:
            print("trigger sent; zuxhook should attempt to LoadLibrary "
                  "\\flash2\\automation\\plugin.dll and call Activate(). "
                  "Read plugin-result-<pid>.log to see what happened.")
            return 0
        print(f"FAIL: err=0x{err:08x} ({err})", file=sys.stderr)
        return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
