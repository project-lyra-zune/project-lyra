#!/usr/bin/env bash
# Connectivity smoke-test: connect to the Lyra HD listener on port 1337 and confirm its Hello banner.
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <zune-ip>" >&2
  exit 2
fi

ip="$1"
port="1337"

if ! command -v python3 >/dev/null 2>&1; then
  echo "ERROR: python3 is required for the listener probe" >&2
  exit 1
fi

echo "Connecting to ${ip}:${port}..." >&2
hello="$(python3 - "$ip" "$port" <<'PY' || true
import socket
import sys

ip = sys.argv[1]
port = int(sys.argv[2])

with socket.create_connection((ip, port), timeout=3) as sock:
    sock.settimeout(3)
    data = sock.recv(64)
    sys.stdout.buffer.write(data)
PY
)"

if printf '%s' "$hello" | grep -q '^Hello'; then
  printf '%s\n' "$hello"
  echo "Lyra listener responded." >&2
  exit 0
fi

echo "ERROR: did not receive the expected Hello banner" >&2
if [ -n "$hello" ]; then
  printf '%s\n' "$hello" >&2
fi
exit 1
