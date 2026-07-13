#!/usr/bin/env python3
"""Write one u32 into a process address via Lyra opcode 4 (WriteProcessMemory)."""
import argparse
import sys

from lyra_debug_common import (
    connect,
    find_process,
    format_u32,
    open_process,
    parse_u32,
    write_process_u32,
)


def main():
    parser = argparse.ArgumentParser(
        description="Write one u32 into a process through Lyra WriteProcessMemory."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--process-name", help="case-insensitive process name")
    target.add_argument("--process-ptr", type=parse_u32, help="process pointer")
    parser.add_argument("--address", type=parse_u32, required=True)
    parser.add_argument("--value", type=parse_u32, required=True)
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
        handle = open_process(sock, proc["id"])
        if handle == 0:
            raise RuntimeError(f"OpenProcess returned null for pid {format_u32(proc['id'])}")
        ok, bytes_written, error = write_process_u32(sock, handle, args.address, args.value)
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
    print(f"process_handle={format_u32(handle)}")
    print(f"address={format_u32(args.address)}")
    print(f"value={format_u32(args.value)}")
    print(f"ok={'yes' if ok else 'no'}")
    print(f"bytes_written={bytes_written}")
    print(f"error={format_u32(error)}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
