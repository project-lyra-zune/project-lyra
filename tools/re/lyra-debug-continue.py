#!/usr/bin/env python3
"""Continue one debug event via Lyra opcode 7 (ContinueDebugEvent)."""
import argparse
import sys

from lyra_debug_common import connect, debug_continue, format_u32, parse_u32


def main():
    parser = argparse.ArgumentParser(
        description="Continue one debug event through the Lyra daemon."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    parser.add_argument("process_id", type=parse_u32, help="debug event process id")
    parser.add_argument("thread_id", type=parse_u32, help="debug event thread id")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    sock, banner = connect(args.ip, args.port, args.timeout)
    try:
        debug_continue(sock, args.process_id, args.thread_id)
    finally:
        sock.close()

    print(f"banner={banner.decode().strip()}")
    print(f"continued_process={format_u32(args.process_id)}")
    print(f"continued_thread={format_u32(args.thread_id)}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
