#!/usr/bin/env python3
"""Read one u32 from a kernel address via Lyra opcode 1 (kernel-word read)."""
import argparse
import sys

from lyra_debug_common import NK_PROCESS_LIST_PTR, connect, parse_u32, read_kernel_u32


def main():
    parser = argparse.ArgumentParser(
        description="Read one u32 through the Lyra HD listener."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    parser.add_argument(
        "address",
        nargs="?",
        type=parse_u32,
        default=NK_PROCESS_LIST_PTR,
        help="kernel address to read, default: 0x80bee010",
    )
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    sock, banner = connect(args.ip, args.port, args.timeout)
    with sock:
        value = read_kernel_u32(sock, args.address)
        print(f"banner={banner.decode().strip()}")
        print(f"address=0x{args.address:08x}")
        print(f"value=0x{value:08x}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
