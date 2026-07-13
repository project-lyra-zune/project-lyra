#!/usr/bin/env python3
"""Read N words from a process address (requires prior debug_attach)."""
import argparse, sys
from lyra_debug_common import connect, find_process, open_process, read_process_u32, parse_u32

def is_text_addr(v):
    if 0x00010000 <= v < 0x000a0000: return "gemstone"
    if 0x41820000 <= v < 0x41880000: return "xuidll"
    if 0x41c50000 <= v < 0x41c70000: return "zuxhook"
    if 0x42000000 <= v < 0x42100000: return "coredll"
    return None

def main():
    p = argparse.ArgumentParser()
    p.add_argument("ip")
    p.add_argument("--process-name", default="gemstone.exe")
    p.add_argument("--address", type=parse_u32, required=True)
    p.add_argument("--words", type=int, default=16)
    p.add_argument("--port", type=int, default=1337)
    p.add_argument("--timeout", type=float, default=6.0)
    args = p.parse_args()
    sock, _ = connect(args.ip, args.port, args.timeout)
    _, proc = find_process(sock, None, args.process_name, 16, 64)
    ph = open_process(sock, proc["id"])
    print(f"--- mem at 0x{args.address:08x} ({args.words} words) ---")
    for i in range(args.words):
        addr = args.address + i*4
        ok, val, _, _ = read_process_u32(sock, ph, addr)
        if not ok: print(f"  +0x{i*4:03x}: <read failed>"); break
        tag = is_text_addr(val)
        suffix = f"  [{tag}]" if tag else ""
        print(f"  +0x{i*4:03x} (0x{addr:08x}): 0x{val:08x}{suffix}")
    sock.close()

if __name__ == "__main__":
    main()
