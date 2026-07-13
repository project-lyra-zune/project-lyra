#!/usr/bin/env python3
"""Walk the Zune HD kernel process list via read-only Lyra kernel-word reads (opcode 1)."""
import argparse
import csv
import sys

from lyra_debug_common import (
    NK_PROCESS_LIST_PTR,
    connect,
    format_u32,
    parse_u32,
    read_kernel_u32,
    read_proc,
)


def emit_csv(rows, looped, truncated):
    print(f"looped={'yes' if looped else 'no'}")
    print(f"truncated={'yes' if truncated else 'no'}")
    fieldnames = [
        "index",
        "proc_ptr",
        "next",
        "last",
        "id",
        "base",
        "thread_next",
        "thread_last",
        "name_ptr",
        "name",
    ]
    writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
    writer.writeheader()
    for index, proc in enumerate(rows):
        writer.writerow(
            {
                "index": index,
                "proc_ptr": format_u32(proc["proc_ptr"]),
                "next": format_u32(proc["next"]),
                "last": format_u32(proc["last"]),
                "id": format_u32(proc["id"]),
                "base": format_u32(proc["base"]),
                "thread_next": format_u32(proc["thread_next"]),
                "thread_last": format_u32(proc["thread_last"]),
                "name_ptr": format_u32(proc["name_ptr"]),
                "name": proc["name"],
            }
        )


def main():
    parser = argparse.ArgumentParser(
        description="Walk the Zune HD kernel process list using read-only Lyra reads."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument(
        "--head",
        type=parse_u32,
        help="process-list head pointer; default reads 0x80bee010",
    )
    parser.add_argument("--max-entries", type=int, default=16)
    parser.add_argument("--name-chars", type=int, default=64)
    args = parser.parse_args()

    if args.max_entries < 1:
        parser.error("--max-entries must be at least 1")
    if args.name_chars < 1:
        parser.error("--name-chars must be at least 1")

    sock, banner = connect(args.ip, args.port, args.timeout)
    with sock:
        head = args.head
        if head is None:
            head = read_kernel_u32(sock, NK_PROCESS_LIST_PTR)

        rows = []
        seen = set()
        current = head
        looped = False
        truncated = False

        for _ in range(args.max_entries):
            if current == 0:
                break
            if current in seen:
                looped = current == head
                truncated = current != head
                break
            seen.add(current)

            proc = read_proc(sock, current, args.name_chars)
            rows.append(proc)
            current = proc["next"]
            if current == head:
                looped = True
                break
        else:
            truncated = current != head
            looped = current == head

        print(f"banner={banner.decode().strip()}")
        print(f"head=0x{head:08x}")
        emit_csv(rows, looped, truncated)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
