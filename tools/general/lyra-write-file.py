#!/usr/bin/env python3
"""Push a local file to the device via Lyra opcode 12 (write-file).

Wire format (daemon: src/nativeapp/src/rpc_server.cpp op_12):

  Request 32-byte header:
    [0]    = 12
    [1-4]  = path_length (u32 LE, 1..256)
    [5-8]  = file_offset (u32 LE): absolute byte offset to write at;
                                    0 means CREATE_ALWAYS (truncate)
    [9-12] = data_length (u32 LE, 0..16384)
    [13-31] reserved
  Then path_length bytes of UTF-8 path
  Then data_length bytes of file content

  Response 32 bytes:
    [0]    = 12
    [1-4]  = bytes_written (u32 LE)
    [5-8]  = GetLastError (u32 LE)
    [9]    = ret flag (1 = WriteFile returned TRUE)
    [10-31] reserved

Requires the daemon built with opcode 12 support (this is a separate
opcode from opcode 9; the stock and pre-opcode-12 Lyra payloads
will return 0xFF unknown-command on first request).
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
from pathlib import Path


DAEMON_DATA_MAX = 16384  # must match WRFILE_DATA_MAX in the daemon


def read_exact(sock: socket.socket, count: int) -> bytes:
    out = bytearray()
    while len(out) < count:
        chunk = sock.recv(count - len(out))
        if not chunk:
            raise RuntimeError(f"socket closed after {len(out)} of {count} bytes")
        out.extend(chunk)
    return bytes(out)


def write_chunk(sock: socket.socket, path: bytes, offset: int, data: bytes) -> tuple[int, int, bool]:
    """Returns (bytes_written, last_error, success)."""
    header = bytearray(32)
    header[0] = 12
    header[1:5] = struct.pack("<I", len(path))
    header[5:9] = struct.pack("<I", offset)
    header[9:13] = struct.pack("<I", len(data))
    sock.sendall(header)
    sock.sendall(path)
    sock.sendall(data)
    resp = read_exact(sock, 32)
    if resp[0] != 12:
        if resp[0] == 0xFF:
            raise RuntimeError(
                "daemon returned 0xFF unknown-command; payload doesn't implement "
                "opcode 12 (src/nativeapp/src/rpc_server.cpp)."
            )
        raise RuntimeError(f"unexpected response opcode: 0x{resp[0]:02x}")
    bytes_written = struct.unpack("<I", resp[1:5])[0]
    err = struct.unpack("<I", resp[5:9])[0]
    ok = resp[9] != 0
    return bytes_written, err, ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ip")
    parser.add_argument("local", type=Path, help="local source file")
    parser.add_argument("remote",
                        help=r"device target path, e.g. \flash2\automation\zuxhook.dll")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--chunk-size", type=int, default=DAEMON_DATA_MAX,
                        help=f"bytes per request (max {DAEMON_DATA_MAX})")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.chunk_size < 1 or args.chunk_size > DAEMON_DATA_MAX:
        parser.error(f"--chunk-size must be in 1..{DAEMON_DATA_MAX}")

    data = args.local.read_bytes()
    path_bytes = args.remote.encode("utf-8")
    if len(path_bytes) > 256:
        parser.error(f"remote path UTF-8 length {len(path_bytes)} exceeds 256")

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        banner = read_exact(sock, len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")

        offset = 0
        if len(data) == 0:
            # special case: create empty file
            bw, err, ok = write_chunk(sock, path_bytes, 0, b"")
            print(f"created empty file: ok={ok} err=0x{err:08x}")
            return 0 if ok else 1

        while offset < len(data):
            chunk = data[offset:offset + args.chunk_size]
            bw, err, ok = write_chunk(sock, path_bytes, offset, chunk)
            if not ok:
                print(f"FAIL at offset {offset}: bytes_written={bw} "
                      f"err=0x{err:08x} ({err})", file=sys.stderr)
                return 1
            if not args.quiet:
                pct = 100.0 * (offset + bw) / max(1, len(data))
                print(f"  offset={offset:>8} chunk={len(chunk):>5} wrote={bw:>5}  "
                      f"({offset + bw}/{len(data)}, {pct:.1f}%)")
            offset += bw

    print(f"complete: pushed {len(data)} bytes to {args.remote!r}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
