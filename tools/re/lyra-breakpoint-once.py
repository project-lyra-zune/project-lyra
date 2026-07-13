#!/usr/bin/env python3
"""Set one ARM software breakpoint in a debug-attached process and wait for a single hit via the Lyra debug opcodes."""
import argparse
import sys
import time

from lyra_debug_common import (
    connect,
    debug_attach,
    debug_continue,
    debug_wait,
    find_process,
    format_u32,
    open_process,
    parse_u32,
    read_process_u32,
    read_process_utf16z,
    read_thread_regs,
    write_process_u32,
)


BKPT_ARM = 0xE7F001F0
EXCEPTION_EVENT = 0x00000001


def main():
    parser = argparse.ArgumentParser(
        description="Set one software breakpoint in a debug-attached process and wait for it once."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    parser.add_argument("--process-name", default="ZIE.exe")
    parser.add_argument("--address", type=parse_u32, required=True)
    parser.add_argument(
        "--arg-reg",
        choices=["r0", "r1", "r2", "r3"],
        help="decode a UTF-16 process string from this register on hit",
    )
    parser.add_argument("--string-chars", type=int, default=260)
    parser.add_argument("--skip-attach", action="store_true")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=6.0)
    parser.add_argument("--wait-seconds", type=float, default=60.0)
    parser.add_argument("--max-events", type=int, default=256)
    parser.add_argument("--max-processes", type=int, default=16)
    parser.add_argument("--name-chars", type=int, default=64)
    args = parser.parse_args()

    sock, banner = connect(args.ip, args.port, args.timeout)
    try:
        process_head, proc = find_process(
            sock,
            None,
            args.process_name,
            args.max_processes,
            args.name_chars,
        )
        attached = True
        if not args.skip_attach:
            attached = debug_attach(sock, proc["id"])
        process_handle = open_process(sock, proc["id"])
        if process_handle == 0:
            raise RuntimeError(f"OpenProcess returned null for pid {format_u32(proc['id'])}")
        ok, original, bytes_read, error = read_process_u32(
            sock, process_handle, args.address
        )
        if not ok or bytes_read != 4:
            raise RuntimeError(
                f"ReadProcessMemory failed at {format_u32(args.address)} "
                f"with error {format_u32(error)}"
            )
        ok, bytes_written, error = write_process_u32(
            sock, process_handle, args.address, BKPT_ARM
        )
        if not ok or bytes_written != 4:
            raise RuntimeError(
                f"WriteProcessMemory failed to arm breakpoint at {format_u32(args.address)} "
                f"with error {format_u32(error)}"
            )

        deadline = time.monotonic() + args.wait_seconds
        event_ok = False
        code = 0xFFFFFFFF
        process_id = 0
        thread_id = 0
        aux = 0
        regs = None
        arg_text = None
        seen_events = 0
        continued_events = 0

        while seen_events < args.max_events and time.monotonic() < deadline:
            wait_ok, wait_code, wait_process_id, wait_thread_id, wait_aux = debug_wait(sock)
            if not wait_ok:
                break
            seen_events += 1
            wait_regs = read_thread_regs(sock, wait_thread_id)
            hit = wait_code == EXCEPTION_EVENT and wait_regs["pc"] in (
                args.address,
                args.address + 4,
            )
            if hit:
                event_ok = True
                code = wait_code
                process_id = wait_process_id
                thread_id = wait_thread_id
                aux = wait_aux
                regs = wait_regs
                if args.arg_reg:
                    arg_text = read_process_utf16z(
                        sock, process_handle, regs[args.arg_reg], args.string_chars
                    )
                break
            debug_continue(sock, wait_process_id, wait_thread_id)
            continued_events += 1

        restore_ok, restore_bytes, restore_error = write_process_u32(
            sock, process_handle, args.address, original
        )
        if event_ok:
            debug_continue(sock, process_id, thread_id)
    finally:
        sock.close()

    print(f"banner={banner.decode().strip()}")
    print(f"process_list_head={format_u32(process_head)}")
    print(
        "process="
        f"{format_u32(proc['proc_ptr'])},"
        f"id={format_u32(proc['id'])},"
        f"base={format_u32(proc['base'])},"
        f"name={proc['name']}"
    )
    print(f"attached={'yes' if attached else 'no'}")
    print(f"process_handle={format_u32(process_handle)}")
    print(f"breakpoint_address={format_u32(args.address)}")
    print(f"original_value={format_u32(original)}")
    print(f"event_available={'yes' if event_ok else 'no'}")
    print(f"event_code={format_u32(code)}")
    print(f"event_process_id={format_u32(process_id)}")
    print(f"event_thread_id={format_u32(thread_id)}")
    print(f"event_aux={format_u32(aux)}")
    print(f"seen_events={seen_events}")
    print(f"continued_events={continued_events}")
    if regs is not None:
        print(
            "regs="
            f"r0={format_u32(regs['r0'])},"
            f"r1={format_u32(regs['r1'])},"
            f"r2={format_u32(regs['r2'])},"
            f"r3={format_u32(regs['r3'])},"
            f"pc={format_u32(regs['pc'])},"
            f"lr={format_u32(regs['lr'])},"
            f"sp={format_u32(regs['sp'])}"
        )
    if arg_text is not None:
        print(f"{args.arg_reg}_utf16={arg_text}")
    print(f"restore_ok={'yes' if restore_ok else 'no'}")
    print(f"restore_bytes={restore_bytes}")
    print(f"restore_error={format_u32(restore_error)}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
