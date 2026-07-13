#!/usr/bin/env python3
"""Spawn / stop a background-daemon plugin via nativeapp opcodes 21 / 22.

Unlike opcode 20 (synchronous Run that blocks the REPL), opcode 21 loads the
plugin and runs its daemon entry on a tracked thread, returning a daemon id
immediately; the REPL (1337) stays free while the daemon runs. Opcode 22
signals the daemon's stop event, joins the thread, and FreeLibrary's it.

  spawn: [0]=21 [1-4]=path_len [5-8]=entry_len [9-12]=arg_len + path+entry+arg
         -> [0]=21 [1-4]=daemon_id (0=fail) [9-12]=GetLastError
  stop:  [0]=22 [1-4]=daemon_id
         -> [0]=22 [1-4]=ok(1/0) [9-12]=GetLastError

Daemon entry signature (in the plugin):
  int RunDaemon(const void* arg, int arg_len, HANDLE stop_event)
"""
import argparse
import socket
import struct
import sys
from pathlib import Path


def _connect(ip, port, timeout):
    s = socket.create_connection((ip, port), timeout=timeout)
    s.settimeout(timeout)
    banner = b""
    while b"\n" not in banner:
        ch = s.recv(1)
        if not ch:
            break
        banner += ch
    if banner != b"Hello\n":
        raise RuntimeError(f"unexpected banner: {banner!r}")
    return s


def _recv(s, n):
    out = b""
    while len(out) < n:
        ch = s.recv(n - len(out))
        if not ch:
            break
        out += ch
    return out


def spawn(ip, port, timeout, dev_path, entry, arg):
    path_b = dev_path.encode("utf-8")
    entry_b = entry.encode("ascii")
    if not (1 <= len(path_b) <= 256) or not (1 <= len(entry_b) <= 63) or len(arg) > 256:
        raise SystemExit("bad path/entry/arg length")
    hdr = bytearray(32)
    hdr[0] = 21
    hdr[1:5] = struct.pack("<I", len(path_b))
    hdr[5:9] = struct.pack("<I", len(entry_b))
    hdr[9:13] = struct.pack("<I", len(arg))
    with _connect(ip, port, timeout) as s:
        s.sendall(bytes(hdr))
        s.sendall(path_b)
        s.sendall(entry_b)
        if arg:
            s.sendall(arg)
        resp = _recv(s, 32)
    if len(resp) < 32 or resp[0] != 21:
        raise SystemExit(f"bad response: {resp[:8].hex()}")
    daemon_id = struct.unpack_from("<I", resp, 1)[0]
    err = struct.unpack_from("<I", resp, 9)[0]
    if daemon_id == 0:
        raise SystemExit(f"spawn failed: GetLastError={err}")
    print(f"spawned daemon id={daemon_id}")
    return daemon_id


def stop(ip, port, timeout, daemon_id):
    hdr = bytearray(32)
    hdr[0] = 22
    hdr[1:5] = struct.pack("<I", daemon_id)
    with _connect(ip, port, timeout) as s:
        s.sendall(bytes(hdr))
        resp = _recv(s, 32)
    if len(resp) < 32 or resp[0] != 22:
        raise SystemExit(f"bad response: {resp[:8].hex()}")
    ok = struct.unpack_from("<I", resp, 1)[0]
    err = struct.unpack_from("<I", resp, 9)[0]
    print(f"stop id={daemon_id}: ok={ok} err={err}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sp = sub.add_parser("spawn")
    sp.add_argument("ip")
    sp.add_argument("dev_path", help=r"device path, e.g. \flash2\automation\plugin-screencast.dll")
    sp.add_argument("--entry", default="RunDaemon")
    sp.add_argument("--arg", default=None, help="raw arg bytes via @file, else utf-8")
    st = sub.add_parser("stop")
    st.add_argument("ip")
    st.add_argument("id", type=int)
    for p in (sp, st):
        p.add_argument("--port", type=int, default=1337)
        p.add_argument("--timeout", type=float, default=15.0)
    a = ap.parse_args()
    if a.cmd == "spawn":
        arg = b""
        if a.arg:
            arg = Path(a.arg[1:]).read_bytes() if a.arg.startswith("@") else a.arg.encode("utf-8")
        spawn(a.ip, a.port, a.timeout, a.dev_path, a.entry, arg)
    else:
        stop(a.ip, a.port, a.timeout, a.id)


if __name__ == "__main__":
    main()
