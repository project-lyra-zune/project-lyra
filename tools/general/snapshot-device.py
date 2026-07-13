#!/usr/bin/env python3
"""Walk reachable directories on a Lyra-deployed device, hash every file, and pull bytes for files under a size cap.

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
import hashlib
import socket
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from lyra_proto import (
    decode_resp_err_payload,
    decode_varint,
    encode_field_bytes,
    encode_field_varint,
    parse_legacy_error_bytes,
)
from lyra_lsdir import list_dir

CMD_RDFILE = 2
RSP_RDFILE_DATA = 2
RSP_RDFILE_EOF = 3
RSP_ERR = 4

LEGACY_MARKERS = (0xCD, 0xCE, 0xCF, 0xDE, 0xDF, 0xFF)


class FileLocked(Exception):
    """Daemon reports size=0 with immediate EOF: the file is memory-mapped or otherwise unreadable.

    Distinct from a real protocol error so the walker can mark and continue.
    """

# Roots that previous probing showed are reachable. \Mounted Volume, \My Documents,
# \Program Files, \Temp returned lsdir_findfirstfile_error and are skipped.
DEFAULT_ROOTS = [
    "\\Windows",
    "\\Flash",
    "\\Flash2",
    "\\ramcache",
    "\\profiles",
    "\\Documents and Settings",
]

# Loose files at the device root that aren't reached by walking a directory.
DEFAULT_LOOSE_FILES = [
    "\\selfcheckCapture.raw",
]

SKIP_EXTENSIONS = {
    ".wma", ".mp3", ".m4a", ".mp4", ".mp4v", ".wmv", ".asf",
    ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".ico",
    ".pba",
}

# Files known to wedge the daemon's flash-read path under our throughput
# (CJK fallback fonts, large runtime ZCPs). Skip outright, never issue rdfile.
HARD_SKIP_PATHS = {
    "\\Windows\\MeiryoForZune.ttf",       # ~9.4 MB Japanese font
    "\\Windows\\MalgunForZune.ttf",       # ~2.3 MB Korean font
    "\\Windows\\runtimeZune.v3.1.zcp",    # ~4.3 MB runtime container
    "\\Windows\\Dictionary.dat",          # ~1.3 MB, borderline-deadline
    "\\Flash2\\zunedb.dat",               # ~4 MB, deadlines; promoted from good-session
    "\\Flash2\\drmstore.dat",             # 54 MB, exceeds cap; promoted from good-session
}

# Directories whose entire subtree we never descend into.
# A single mid-stream abort wedges the daemon, and these subtrees are dominated
# by multi-MB content blobs (music tracks, etc.).
HARD_SKIP_DIRS = {
    "\\Flash2\\Content",  # music library, many multi-MB .zcp files
}


# ── Wire helpers ─────────────────────────────────────────────────────────

def make_rdfile_request(path: str) -> bytes:
    payload = encode_field_bytes(1, path.encode("utf-8"))
    msg = encode_field_varint(1, CMD_RDFILE) + encode_field_bytes(3, payload)
    return bytes([16]) + msg


def parse_rdfile_response_one(data: bytes) -> dict | None:
    """Return one parsed message or None if more bytes needed.

    Result: {"kind": "data"|"eof"|"error", "consumed": N, ...}.
    """
    offset = 0
    cmd = None
    while offset < len(data):
        try:
            key, offset = decode_varint(data, offset)
        except ValueError:
            return None
        field, wire = key >> 3, key & 7
        if field == 1 and wire == 0:
            try:
                cmd, offset = decode_varint(data, offset)
            except ValueError:
                return None
            continue
        if wire == 2:
            try:
                sz, offset = decode_varint(data, offset)
            except ValueError:
                return None
            if offset + sz > len(data):
                return None
            payload = data[offset:offset + sz]
            offset += sz
            if field == 3 and cmd == RSP_RDFILE_DATA:
                chunk, full_size = parse_rdfile_data_payload(payload)
                return {"kind": "data", "data": chunk, "full_size": full_size, "consumed": offset}
            if field == 4 and cmd == RSP_RDFILE_EOF:
                return {"kind": "eof", "consumed": offset}
            if field == 5 and cmd == RSP_ERR:
                err = decode_resp_err_payload(payload)
                return {"kind": "error", "error": err, "consumed": offset}
            continue
        if wire == 0:
            try:
                _, offset = decode_varint(data, offset)
            except ValueError:
                return None
            continue
        raise ValueError(f"unsupported rdfile field {field} wire {wire}")
    return None


def parse_rdfile_data_payload(payload: bytes) -> tuple[bytes, int]:
    offset = 0
    data = b""
    full_size = None
    while offset < len(payload):
        key, offset = decode_varint(payload, offset)
        field, wire = key >> 3, key & 7
        if field == 1 and wire == 2:
            sz, offset = decode_varint(payload, offset)
            data = payload[offset:offset + sz]
            offset += sz
        elif field == 2 and wire == 0:
            full_size, offset = decode_varint(payload, offset)
        else:
            raise ValueError(f"unexpected RespRdfile field {field} wire {wire}")
    if full_size is None:
        raise ValueError("missing fullsz")
    return data, full_size


# ── Connection helpers ───────────────────────────────────────────────────

def connect(ip: str, port: int, timeout: float) -> socket.socket:
    s = socket.create_connection((ip, port), timeout=timeout)
    banner = s.recv(len(b"Hello\n"))
    if banner != b"Hello\n":
        s.close()
        raise RuntimeError(f"unexpected banner: {banner!r}")
    return s


def lsdir(ip: str, port: int, timeout: float, path: str) -> list[dict]:
    """One connection, one lsdir, return entries (or raise)."""
    with connect(ip, port, timeout) as s:
        return list(list_dir(s, path, timeout))


def fetch(
    ip: str,
    port: int,
    timeout: float,
    path: str,
    sink_path: Path,
    max_bytes: int,
    deadline_seconds: float = 120.0,
) -> tuple[int, str]:
    """Fetch a file streaming bytes to sink_path. Returns (size, sha256_hex).

    `timeout` is per-recv socket inactivity. `deadline_seconds` is a total wall-clock
    cap from the moment the rdfile request is sent, guarding against slow-trickle
    daemons that never satisfy the per-recv timeout but never finish either.
    """
    sink_path.parent.mkdir(parents=True, exist_ok=True)
    hasher = hashlib.sha256()
    bytes_seen = 0
    full_size = None
    with connect(ip, port, timeout) as s, sink_path.open("wb") as out:
        s.settimeout(timeout)
        s.sendall(make_rdfile_request(path))
        deadline = time.monotonic() + deadline_seconds
        buf = bytearray()
        while True:
            while True:
                if buf and buf[0] in LEGACY_MARKERS:
                    raise RuntimeError(parse_legacy_error_bytes(bytes(buf)))
                msg = parse_rdfile_response_one(bytes(buf))
                if msg is not None:
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        f"deadline exceeded ({deadline_seconds:.0f}s) at {bytes_seen}/{full_size} bytes"
                    )
                chunk = s.recv(8192)
                if not chunk:
                    raise RuntimeError("socket closed during rdfile")
                if not buf and chunk[0] in LEGACY_MARKERS:
                    raise RuntimeError(parse_legacy_error_bytes(chunk))
                buf.extend(chunk)
            del buf[:msg["consumed"]]
            if msg["kind"] == "error":
                err = msg["error"] or {}
                raise RuntimeError(f"device error code={err.get('code')} msg={err.get('msg')!r}")
            if msg["kind"] == "eof":
                if full_size is None:
                    raise FileLocked(path)
                if full_size == 0 and bytes_seen == 0:
                    raise FileLocked(path)
                if bytes_seen != full_size:
                    raise RuntimeError(f"short read: got {bytes_seen}, expected {full_size}")
                return full_size, hasher.hexdigest()
            data = msg["data"]
            if full_size is None:
                full_size = msg["full_size"]
                if full_size > max_bytes:
                    raise RuntimeError(f"size {full_size} exceeds cap {max_bytes}")
            elif full_size != msg["full_size"]:
                raise RuntimeError(f"size changed mid-stream {full_size} -> {msg['full_size']}")
            out.write(data)
            hasher.update(data)
            bytes_seen += len(data)


# ── Snapshot driver ──────────────────────────────────────────────────────

def safe_local_path(device_path: str, files_dir: Path) -> Path:
    parts = [p for p in device_path.split("\\") if p]
    return files_dir.joinpath(*parts)


def should_fetch(device_path: str) -> tuple[bool, str]:
    """Return (fetch?, skip_reason). skip_reason is "" when fetch=True."""
    if device_path in HARD_SKIP_PATHS:
        return False, "skipped:hard-skip-list"
    lower = device_path.lower()
    for ext in SKIP_EXTENSIONS:
        if lower.endswith(ext):
            return False, "skipped:extension"
    return True, ""


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
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--max-bytes", type=int, default=16 * 1024 * 1024,
                        help="per-file size cap; larger files are listed but not fetched")
    parser.add_argument("--total-cap", type=int, default=256 * 1024 * 1024,
                        help="abort if total fetched bytes exceeds this")
    parser.add_argument("--max-depth", type=int, default=12)
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

    roots = args.roots if args.roots is not None else DEFAULT_ROOTS
    if args.no_loose_files:
        loose = []
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
    walk_log_path = out_dir / "walk.log"
    errors_log_path = out_dir / "errors.log"

    existing = load_existing_manifest(manifest_path)
    manifest_fields = ["path", "is_dir", "size", "sha256", "fetched", "error"]
    new_manifest: bool = manifest_path.exists() and existing
    manifest_file = manifest_path.open("a" if new_manifest else "w", newline="")
    manifest_writer = csv.DictWriter(manifest_file, fieldnames=manifest_fields)
    if not new_manifest:
        manifest_writer.writeheader()
    walk_log = walk_log_path.open("a")
    errors_log = errors_log_path.open("a")

    started_at = time.monotonic()
    counts = {"dirs": 0, "files_listed": 0, "files_fetched": 0, "files_skipped_ext": 0,
              "files_skipped_size": 0, "files_resumed": 0, "files_locked": 0,
              "fetch_errors": 0, "lsdir_errors": 0}
    bytes_fetched = 0
    consecutive_dead_errors = 0  # connect timeouts in a row → daemon/device dead, abort run
    DEAD_THRESHOLD = 3

    def log_walk(msg: str) -> None:
        walk_log.write(f"{datetime.now(timezone.utc).isoformat()} {msg}\n")
        walk_log.flush()

    def log_error(msg: str) -> None:
        errors_log.write(f"{datetime.now(timezone.utc).isoformat()} {msg}\n")
        errors_log.flush()

    def emit_manifest(row: dict) -> None:
        manifest_writer.writerow(row)
        manifest_file.flush()

    def already_fetched(device_path: str, expected_size: int | None = None) -> bool:
        prior = existing.get(device_path)
        if not prior or prior.get("fetched") != "yes" or not prior.get("sha256"):
            return False
        local = safe_local_path(device_path, files_dir)
        if not local.exists():
            return False
        if expected_size is not None and local.stat().st_size != expected_size:
            return False
        return True

    def previously_unrecoverable(device_path: str) -> str | None:
        """Return prior unrecoverable error string (timeout/deadline/locked) so the
        walker can mark and skip without re-attempting a known-bad fetch."""
        prior = existing.get(device_path)
        if not prior:
            return None
        err = prior.get("error", "")
        if err.startswith("locked:") or err == "timed out" or err.startswith("deadline"):
            return err
        return None

    def is_hard_skip_dir(p: str) -> bool:
        for skip in HARD_SKIP_DIRS:
            if p == skip or p.startswith(skip + "\\"):
                return True
        return False

    queue: list[tuple[str, int]] = [(r, 0) for r in roots if not is_hard_skip_dir(r)]
    seen: set[str] = set()

    try:
        while queue:
            dir_path, depth = queue.pop(0)
            if dir_path in seen:
                continue
            seen.add(dir_path)
            counts["dirs"] += 1

            try:
                entries = lsdir(args.ip, args.port, args.timeout, dir_path)
                log_walk(f"lsdir ok depth={depth} dir={dir_path!r} count={len(entries)}")
            except Exception as exc:
                counts["lsdir_errors"] += 1
                emit_manifest({"path": dir_path, "is_dir": "yes", "size": "", "sha256": "",
                               "fetched": "no", "error": f"lsdir: {exc}"})
                log_walk(f"lsdir ERR depth={depth} dir={dir_path!r} err={exc}")
                log_error(f"lsdir {dir_path!r}: {exc}")
                continue

            emit_manifest({"path": dir_path, "is_dir": "yes", "size": "", "sha256": "",
                           "fetched": "no", "error": ""})

            for entry in entries:
                name = entry["name"] or ""
                if not name or name in (".", ".."):
                    continue
                full = dir_path.rstrip("\\") + "\\" + name
                if entry["is_dir"]:
                    if is_hard_skip_dir(full):
                        emit_manifest({"path": full, "is_dir": "yes", "size": "", "sha256": "",
                                       "fetched": "no", "error": "skipped:hard-skip-dir"})
                        log_walk(f"hard-skip-dir path={full!r}")
                        continue
                    if depth < args.max_depth:
                        queue.append((full, depth + 1))
                    continue

                counts["files_listed"] += 1
                want, skip_reason = should_fetch(full)
                if not want:
                    counts["files_skipped_ext"] += 1
                    emit_manifest({"path": full, "is_dir": "no", "size": "", "sha256": "",
                                   "fetched": "no", "error": skip_reason})
                    continue

                if already_fetched(full):
                    counts["files_resumed"] += 1
                    prior = existing[full]
                    emit_manifest({"path": full, "is_dir": "no", "size": prior.get("size", ""),
                                   "sha256": prior.get("sha256", ""), "fetched": "yes",
                                   "error": ""})
                    continue

                prior_err = previously_unrecoverable(full)
                if prior_err is not None:
                    if prior_err.startswith("locked:"):
                        counts["files_locked"] += 1
                    else:
                        counts["fetch_errors"] += 1
                    emit_manifest({"path": full, "is_dir": "no", "size": "", "sha256": "",
                                   "fetched": "no", "error": f"resume-skip: {prior_err}"})
                    log_walk(f"resume-skip path={full!r} prior_err={prior_err!r}")
                    continue

                if bytes_fetched >= args.total_cap:
                    counts["files_skipped_size"] += 1
                    emit_manifest({"path": full, "is_dir": "no", "size": "", "sha256": "",
                                   "fetched": "no", "error": "skipped:total-cap"})
                    continue

                local = safe_local_path(full, files_dir)
                start = time.monotonic()
                try:
                    size, sha = fetch(args.ip, args.port, args.timeout, full, local, args.max_bytes)
                    elapsed_f = time.monotonic() - start
                    bytes_fetched += size
                    counts["files_fetched"] += 1
                    consecutive_dead_errors = 0
                    emit_manifest({"path": full, "is_dir": "no", "size": size, "sha256": sha,
                                   "fetched": "yes", "error": ""})
                    rate = size / elapsed_f if elapsed_f > 0 else 0
                    print(f"  {full}  {size}B  {elapsed_f:.1f}s  {rate/1024:.1f} KiB/s  "
                          f"[{counts['files_fetched']}/{counts['files_listed']}]",
                          file=sys.stderr)
                    log_walk(f"fetch ok path={full!r} size={size} elapsed={elapsed_f:.1f}s")
                except FileLocked:
                    counts["files_locked"] += 1
                    if local.exists():
                        try:
                            local.unlink()
                        except OSError:
                            pass
                    emit_manifest({"path": full, "is_dir": "no", "size": "", "sha256": "",
                                   "fetched": "no", "error": "locked:size0-eof"})
                    log_walk(f"fetch locked path={full!r}")
                except Exception as exc:
                    counts["fetch_errors"] += 1
                    if local.exists():
                        try:
                            local.unlink()
                        except OSError:
                            pass
                    err_str = str(exc)
                    if "exceeds cap" in err_str:
                        counts["files_skipped_size"] += 1
                    emit_manifest({"path": full, "is_dir": "no", "size": "", "sha256": "",
                                   "fetched": "no", "error": err_str})
                    log_error(f"fetch {full!r}: {exc}")
                    # Connect-level failures cluster when the device/daemon is dying.
                    # Bail out of the whole run so we stop generating noise and don't
                    # keep abusing a wedged target.
                    if "Errno 60" in err_str or "Errno 64" in err_str or "Host is down" in err_str:
                        consecutive_dead_errors += 1
                        if consecutive_dead_errors >= DEAD_THRESHOLD:
                            log_error(f"abort: {consecutive_dead_errors} consecutive connect failures: device unreachable")
                            print(f"\nABORT: device unreachable after {consecutive_dead_errors} consecutive connect failures",
                                  file=sys.stderr)
                            raise RuntimeError("device-unreachable-abort") from exc
                    else:
                        consecutive_dead_errors = 0
                    # The daemon's flash-read state machine needs ~30s to unwind
                    # after we abandon a partial transfer (timeout / deadline).
                    # Without this pause the next fetch hangs while the daemon recovers.
                    if "timed out" in err_str or err_str.startswith("deadline"):
                        log_walk(f"daemon-recovery sleep 30s after {err_str}")
                        time.sleep(30)

        # Loose root files (not surfaced by walking any directory).
        for path in loose:
            if already_fetched(path):
                counts["files_resumed"] += 1
                prior = existing[path]
                emit_manifest({"path": path, "is_dir": "no", "size": prior.get("size", ""),
                               "sha256": prior.get("sha256", ""), "fetched": "yes",
                               "error": ""})
                continue
            counts["files_listed"] += 1
            want, skip_reason = should_fetch(path)
            if not want:
                counts["files_skipped_ext"] += 1
                emit_manifest({"path": path, "is_dir": "no", "size": "", "sha256": "",
                               "fetched": "no", "error": skip_reason})
                continue
            local = safe_local_path(path, files_dir)
            try:
                size, sha = fetch(args.ip, args.port, args.timeout, path, local, args.max_bytes)
                bytes_fetched += size
                counts["files_fetched"] += 1
                emit_manifest({"path": path, "is_dir": "no", "size": size, "sha256": sha,
                               "fetched": "yes", "error": ""})
                log_walk(f"fetch ok path={path!r} size={size}")
            except FileLocked:
                counts["files_locked"] += 1
                if local.exists():
                    try:
                        local.unlink()
                    except OSError:
                        pass
                emit_manifest({"path": path, "is_dir": "no", "size": "", "sha256": "",
                               "fetched": "no", "error": "locked:size0-eof"})
                log_walk(f"fetch locked path={path!r}")
            except Exception as exc:
                counts["fetch_errors"] += 1
                if local.exists():
                    try:
                        local.unlink()
                    except OSError:
                        pass
                emit_manifest({"path": path, "is_dir": "no", "size": "", "sha256": "",
                               "fetched": "no", "error": str(exc)})
                log_error(f"fetch {path!r}: {exc}")

    finally:
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
            f"files_locked={counts['files_locked']}\n"
            f"files_skipped_extension={counts['files_skipped_ext']}\n"
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
