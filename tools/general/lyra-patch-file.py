#!/usr/bin/env python3
"""In-place patch of a device file via Lyra opcode 12 (offset write, no truncate).

Sends a single opcode-12 offset-write: the daemon does OPEN_ALWAYS +
SetFilePointer(offset, FILE_BEGIN) + WriteFile (src/nativeapp/src/rpc_server.cpp
op_12). A nonzero offset does NOT truncate, so exactly len(bytes) bytes at `offset`
are overwritten and the rest of the file is left intact. Max 16384 bytes per call
(WRFILE_DATA_MAX). Offset 0 is refused here because the daemon maps it to
CREATE_ALWAYS (full-file truncate) - use lyra-write-file.py for that.

Patch bytes come from exactly one of --hex / --ascii / --utf16le / --infile.

  lyra-patch-file.py 192.168.0.100 "\\Flash2\\zunedb.dat" \
      --offset 0x44f48 --ascii "Uninstall Lyra"
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
from pathlib import Path

DAEMON_DATA_MAX = 16384  # WRFILE_DATA_MAX in the daemon


def read_exact(sock: socket.socket, count: int) -> bytes:
    out = bytearray()
    while len(out) < count:
        chunk = sock.recv(count - len(out))
        if not chunk:
            raise RuntimeError(f"socket closed after {len(out)} of {count} bytes")
        out.extend(chunk)
    return bytes(out)


def offset_write(sock: socket.socket, path: bytes, offset: int, data: bytes) -> tuple[int, int, bool]:
    """One opcode-12 offset-write. Returns (bytes_written, last_error, success)."""
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
            raise RuntimeError("daemon returned 0xFF unknown-command; payload lacks opcode 12")
        raise RuntimeError(f"unexpected response opcode: 0x{resp[0]:02x}")
    bytes_written = struct.unpack("<I", resp[1:5])[0]
    err = struct.unpack("<I", resp[5:9])[0]
    return bytes_written, err, resp[9] != 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ip")
    parser.add_argument("remote", help=r"device file path, e.g. \Flash2\zunedb.dat")
    parser.add_argument("--offset", type=lambda s: int(s, 0), required=True,
                        help="byte offset, decimal or 0xHEX; must be > 0")
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--hex", help="patch bytes as hex (e.g. 556e696e...)")
    src.add_argument("--ascii", help="patch bytes as ASCII text")
    src.add_argument("--utf16le", help="patch bytes as UTF-16LE text")
    src.add_argument("--infile", type=Path, help="patch bytes from a local file")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    if args.offset <= 0:
        parser.error("--offset must be > 0 (offset 0 truncates; use lyra-write-file.py)")

    if args.hex is not None:
        data = bytes.fromhex(args.hex)
    elif args.ascii is not None:
        data = args.ascii.encode("ascii")
    elif args.utf16le is not None:
        data = args.utf16le.encode("utf-16le")
    else:
        data = args.infile.read_bytes()

    if not (1 <= len(data) <= DAEMON_DATA_MAX):
        parser.error(f"patch length {len(data)} must be 1..{DAEMON_DATA_MAX}")
    path_bytes = args.remote.encode("utf-8")
    if len(path_bytes) > 256:
        parser.error(f"remote path UTF-8 length {len(path_bytes)} exceeds 256")

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        banner = read_exact(sock, len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")
        bw, err, ok = offset_write(sock, path_bytes, args.offset, data)

    print(f"patch {args.remote} @ {args.offset:#x} ({len(data)} bytes): "
          f"wrote={bw} ok={ok} err=0x{err:08x} ({err})")
    return 0 if ok and bw == len(data) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
