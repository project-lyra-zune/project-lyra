#!/usr/bin/env python3
"""Dump a process address range using Lyra bulk-RPM opcode 9.

Wire format (daemon: src/nativeapp/src/rpc_server.cpp op_9):

  Request (32 bytes):
    [0]    = 9
    [1-4]  = process handle (u32 LE)
    [5-8]  = address (u32 LE)
    [9-12] = length (u32 LE, daemon caps at 16384)
    [13-31] reserved

  Response:
    32-byte header:
      [0]    = 9
      [1-4]  = bytes_read (u32 LE)
      [5-8]  = GetLastError (u32 LE)
      [9]    = ret flag (1 = ReadProcessMemory returned TRUE)
      [10-31] reserved
    Then `bytes_read` bytes of data.

Use --chunk-size 4 with --fill-failures for per-word failure isolation.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from pathlib import Path

from lyra_debug_common import (
    connect,
    find_process,
    format_u32,
    open_process,
    parse_u32,
    read_exact,
)


DAEMON_BULK_MAX = 16384  # must match BULK_RPM_MAX in the daemon


def bulk_read(
    sock: socket.socket,
    handle: int,
    address: int,
    length: int,
) -> tuple[int, int, bool, bytes]:
    """Single bulk-RPM request. Returns (bytes_read, last_error, success, data)."""
    if length > DAEMON_BULK_MAX:
        length = DAEMON_BULK_MAX
    payload = bytearray(32)
    payload[0] = 9
    payload[1:5] = struct.pack("<I", handle)
    payload[5:9] = struct.pack("<I", address)
    payload[9:13] = struct.pack("<I", length)
    sock.sendall(payload)
    header = read_exact(sock, 32)
    if header[0] != 9:
        if header[0] == 0xFF:
            raise RuntimeError(
                "daemon returned 0xFF unknown-command: this build does not implement "
                "opcode 9 (src/nativeapp/src/rpc_server.cpp op_9)."
            )
        raise RuntimeError(f"unexpected response opcode: 0x{header[0]:02x}")
    bytes_read = struct.unpack("<I", header[1:5])[0]
    err = struct.unpack("<I", header[5:9])[0]
    ok = header[9] != 0
    data = read_exact(sock, bytes_read) if bytes_read > 0 else b""
    return bytes_read, err, ok, data


def dump_range(
    sock: socket.socket,
    handle: int,
    start: int,
    end: int,
    chunk_size: int,
    fill_failures: bool,
    progress_interval: int,
) -> tuple[bytes, list[tuple[int, int]]]:
    output = bytearray()
    failures: list[tuple[int, int]] = []
    address = start
    next_progress = address + progress_interval if progress_interval > 0 else 0
    started = time.monotonic()

    while address < end:
        want = min(chunk_size, end - address)
        bytes_read, err, ok, data = bulk_read(sock, handle, address, want)

        if bytes_read > 0:
            output.extend(data)
            address += bytes_read
        elif fill_failures:
            failures.append((address, err))
            output.extend(b"\x00" * want)
            address += want
        else:
            raise RuntimeError(
                f"ReadProcessMemory failed at {format_u32(address)} "
                f"with error {format_u32(err)}"
            )

        if progress_interval > 0 and address >= next_progress:
            elapsed = time.monotonic() - started
            done = address - start
            total = end - start
            rate = done / elapsed if elapsed > 0 else 0
            print(
                f"progress={done}/{total} elapsed={elapsed:.1f}s rate={rate/1024:.1f} KiB/s",
                file=sys.stderr,
            )
            next_progress = address + progress_interval

    return bytes(output), failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Dump a process address range via Lyra bulk-RPM opcode 9."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--process-name", help="case-insensitive process name to dump")
    target.add_argument("--process-ptr", type=parse_u32, help="process pointer to dump")
    parser.add_argument("--start", type=parse_u32, required=True)
    parser.add_argument("--end", type=parse_u32, required=True, help="exclusive end address")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--chunk-size", type=int, default=DAEMON_BULK_MAX,
                        help=f"bytes per request, capped at daemon limit {DAEMON_BULK_MAX}")
    parser.add_argument("--fill-failures", action="store_true",
                        help="fill unreadable chunks with zeros and continue")
    parser.add_argument("--progress-interval", type=int, default=64 * 1024,
                        help="emit stderr progress every N bytes (0 disables)")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--max-processes", type=int, default=16)
    parser.add_argument("--name-chars", type=int, default=64)
    args = parser.parse_args()

    if args.end <= args.start:
        parser.error("--end must be greater than --start")
    if args.chunk_size < 1 or args.chunk_size > DAEMON_BULK_MAX:
        parser.error(f"--chunk-size must be in 1..{DAEMON_BULK_MAX}")
    if args.max_processes < 1 or args.name_chars < 1:
        parser.error("--max-processes and --name-chars must be positive")

    sock, banner = connect(args.ip, args.port, args.timeout)
    with sock:
        head, proc = find_process(
            sock, args.process_ptr, args.process_name,
            args.max_processes, args.name_chars,
        )
        handle = open_process(sock, proc["id"])
        if handle == 0:
            raise RuntimeError(f"OpenProcess returned null for pid {format_u32(proc['id'])}")

        started = time.monotonic()
        data, failures = dump_range(
            sock, handle, args.start, args.end, args.chunk_size,
            args.fill_failures, args.progress_interval,
        )
        elapsed = time.monotonic() - started

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)

    rate = len(data) / elapsed if elapsed > 0 else 0
    print(f"banner={banner.decode().strip()}")
    print(f"process_list_head={format_u32(head)}")
    print(
        "process="
        f"{format_u32(proc['proc_ptr'])},"
        f"id={format_u32(proc['id'])},"
        f"base={format_u32(proc['base'])},"
        f"name={proc['name']}"
    )
    print(f"process_handle={format_u32(handle)}")
    print(f"start={format_u32(args.start)}")
    print(f"end={format_u32(args.end)}")
    print(f"chunk_size={args.chunk_size}")
    print(f"bytes={len(data)}")
    print(f"failure_count={len(failures)}")
    print(f"elapsed_seconds={elapsed:.2f}")
    print(f"rate_kib_per_sec={rate/1024:.1f}")
    print(f"output={args.output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
