#!/usr/bin/env python3
"""Recursively pull a device filesystem tree over the Lyra protocol: walk every
reachable directory, hash and mirror every file.

Output lives under dumps/snapshots/<timestamp>/:
  manifest.csv  path,is_dir,size,sha256,fetched,error
  files/        mirrored tree for fetched bytes
  walk.log      one line per lsdir call
  errors.log    one line per per-file failure
  summary.txt   counts, totals, elapsed time
"""
from __future__ import annotations

import argparse
import csv
import errno
import hashlib
import socket
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from lyra_conn import DeviceConn, lsdir
from lyra_rdfile import FileTooLarge, read_file

DEFAULT_ROOTS = [
    "\\Windows",
    "\\Flash",
    "\\Flash2",
    "\\ramcache",
    "\\profiles",
    "\\Documents and Settings",
]

# Root files not surfaced by walking a directory.
DEFAULT_LOOSE_FILES = [
    "\\selfcheckCapture.raw",
]

_CONNECT_ERRNOS = {
    errno.ECONNREFUSED, errno.ETIMEDOUT, errno.EHOSTDOWN,
    errno.EHOSTUNREACH, errno.ENETUNREACH, errno.ECONNRESET,
}


def is_connect_error(exc: BaseException) -> bool:
    if isinstance(exc, socket.timeout):
        return True
    if isinstance(exc, OSError) and exc.errno in _CONNECT_ERRNOS:
        return True
    return False


def fetch(
    conn: DeviceConn,
    path: str,
    sink_path: Path,
    max_bytes: int,
    range_size: int,
    timeout: float,
    max_retries: int,
) -> tuple[int, str]:
    """Pull a file in bounded ranges to sink_path; returns (size, sha256_hex).

    A range is atomic: a failed one reconnects and re-requests from the same
    offset, so a large file resumes rather than restarting."""
    cap = max_bytes if max_bytes else None
    sink_path.parent.mkdir(parents=True, exist_ok=True)
    hasher = hashlib.sha256()
    got = 0
    full_size: int | None = None

    with sink_path.open("wb") as out:
        while True:
            attempt = 0
            while True:
                try:
                    data, reported = read_file(
                        conn.sock(), path, timeout,
                        max_bytes=cap, offset=got, length=range_size,
                    )
                    break
                except FileTooLarge:
                    raise
                except Exception:
                    conn.drop()
                    attempt += 1
                    if attempt > max_retries:
                        raise

            if full_size is None:
                full_size = reported
            if data:
                out.write(data)
                hasher.update(data)
                got += len(data)
            if not data or got >= full_size:
                break

    if full_size and got < full_size:
        raise RuntimeError(f"short read: got {got}, expected {full_size} (empty range at {got})")
    return got, hasher.hexdigest()


def safe_local_path(device_path: str, files_dir: Path) -> Path:
    # split on both separators so no component escapes files_dir as an absolute path
    parts = [p for p in device_path.replace("/", "\\").split("\\") if p]
    return files_dir.joinpath(*parts)


def load_existing_manifest(manifest_path: Path) -> dict[str, dict]:
    if not manifest_path.exists():
        return {}
    out: dict[str, dict] = {}
    with manifest_path.open("r", newline="") as f:
        for row in csv.DictReader(f):
            out[row["path"]] = row
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ip")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="per-recv socket inactivity timeout")
    parser.add_argument("--range-size", type=int, default=4 * 1024 * 1024,
                        help="bytes per rdfile request; the resume granularity for large files")
    parser.add_argument("--max-bytes", type=int, default=0,
                        help="per-file size cap; 0 = unlimited")
    parser.add_argument("--total-cap", type=int, default=0,
                        help="stop fetching once this many total bytes are pulled; 0 = unlimited")
    parser.add_argument("--max-depth", type=int, default=64)
    parser.add_argument("--max-retries", type=int, default=4,
                        help="reconnect-and-retry attempts per range before failing a file")
    parser.add_argument("--root", action="append", dest="roots", default=None,
                        help="override default roots (repeatable)")
    parser.add_argument("--loose-file", action="append", dest="loose_files", default=None,
                        help="override default loose files (repeatable)")
    parser.add_argument("--no-loose-files", action="store_true",
                        help="skip the default/configured loose-files list")
    parser.add_argument("--out", type=Path,
                        help="output dir (default dumps/snapshots/<UTC timestamp>)")
    parser.add_argument("--resume", type=Path,
                        help="resume into an existing snapshot dir instead of creating a new one")
    args = parser.parse_args()

    if args.range_size < 1:
        parser.error("--range-size must be at least 1")

    roots = args.roots if args.roots is not None else DEFAULT_ROOTS
    if args.no_loose_files:
        loose: list[str] = []
    else:
        loose = args.loose_files if args.loose_files is not None else DEFAULT_LOOSE_FILES
    loose = [p for p in loose if p]

    repo_dumps = Path(__file__).resolve().parents[1] / "dumps" / "snapshots"
    if args.resume:
        out_dir = args.resume
        if not out_dir.exists():
            print(f"--resume target does not exist: {out_dir}", file=sys.stderr)
            return 2
    elif args.out:
        out_dir = args.out
        out_dir.mkdir(parents=True, exist_ok=True)
    else:
        ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        out_dir = repo_dumps / ts
        out_dir.mkdir(parents=True, exist_ok=True)

    files_dir = out_dir / "files"
    files_dir.mkdir(exist_ok=True)
    manifest_path = out_dir / "manifest.csv"

    existing = load_existing_manifest(manifest_path)
    manifest_fields = ["path", "is_dir", "size", "sha256", "fetched", "error"]
    append = bool(manifest_path.exists() and existing)
    manifest_file = manifest_path.open("a" if append else "w", newline="")
    manifest_writer = csv.DictWriter(manifest_file, fieldnames=manifest_fields)
    if not append:
        manifest_writer.writeheader()
    walk_log = (out_dir / "walk.log").open("a")
    errors_log = (out_dir / "errors.log").open("a")

    started_at = time.monotonic()
    counts = {"dirs": 0, "files_listed": 0, "files_fetched": 0, "files_resumed": 0,
              "files_skipped_size": 0, "fetch_errors": 0, "lsdir_errors": 0}
    bytes_fetched = 0
    consecutive_dead = 0
    DEAD_THRESHOLD = 3

    def log_walk(msg: str) -> None:
        walk_log.write(f"{datetime.now(timezone.utc).isoformat()} {msg}\n")
        walk_log.flush()

    def log_error(msg: str) -> None:
        errors_log.write(f"{datetime.now(timezone.utc).isoformat()} {msg}\n")
        errors_log.flush()

    def emit(row: dict) -> None:
        manifest_writer.writerow(row)
        manifest_file.flush()

    def already_fetched(device_path: str) -> bool:
        prior = existing.get(device_path)
        if not prior or prior.get("fetched") != "yes" or not prior.get("sha256"):
            return False
        return safe_local_path(device_path, files_dir).exists()

    conn = DeviceConn(args.ip, args.port, args.timeout)
    queue: list[tuple[str, int]] = [(r, 0) for r in roots]
    seen: set[str] = set()

    def handle_dead(exc: Exception) -> None:
        nonlocal consecutive_dead
        if is_connect_error(exc):
            consecutive_dead += 1
            if consecutive_dead >= DEAD_THRESHOLD:
                log_error(f"abort: {consecutive_dead} consecutive connect failures, device unreachable")
                print(f"\nABORT: device unreachable after {consecutive_dead} connect failures",
                      file=sys.stderr)
                raise RuntimeError("device-unreachable-abort") from exc
        else:
            consecutive_dead = 0

    try:
        while queue:
            dir_path, depth = queue.pop(0)
            if dir_path in seen:
                continue
            seen.add(dir_path)
            counts["dirs"] += 1

            try:
                entries = lsdir(conn, dir_path, args.timeout)
                consecutive_dead = 0
                log_walk(f"lsdir ok depth={depth} dir={dir_path!r} count={len(entries)}")
            except Exception as exc:
                counts["lsdir_errors"] += 1
                emit({"path": dir_path, "is_dir": "yes", "size": "", "sha256": "",
                      "fetched": "no", "error": f"lsdir: {exc}"})
                log_walk(f"lsdir ERR depth={depth} dir={dir_path!r} err={exc}")
                log_error(f"lsdir {dir_path!r}: {exc}")
                handle_dead(exc)
                continue

            emit({"path": dir_path, "is_dir": "yes", "size": "", "sha256": "",
                  "fetched": "no", "error": ""})

            for entry in entries:
                name = entry["name"] or ""
                if not name or name in (".", ".."):
                    continue
                full = dir_path.rstrip("\\") + "\\" + name
                if entry["is_dir"]:
                    if depth < args.max_depth:
                        queue.append((full, depth + 1))
                    continue

                counts["files_listed"] += 1
                if already_fetched(full):
                    counts["files_resumed"] += 1
                    prior = existing[full]
                    emit({"path": full, "is_dir": "no", "size": prior.get("size", ""),
                          "sha256": prior.get("sha256", ""), "fetched": "yes", "error": ""})
                    continue

                if args.total_cap and bytes_fetched >= args.total_cap:
                    counts["files_skipped_size"] += 1
                    emit({"path": full, "is_dir": "no", "size": "", "sha256": "",
                          "fetched": "no", "error": "skipped:total-cap"})
                    continue

                local = safe_local_path(full, files_dir)
                start = time.monotonic()
                try:
                    size, sha = fetch(conn, full, local, args.max_bytes,
                                      args.range_size, args.timeout, args.max_retries)
                    elapsed_f = time.monotonic() - start
                    bytes_fetched += size
                    counts["files_fetched"] += 1
                    consecutive_dead = 0
                    emit({"path": full, "is_dir": "no", "size": size, "sha256": sha,
                          "fetched": "yes", "error": ""})
                    rate = size / elapsed_f / 1024 if elapsed_f > 0 else 0
                    print(f"  {full}  {size}B  {elapsed_f:.1f}s  {rate:.1f} KiB/s  "
                          f"[{counts['files_fetched']}/{counts['files_listed']}]",
                          file=sys.stderr)
                    log_walk(f"fetch ok path={full!r} size={size} elapsed={elapsed_f:.1f}s")
                except FileTooLarge as exc:
                    counts["files_skipped_size"] += 1
                    if local.exists():
                        local.unlink()
                    emit({"path": full, "is_dir": "no", "size": exc.size, "sha256": "",
                          "fetched": "no", "error": "skipped:max-bytes"})
                    log_walk(f"skip-size path={full!r} size={exc.size}")
                except Exception as exc:
                    counts["fetch_errors"] += 1
                    if local.exists():
                        local.unlink()
                    emit({"path": full, "is_dir": "no", "size": "", "sha256": "",
                          "fetched": "no", "error": str(exc)})
                    log_error(f"fetch {full!r}: {exc}")
                    handle_dead(exc)

        for path in loose:
            if already_fetched(path):
                counts["files_resumed"] += 1
                prior = existing[path]
                emit({"path": path, "is_dir": "no", "size": prior.get("size", ""),
                      "sha256": prior.get("sha256", ""), "fetched": "yes", "error": ""})
                continue
            counts["files_listed"] += 1
            local = safe_local_path(path, files_dir)
            try:
                size, sha = fetch(conn, path, local, args.max_bytes,
                                  args.range_size, args.timeout, args.max_retries)
                bytes_fetched += size
                counts["files_fetched"] += 1
                emit({"path": path, "is_dir": "no", "size": size, "sha256": sha,
                      "fetched": "yes", "error": ""})
                log_walk(f"fetch ok path={path!r} size={size}")
            except FileTooLarge as exc:
                counts["files_skipped_size"] += 1
                if local.exists():
                    local.unlink()
                emit({"path": path, "is_dir": "no", "size": exc.size, "sha256": "",
                      "fetched": "no", "error": "skipped:max-bytes"})
            except Exception as exc:
                counts["fetch_errors"] += 1
                if local.exists():
                    local.unlink()
                emit({"path": path, "is_dir": "no", "size": "", "sha256": "",
                      "fetched": "no", "error": str(exc)})
                log_error(f"fetch {path!r}: {exc}")

    finally:
        conn.drop()
        elapsed = time.monotonic() - started_at
        manifest_file.close()
        walk_log.close()
        errors_log.close()
        summary = (
            f"snapshot device={args.ip} out={out_dir}\n"
            f"elapsed_seconds={elapsed:.1f}\n"
            f"dirs_listed={counts['dirs']}\n"
            f"lsdir_errors={counts['lsdir_errors']}\n"
            f"files_listed={counts['files_listed']}\n"
            f"files_fetched={counts['files_fetched']}\n"
            f"files_resumed={counts['files_resumed']}\n"
            f"files_skipped_size={counts['files_skipped_size']}\n"
            f"fetch_errors={counts['fetch_errors']}\n"
            f"bytes_fetched={bytes_fetched}\n"
        )
        (out_dir / "summary.txt").write_text(summary)
        print(summary)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        sys.exit(130)
