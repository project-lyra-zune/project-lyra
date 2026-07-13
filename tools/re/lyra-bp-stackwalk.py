#!/usr/bin/env python3
"""Set one software breakpoint, capture full regs + a stack dump on hit.

For RE'ing call chains where we want to know "who called this function?".
The daemon only exposes r0-r3/pc/lr/sp via read_thread_regs, so we manually
read N words from sp and decode any that look like .text return addresses.
"""
from __future__ import annotations

import argparse
import struct
import sys
import time

from lyra_debug_common import (
    connect,
    debug_attach,
    debug_continue,
    debug_detach,
    debug_wait,
    find_process,
    format_u32,
    open_process,
    parse_u32,
    read_process_u32,
    read_thread_regs,
    write_process_u32,
)


BKPT_ARM = 0xE7F001F0
EXCEPTION_EVENT = 0x00000001


def is_text_addr(v):
    # gemstone .text spans roughly 0x00010000..0x000a0000 in its process.
    # xuidll typical load 0x41820000..0x41880000.
    # coredll typical load 0x42000000..0x42100000.
    # zuxhook typical load 0x41c50000..0x41c60000.
    if 0x00010000 <= v < 0x000a0000: return "gemstone"
    if 0x41820000 <= v < 0x41880000: return "xuidll"
    if 0x41c50000 <= v < 0x41c70000: return "zuxhook"
    if 0x42000000 <= v < 0x42100000: return "coredll"
    return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("ip")
    p.add_argument("--process-name", default="gemstone.exe")
    p.add_argument("--address", type=parse_u32, required=True,
                   help="VA to breakpoint")
    p.add_argument("--stack-words", type=int, default=64,
                   help="words from sp to dump on hit (default 64 = 256 bytes)")
    p.add_argument("--dump-at-lr", type=int, default=0,
                   help="if >0, dump this many words at *lr (e.g. for scene `this` pointer)")
    p.add_argument("--dump-at-r1", type=int, default=0,
                   help="if >0, dump this many words at *r1 (e.g. the mouse-message struct)")
    p.add_argument("--dump-at-r0", type=int, default=0,
                   help="if >0, dump this many words at *r0 (e.g. the factory buildSpec)")
    p.add_argument("--deref-r1", type=parse_u32, default=None,
                   help="if set, follow the u32 at [r1+OFFSET] and dump --deref-words words there")
    p.add_argument("--deref-words", type=int, default=16,
                   help="words to dump at the --deref-r1 target (default 16)")
    p.add_argument("--port", type=int, default=1337)
    p.add_argument("--timeout", type=float, default=6.0)
    p.add_argument("--wait-seconds", type=float, default=120.0)
    p.add_argument("--max-events", type=int, default=512)
    p.add_argument("--max-processes", type=int, default=16)
    p.add_argument("--name-chars", type=int, default=64)
    p.add_argument("--skip-attach", action="store_true")
    p.add_argument("--break-on-r1x", type=parse_u32, default=None,
                   help="only break when the u32 at [r1] equals this (e.g. "
                        "0x43430000 = 195.0f position x); skip+re-arm otherwise")
    p.add_argument("--dump", action="append", default=[],
                   metavar="REG:WORDS",
                   help="dump WORDS u32s at the value of REG (e.g. r5:32). "
                        "Repeatable. REG is any of r0-r12/lr/sp/pc.")
    args = p.parse_args()

    sock, banner = connect(args.ip, args.port, args.timeout)
    attached_pid = None
    bp_armed_addr = None
    orig_opcode = None
    ph = None
    try:
        process_head, proc = find_process(
            sock, None, args.process_name,
            args.max_processes, args.name_chars,
        )
        print(f"process: {proc['name']} id=0x{proc['id']:08x}")
        ph = open_process(sock, proc["id"])
        if not ph:
            print("open_process failed"); return 1

        if not args.skip_attach:
            ok = debug_attach(sock, proc["id"])
            print(f"debug_attach -> {ok}")
            if not ok:
                print("attach failed"); return 1
            attached_pid = proc["id"]
            # Drain initial events (process create / module loads)
            for _ in range(args.max_events):
                ok, code, pid, tid, aux = debug_wait(sock)
                if not ok: break
                if code == 0: continue  # nothing
                debug_continue(sock, pid, tid)
                if code == EXCEPTION_EVENT: break

        # Save original opcode, write breakpoint
        ok, orig, _, _ = read_process_u32(sock, ph, args.address)
        print(f"orig opcode @ 0x{args.address:08x} = 0x{orig:08x}")
        if not ok:
            print("read_process_u32 failed"); return 1
        ok, _, _ = write_process_u32(sock, ph, args.address, BKPT_ARM)
        if not ok:
            print("write breakpoint failed"); return 1
        bp_armed_addr = args.address
        orig_opcode = orig
        print(f"breakpoint set @ 0x{args.address:08x}")

        # Wait for hit
        end = time.monotonic() + args.wait_seconds
        hit = False
        while time.monotonic() < end:
            ok, code, pid, tid, aux = debug_wait(sock)
            if not ok:
                time.sleep(0.05); continue
            if code == 0:
                time.sleep(0.05); continue
            if code == EXCEPTION_EVENT:
                regs = read_thread_regs(sock, tid)
                # Value filter: skip writes whose [r1] (e.g. position x) doesn't
                # match, re-arming after each so we keep catching until the
                # target value appears. No single-step in the daemon, so re-arm
                # = restore orig, continue past, brief sleep, re-write BKPT.
                if args.break_on_r1x is not None:
                    okx, xval, _, _ = read_process_u32(sock, ph, regs['r1'])
                    if (not okx) or xval != args.break_on_r1x:
                        write_process_u32(sock, ph, args.address, orig)
                        debug_continue(sock, pid, tid)
                        time.sleep(0.003)
                        write_process_u32(sock, ph, args.address, BKPT_ARM)
                        continue
                print(f"\n--- HIT ---  tid=0x{tid:08x}")
                print(f"r0 = 0x{regs['r0']:08x}   r1 = 0x{regs['r1']:08x}")
                print(f"r2 = 0x{regs['r2']:08x}   r3 = 0x{regs['r3']:08x}")
                print(f"r4 = 0x{regs['r4']:08x}   r5 = 0x{regs['r5']:08x}")
                print(f"r6 = 0x{regs['r6']:08x}   r7 = 0x{regs['r7']:08x}")
                print(f"r8 = 0x{regs['r8']:08x}   r9 = 0x{regs['r9']:08x}")
                print(f"r10= 0x{regs['r10']:08x}   r11= 0x{regs['r11']:08x}")
                print(f"r12= 0x{regs['r12']:08x}")
                print(f"pc = 0x{regs['pc']:08x}   lr = 0x{regs['lr']:08x}")
                print(f"sp = 0x{regs['sp']:08x}")
                print(f"\n--- STACK ({args.stack_words} words from sp) ---")
                for i in range(args.stack_words):
                    addr = regs['sp'] + i*4
                    ok, val, _, _ = read_process_u32(sock, ph, addr)
                    if not ok:
                        print(f"  sp+0x{i*4:03x}: <read failed>"); break
                    tag = is_text_addr(val)
                    suffix = f"  [{tag}]" if tag else ""
                    print(f"  sp+0x{i*4:03x} (0x{addr:08x}): 0x{val:08x}{suffix}")
                # Also dump memory at lr (likely the scene `this` pointer)
                if args.dump_at_lr > 0:
                    print(f"\n--- INSTANCE at lr=0x{regs['lr']:08x} ({args.dump_at_lr} words) ---")
                    for i in range(args.dump_at_lr):
                        addr = regs['lr'] + i*4
                        ok, val, _, _ = read_process_u32(sock, ph, addr)
                        if not ok:
                            print(f"  +0x{i*4:03x}: <read failed>"); break
                        tag = is_text_addr(val)
                        suffix = f"  [{tag}]" if tag else ""
                        print(f"  lr+0x{i*4:03x} (0x{addr:08x}): 0x{val:08x}{suffix}")
                if args.dump_at_r0 > 0:
                    print(f"\n--- STRUCT at r0=0x{regs['r0']:08x} ({args.dump_at_r0} words) ---")
                    for i in range(args.dump_at_r0):
                        addr = regs['r0'] + i*4
                        ok, val, _, _ = read_process_u32(sock, ph, addr)
                        if not ok:
                            print(f"  +0x{i*4:03x}: <read failed>"); break
                        tag = is_text_addr(val)
                        suffix = f"  [{tag}]" if tag else ""
                        print(f"  r0+0x{i*4:03x} (0x{addr:08x}): 0x{val:08x}{suffix}")
                if args.dump_at_r1 > 0:
                    print(f"\n--- STRUCT at r1=0x{regs['r1']:08x} ({args.dump_at_r1} words) ---")
                    for i in range(args.dump_at_r1):
                        addr = regs['r1'] + i*4
                        ok, val, _, _ = read_process_u32(sock, ph, addr)
                        if not ok:
                            print(f"  +0x{i*4:03x}: <read failed>"); break
                        tag = is_text_addr(val)
                        suffix = f"  [{tag}]" if tag else ""
                        print(f"  r1+0x{i*4:03x} (0x{addr:08x}): 0x{val:08x}{suffix}")
                if args.deref_r1 is not None:
                    ok, ptr, _, _ = read_process_u32(sock, ph, regs['r1'] + args.deref_r1)
                    print(f"\n--- DEREF [r1+0x{args.deref_r1:x}] = 0x{ptr:08x} ({args.deref_words} words) ---")
                    if ok and ptr:
                        for i in range(args.deref_words):
                            addr = ptr + i*4
                            ok2, val, _, _ = read_process_u32(sock, ph, addr)
                            if not ok2:
                                print(f"  +0x{i*4:03x}: <read failed>"); break
                            tag = is_text_addr(val)
                            suffix = f"  [{tag}]" if tag else ""
                            print(f"  +0x{i*4:03x} (0x{addr:08x}): 0x{val:08x}{suffix}")
                for spec in args.dump:
                    reg, _, wstr = spec.partition(":")
                    reg = reg.strip().lower()
                    words = int(wstr) if wstr else 16
                    base = regs.get(reg)
                    if base is None:
                        print(f"\n--- DUMP {spec}: unknown reg '{reg}' ---")
                        continue
                    print(f"\n--- DUMP at {reg}=0x{base:08x} ({words} words) ---")
                    for i in range(words):
                        addr = base + i*4
                        ok, val, _, _ = read_process_u32(sock, ph, addr)
                        if not ok:
                            print(f"  +0x{i*4:03x}: <read failed>"); break
                        tag = is_text_addr(val)
                        suffix = f"  [{tag}]" if tag else ""
                        print(f"  {reg}+0x{i*4:03x} (0x{addr:08x}): 0x{val:08x}{suffix}")
                hit = True
                # Restore opcode + continue. Cleanup of orig/detach happens
                # in the finally block.
                write_process_u32(sock, ph, args.address, orig)
                bp_armed_addr = None  # already restored
                debug_continue(sock, pid, tid)
                break
            else:
                debug_continue(sock, pid, tid)
        if not hit:
            print(f"\nTimed out after {args.wait_seconds}s without hitting bp.")
    finally:
        # Guarantee opcode restoration + detach even on exception / Ctrl-C.
        # Without this, an interrupted bp leaves a software breakpoint in
        # gemstone (next thread to hit that address crashes) and a stuck
        # debug session in the daemon (next attach attempt fails until
        # reboot).
        try:
            if bp_armed_addr is not None and orig_opcode is not None and ph is not None:
                write_process_u32(sock, ph, bp_armed_addr, orig_opcode)
                print(f"cleanup: restored opcode @ 0x{bp_armed_addr:08x}")
        except Exception as e:
            print(f"cleanup: opcode restore failed: {e}")
        try:
            if attached_pid is not None:
                ok = debug_detach(sock, attached_pid)
                print(f"cleanup: debug_detach -> {ok}")
        except Exception as e:
            print(f"cleanup: detach failed: {e}")
        try: sock.close()
        except Exception: pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
