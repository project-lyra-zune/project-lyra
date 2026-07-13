#!/usr/bin/env python3
"""Recursively list a device filesystem tree via the Lyra streaming protobuf lsdir command (protobuf command 0x10, CMD_LSDIR 1)."""
import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from lyra_conn import DeviceConn, lsdir
from lyra_lsdir import normalize_path


def main():
    parser = argparse.ArgumentParser(
        description="Recursively list Zune filesystem trees through Lyra lsdir."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    parser.add_argument("roots", nargs="+", help="root directory paths")
    parser.add_argument("--port", type=int, default=1337)
    # Above the daemon's 4 s find-first retry budget.
    parser.add_argument("--timeout", type=float, default=6.0)
    parser.add_argument("--max-depth", type=int, default=4)
    parser.add_argument("--max-entries", type=int, default=1000)
    args = parser.parse_args()

    if args.max_depth < 0:
        parser.error("--max-depth must be non-negative")
    if args.max_entries < 1:
        parser.error("--max-entries must be at least 1")

    conn = DeviceConn(args.ip, args.port, args.timeout)
    writer = csv.DictWriter(
        sys.stdout,
        fieldnames=["depth", "is_dir", "name", "full_path", "query", "error"],
    )
    writer.writeheader()

    pending = [(root, 0) for root in args.roots]
    seen_dirs = set()
    emitted = 0

    try:
        while pending:
            path, depth = pending.pop(0)
            if path in seen_dirs:
                continue
            seen_dirs.add(path)

            try:
                entries = lsdir(conn, path, args.timeout)
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
    finally:
        conn.drop()


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
