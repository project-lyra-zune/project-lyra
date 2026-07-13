#!/usr/bin/env python3
"""Wait for one debug event via Lyra opcode 6 (WaitForDebugEvent)."""
import argparse
import sys

from lyra_debug_common import connect, debug_wait, format_u32


def main():
    parser = argparse.ArgumentParser(
        description="Wait for one debug event through the Lyra daemon."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    sock, banner = connect(args.ip, args.port, args.timeout)
    try:
        ok, code, process_id, thread_id, aux = debug_wait(sock)
    finally:
        sock.close()

    print(f"banner={banner.decode().strip()}")
    print(f"event_available={'yes' if ok else 'no'}")
    print(f"code={format_u32(code)}")
    print(f"process_id={format_u32(process_id)}")
    print(f"thread_id={format_u32(thread_id)}")
    print(f"aux={format_u32(aux)}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
