#!/usr/bin/env python3
"""Attach the Lyra daemon as a debugger to one process via opcode 5 (DebugActiveProcess)."""
import argparse
import sys

from lyra_debug_common import connect, debug_attach, find_process, format_u32, parse_u32


def main():
    parser = argparse.ArgumentParser(
        description="Attach the Lyra daemon as a debugger to one process."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--process-name", help="case-insensitive process name")
    target.add_argument("--process-ptr", type=parse_u32, help="process pointer")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--max-processes", type=int, default=16)
    parser.add_argument("--name-chars", type=int, default=64)
    args = parser.parse_args()

    sock, banner = connect(args.ip, args.port, args.timeout)
    try:
        process_head, proc = find_process(
            sock,
            args.process_ptr,
            args.process_name,
            args.max_processes,
            args.name_chars,
        )
        ok = debug_attach(sock, proc["id"])
    finally:
        sock.close()

    print(f"banner={banner.decode().strip()}")
    print(f"process_list_head={format_u32(process_head)}")
    print(
        "process="
        f"{format_u32(proc['proc_ptr'])},"
        f"id={format_u32(proc['id'])},"
        f"base={format_u32(proc['base'])},"
        f"name={proc['name']}"
    )
    print(f"attached={'yes' if ok else 'no'}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
