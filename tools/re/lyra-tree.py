#!/usr/bin/env python3
"""Recursively list a device filesystem tree via the Lyra streaming protobuf lsdir command (protobuf command 0x10, CMD_LSDIR 1)."""
import argparse
import csv
import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from lyra_lsdir import list_dir, normalize_path


def main():
    parser = argparse.ArgumentParser(
        description="Recursively list Zune filesystem trees through Lyra lsdir."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    parser.add_argument("roots", nargs="+", help="root directory paths")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--max-depth", type=int, default=4)
    parser.add_argument("--max-entries", type=int, default=1000)
    args = parser.parse_args()

    if args.max_depth < 0:
        parser.error("--max-depth must be non-negative")
    if args.max_entries < 1:
        parser.error("--max-entries must be at least 1")

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        banner = sock.recv(len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")
        print(f"banner={banner.decode().strip()}")

        writer = csv.DictWriter(
            sys.stdout,
            fieldnames=["depth", "is_dir", "name", "full_path", "query", "error"],
        )
        writer.writeheader()

        pending = [(root, 0) for root in args.roots]
        seen_dirs = set()
        emitted = 0

        while pending:
            path, depth = pending.pop(0)
            if path in seen_dirs:
                continue
            seen_dirs.add(path)

            try:
                entries = list(list_dir(sock, path, args.timeout))
            except Exception as exc:
                writer.writerow(
                    {
                        "depth": depth,
                        "is_dir": "error",
                        "name": "",
                        "full_path": path,
                        "query": normalize_path(path),
                        "error": str(exc),
                    }
                )
                emitted += 1
                if emitted >= args.max_entries:
                    print(f"truncated=max_entries:{args.max_entries}", file=sys.stderr)
                    return
                continue

            for entry in entries:
                writer.writerow(
                    {
                        "depth": depth,
                        "is_dir": "yes" if entry["is_dir"] else "no",
                        "name": entry["name"],
                        "full_path": entry["full_path"],
                        "query": normalize_path(path),
                        "error": "",
                    }
                )
                emitted += 1
                if emitted >= args.max_entries:
                    print(f"truncated=max_entries:{args.max_entries}", file=sys.stderr)
                    return
                if entry["is_dir"] and depth < args.max_depth:
                    pending.append((entry["full_path"], depth + 1))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
