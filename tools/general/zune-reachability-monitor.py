#!/usr/bin/env python3
"""Monitor Zune reachability with ICMP ping and the port-1337 listener banner."""
from __future__ import annotations

import argparse
import socket
import subprocess
import time
from datetime import datetime


def ping_once(ip: str, timeout_ms: int) -> str:
    timeout_s = max(1, int((timeout_ms + 999) / 1000))
    try:
        proc = subprocess.run(
            ["ping", "-c", "1", "-W", str(timeout_ms), ip],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_s + 1,
        )
    except Exception as exc:
        return f"ping=ERR:{exc}"
    if proc.returncode == 0:
        for line in proc.stdout.splitlines():
            if "time=" in line:
                return "ping=OK " + line.strip()
        return "ping=OK"
    detail = (proc.stderr or proc.stdout).strip().splitlines()
    return "ping=FAIL " + (detail[-1] if detail else f"rc={proc.returncode}")


def banner_once(ip: str, port: int, timeout: float) -> str:
    try:
        with socket.create_connection((ip, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            banner = sock.recv(len(b"Hello\n"))
        if banner == b"Hello\n":
            return "banner=OK"
        return f"banner=BAD:{banner!r}"
    except Exception as exc:
        return f"banner=FAIL:{exc}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ip")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--seconds", type=int, default=90)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--timeout", type=float, default=1.5)
    ap.add_argument("--ping-timeout-ms", type=int, default=1000)
    args = ap.parse_args()

    deadline = time.monotonic() + args.seconds
    attempt = 0
    while True:
        attempt += 1
        stamp = datetime.now().strftime("%H:%M:%S")
        print(
            f"[{stamp}] #{attempt} "
            f"{ping_once(args.ip, args.ping_timeout_ms)} "
            f"{banner_once(args.ip, args.port, args.timeout)}",
            flush=True,
        )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(args.interval, remaining))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
