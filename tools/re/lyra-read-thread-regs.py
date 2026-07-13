#!/usr/bin/env python3
"""Read the ARM register context of each thread in one process via Lyra opcode 8 (GetThreadContext)."""
import argparse
import csv
import sys

from lyra_debug_common import (
    NK_PROCESS_LIST_PTR,
    connect,
    find_process,
    format_u32,
    parse_u32,
    read_kernel_u32,
    read_thread_regs,
)


REG_NAMES = (
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "pc", "lr", "sp",
)


def walk_threads(sock, thread_head, max_threads):
    rows = []
    seen = set()
    current = thread_head
    looped = False
    truncated = False

    for _ in range(max_threads):
        if current == 0:
            break
        if current in seen:
            looped = current == thread_head
            truncated = current != thread_head
            break
        seen.add(current)

        row = {
            "thread_ptr": current,
            "next": read_kernel_u32(sock, current + 0x00),
            "thread_id": read_kernel_u32(sock, current + 0x20),
        }
        rows.append(row)
        current = row["next"]
        if current == thread_head:
            looped = True
            break
    else:
        truncated = current != thread_head
        looped = current == thread_head

    return rows, looped, truncated


def main():
    parser = argparse.ArgumentParser(
        description="Read Lyra thread contexts for one process using opcode 0x08."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--process-name", help="case-insensitive process name to sample")
    target.add_argument("--process-ptr", type=parse_u32, help="process pointer to sample")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--max-processes", type=int, default=16)
    parser.add_argument("--max-threads", type=int, default=16)
    parser.add_argument("--name-chars", type=int, default=64)
    args = parser.parse_args()

    for option, value in (
        ("--max-processes", args.max_processes),
        ("--max-threads", args.max_threads),
        ("--name-chars", args.name_chars),
    ):
        if value < 1:
            parser.error(f"{option} must be at least 1")

    sock, banner = connect(args.ip, args.port, args.timeout)
    with sock:
        process_head = read_kernel_u32(sock, NK_PROCESS_LIST_PTR)
        _, proc = find_process(
            sock,
            args.process_ptr,
            args.process_name,
            args.max_processes,
            args.name_chars,
        )
        threads, looped, truncated = walk_threads(sock, proc["thread_next"], args.max_threads)

        print(f"banner={banner.decode().strip()}")
        print(f"process_list_head={format_u32(process_head)}")
        print(
            "process="
            f"{format_u32(proc['proc_ptr'])},"
            f"id={format_u32(proc['id'])},"
            f"name={proc['name']}"
        )
        print(f"threads_looped={'yes' if looped else 'no'}")
        print(f"threads_truncated={'yes' if truncated else 'no'}")

        writer = csv.DictWriter(
            sys.stdout,
            fieldnames=["index", "thread_ptr", "next", "thread_id", *REG_NAMES],
        )
        writer.writeheader()
        for index, thread in enumerate(threads):
            regs = read_thread_regs(sock, thread["thread_id"])
            row = {
                "index": index,
                "thread_ptr": format_u32(thread["thread_ptr"]),
                "next": format_u32(thread["next"]),
                "thread_id": format_u32(thread["thread_id"]),
            }
            for name in REG_NAMES:
                row[name] = format_u32(regs[name])
            writer.writerow(row)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
