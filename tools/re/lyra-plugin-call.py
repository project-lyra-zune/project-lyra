#!/usr/bin/env python3
"""Invoke a plugin DLL inside the daemon process via Lyra opcode 20.

Wire format:
  Request 32-byte header:
    [0]=20, [1-4]=path_len (u32 LE, 1..256),
    [5-8]=entry_len (u32 LE, 1..63),
    [9-12]=arg_len (u32 LE, 0..16384), [13-31] reserved
  Body: dll_path UTF-8 + entry name ASCII + arg bytes
  Response 32-byte header:
    [0]=20, [1-4]=int_return (u32 LE),
    [5-8]=out_len (u32 LE), [9-12]=GetLastError, [13-31] reserved
  Then out_len bytes of plugin output.

Plugin contract. The DLL exports one function:
  int Activate(const void* arg, int arg_len,
               void* out, int out_max, int* out_used)
  Returns int (whatever you want; passed back as int_return).
  Optionally writes up to out_max bytes to out, sets *out_used.

The plugin runs INSIDE nativeapp.exe (the daemon, kernel-privileged
after hax(), separate process from the UI host). LoadLibraryW resolves
imports against nativeapp's loaded modules: coredll, winsock,
iphlpapi, wininet, zdksystem, compclient. Plugins that want xuidll's
Xui APIs need to use lyra-plugin-trigger.py instead (which loads
plugin.dll inside gemstone.exe where xuidll lives).

Faulty plugins are SEH-caught: invocation fails with int_return=-1
and GetLastError set to the exception code, but the daemon stays up.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
from pathlib import Path


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
    parser.add_argument("dll_path", help=r"device path to the plugin, e.g. \flash2\automation\my-plugin.dll")
    parser.add_argument("--entry", default="Activate",
                        help="exported function name (ASCII; default: Activate)")
    parser.add_argument("--arg", default=None,
                        help="raw bytes to pass as arg (use @file to read from a file)")
    parser.add_argument("--out", default=None,
                        help="if set, write the plugin's output buffer to this local path")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    path_bytes = args.dll_path.encode("utf-8")
    entry_bytes = args.entry.encode("ascii")
    if not path_bytes or len(path_bytes) > 256:
        parser.error(f"dll_path UTF-8 length must be 1..256 (got {len(path_bytes)})")
    if not entry_bytes or len(entry_bytes) > 63:
        parser.error(f"entry name ASCII length must be 1..63 (got {len(entry_bytes)})")

    if args.arg is None:
        arg_bytes = b""
    elif args.arg.startswith("@"):
        arg_bytes = Path(args.arg[1:]).read_bytes()
    else:
        arg_bytes = args.arg.encode("utf-8")
    if len(arg_bytes) > 16384:
        parser.error(f"arg length must be 0..16384 (got {len(arg_bytes)})")

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        banner = read_exact(sock, len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")

        header = bytearray(32)
        header[0] = 20
        header[1:5] = struct.pack("<I", len(path_bytes))
        header[5:9] = struct.pack("<I", len(entry_bytes))
        header[9:13] = struct.pack("<I", len(arg_bytes))
        sock.sendall(header)
        sock.sendall(path_bytes)
        sock.sendall(entry_bytes)
        if arg_bytes:
            sock.sendall(arg_bytes)

        resp = read_exact(sock, 32)
        if resp[0] != 20:
            if resp[0] == 0xFF:
                raise RuntimeError(
                    "daemon returned 0xFF unknown-command; payload doesn't "
                    "implement opcode 20 (src/nativeapp/src/rpc_server.cpp)."
                )
            raise RuntimeError(f"unexpected response opcode: 0x{resp[0]:02x}")
        rc = struct.unpack("<i", resp[1:5])[0]
        out_len = struct.unpack("<I", resp[5:9])[0]
        err = struct.unpack("<I", resp[9:13])[0]

        out_bytes = read_exact(sock, out_len) if out_len else b""

        print(f"plugin returned rc={rc} (0x{rc & 0xffffffff:08x})  out_len={out_len}  err=0x{err:08x}")
        if args.out and out_bytes:
            Path(args.out).write_bytes(out_bytes)
            print(f"output written to {args.out}")
        elif out_bytes:
            try:
                print(f"output (utf-8): {out_bytes.decode('utf-8')}")
            except UnicodeDecodeError:
                print(f"output (hex): {out_bytes.hex()}")
        return 0 if (err == 0 and rc != -1) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
