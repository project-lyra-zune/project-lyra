#!/usr/bin/env python3
"""List a device filesystem directory via the Lyra streaming protobuf lsdir command (protobuf command 0x10, CMD_LSDIR 1)."""
import argparse
import csv
import socket
import sys

from lyra_lsdir import list_dir


def main():
    parser = argparse.ArgumentParser(
        description="List a Zune filesystem directory through Lyra streaming protobuf lsdir."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    parser.add_argument("paths", nargs="+", help="directory paths, e.g. \\\\Flash2")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        banner = sock.recv(len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")
        print(f"banner={banner.decode().strip()}")

        writer = csv.DictWriter(
            sys.stdout, fieldnames=["query", "is_dir", "name", "full_path"]
        )
        writer.writeheader()
        for path in args.paths:
            for entry in list_dir(sock, path, args.timeout):
                writer.writerow(
                    {
                        "query": path,
                        "is_dir": "yes" if entry["is_dir"] else "no",
                        "name": entry["name"],
                        "full_path": entry["full_path"],
                    }
                )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
