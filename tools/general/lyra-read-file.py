#!/usr/bin/env python3
"""Read device files to disk or stdout via the Lyra protobuf read-file command (protobuf command 0x10, CMD_RDFILE 2)."""
import argparse
import socket
import sys
from pathlib import Path

from lyra_rdfile import read_file


def safe_name(path):
    return path.strip("\\").replace("\\", "__").replace("/", "_") or "root"


def main():
    parser = argparse.ArgumentParser(
        description="Read small files through Lyra protobuf command 0x10."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    parser.add_argument("paths", nargs="+", help="device file paths")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--max-bytes", type=int, default=1024 * 1024)
    parser.add_argument(
        "--progress-interval",
        type=int,
        default=1024 * 1024,
        help="emit stderr progress every N bytes while writing to --output-dir; 0 disables",
    )
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument(
        "--resume",
        action="store_true",
        help="append to an existing .part file, seeking on the device to resume from its current length",
    )
    args = parser.parse_args()

    if args.max_bytes < 1:
        parser.error("--max-bytes must be at least 1")

    if args.output_dir is not None:
        args.output_dir.mkdir(parents=True, exist_ok=True)

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        banner = sock.recv(len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")
        print(f"banner={banner.decode().strip()}")

        for path in args.paths:
            if args.output_dir is not None:
                out_path = args.output_dir / safe_name(path)
                partial_path = out_path.with_suffix(out_path.suffix + ".part")
                resume_bytes = 0
                mode = "wb"
                if args.resume and partial_path.exists():
                    resume_bytes = partial_path.stat().st_size
                    mode = "ab"
                    print(f"resume_path={partial_path}")
                    print(f"resume_bytes={resume_bytes}")
                with partial_path.open(mode) as sink:
                    data, full_size = read_file(
                        sock,
                        path,
                        args.timeout,
                        args.max_bytes,
                        sink=sink,
                        offset=resume_bytes,
                        progress_interval=args.progress_interval,
                    )
                partial_path.replace(out_path)
                print(f"path={path}")
                print(f"bytes={full_size}")
                print(f"reported_size={full_size}")
                print(f"output={out_path}")
            else:
                data, full_size = read_file(sock, path, args.timeout, args.max_bytes)
                print(f"path={path}")
                print(f"bytes={len(data)}")
                print(f"reported_size={full_size}")
                sys.stdout.buffer.write(data)
                if not data.endswith(b"\n"):
                    sys.stdout.buffer.write(b"\n")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
