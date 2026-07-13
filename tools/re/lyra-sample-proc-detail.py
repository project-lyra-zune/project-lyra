#!/usr/bin/env python3
"""Sample one process's thread and module lists via read-only Lyra kernel-word reads (opcode 1)."""
import argparse
import csv
import socket
import struct
import sys


NK_PROCESS_LIST_PTR = 0x80BEE010


def parse_u32(value):
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if parsed < 0 or parsed > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in u32")
    return parsed


def read_exact(sock, count):
    out = bytearray()
    while len(out) < count:
        chunk = sock.recv(count - len(out))
        if not chunk:
            raise RuntimeError(f"socket closed after {len(out)} of {count} bytes")
        out.extend(chunk)
    return bytes(out)


def read_kernel_u32(sock, address):
    request = bytearray(32)
    request[0] = 1
    request[1:5] = struct.pack("<I", address)
    sock.sendall(request)

    response = read_exact(sock, 32)
    if response[0] != 1:
        raise RuntimeError(f"unexpected response opcode: 0x{response[0]:02x}")
    return struct.unpack("<I", response[1:5])[0]


def read_kernel_u16(sock, address):
    return read_kernel_u32(sock, address) & 0xFFFF


def read_utf16z(sock, address, max_chars):
    if address == 0:
        return ""

    chars = []
    for offset in range(0, max_chars * 2, 2):
        code_unit = read_kernel_u16(sock, address + offset)
        if code_unit == 0:
            break
        if 0xD800 <= code_unit <= 0xDFFF:
            chars.append("?")
        else:
            chars.append(chr(code_unit))
    return "".join(chars)


def format_u32(value):
    return f"0x{value:08x}"


def read_proc(sock, address, name_chars):
    name_ptr = read_kernel_u32(sock, address + 0x20)
    return {
        "proc_ptr": address,
        "next": read_kernel_u32(sock, address + 0x00),
        "last": read_kernel_u32(sock, address + 0x04),
        "id": read_kernel_u32(sock, address + 0x0C),
        "thread_next": read_kernel_u32(sock, address + 0x10),
        "thread_last": read_kernel_u32(sock, address + 0x14),
        "base": read_kernel_u32(sock, address + 0x18),
        "name_ptr": name_ptr,
        "name": read_utf16z(sock, name_ptr, name_chars),
    }


def find_process(sock, requested_ptr, requested_name, max_processes, name_chars):
    head = read_kernel_u32(sock, NK_PROCESS_LIST_PTR)
    current = head
    seen = set()

    for _ in range(max_processes):
        if current == 0 or current in seen:
            break
        seen.add(current)

        proc = read_proc(sock, current, name_chars)
        if requested_ptr is not None and proc["proc_ptr"] == requested_ptr:
            return head, proc
        if requested_name is not None and proc["name"].lower() == requested_name.lower():
            return head, proc

        current = proc["next"]
        if current == head:
            break

    target = format_u32(requested_ptr) if requested_ptr is not None else requested_name
    raise RuntimeError(f"process not found within {max_processes} entries: {target}")


def walk_threads(sock, thread_head, max_threads):
    rows = []
    seen = set()
    current = thread_head
    looped = False
    truncated = False

    for _ in range(max_threads):
        if current == 0:
            break
        if current in seen:
            looped = current == thread_head
            truncated = current != thread_head
            break
        seen.add(current)

        row = {
            "thread_ptr": current,
            "next": read_kernel_u32(sock, current + 0x00),
            "base": read_kernel_u32(sock, current + 0x20),
        }
        rows.append(row)
        current = row["next"]
        if current == thread_head:
            looped = True
            break
    else:
        truncated = current != thread_head
        looped = current == thread_head

    return rows, looped, truncated


def walk_modules(sock, proc_ptr, max_modules, name_chars):
    module_list = read_kernel_u32(sock, proc_ptr + 0x108)
    module_head = read_kernel_u32(sock, module_list + 0x08) if module_list != 0 else 0
    rows = []
    seen = set()
    current = module_head
    looped = False
    truncated = False

    for _ in range(max_modules):
        if current == 0:
            break
        if current in seen:
            looped = current == module_head
            truncated = current != module_head
            break
        seen.add(current)

        name_ptr = read_kernel_u32(sock, current + 0x70)
        row = {
            "module_ptr": current,
            "next": read_kernel_u32(sock, current + 0x00),
            "name_ptr": name_ptr,
            "name": read_utf16z(sock, name_ptr, name_chars),
        }
        rows.append(row)
        current = row["next"]
        if current == module_head:
            looped = True
            break
    else:
        truncated = current != module_head
        looped = current == module_head

    return module_list, module_head, rows, looped, truncated


def print_process(proc):
    print("process")
    print("proc_ptr,next,last,id,base,thread_next,thread_last,name_ptr,name")
    writer = csv.DictWriter(
        sys.stdout,
        fieldnames=[
            "proc_ptr",
            "next",
            "last",
            "id",
            "base",
            "thread_next",
            "thread_last",
            "name_ptr",
            "name",
        ],
    )
    writer.writerow(
        {
            "proc_ptr": format_u32(proc["proc_ptr"]),
            "next": format_u32(proc["next"]),
            "last": format_u32(proc["last"]),
            "id": format_u32(proc["id"]),
            "base": format_u32(proc["base"]),
            "thread_next": format_u32(proc["thread_next"]),
            "thread_last": format_u32(proc["thread_last"]),
            "name_ptr": format_u32(proc["name_ptr"]),
            "name": proc["name"],
        }
    )


def print_threads(rows, looped, truncated):
    print("threads")
    print(f"looped={'yes' if looped else 'no'}")
    print(f"truncated={'yes' if truncated else 'no'}")
    writer = csv.DictWriter(sys.stdout, fieldnames=["index", "thread_ptr", "next", "base"])
    writer.writeheader()
    for index, row in enumerate(rows):
        writer.writerow(
            {
                "index": index,
                "thread_ptr": format_u32(row["thread_ptr"]),
                "next": format_u32(row["next"]),
                "base": format_u32(row["base"]),
            }
        )


def print_modules(module_list, module_head, rows, looped, truncated):
    print("modules")
    print(f"module_list={format_u32(module_list)}")
    print(f"module_head={format_u32(module_head)}")
    print(f"looped={'yes' if looped else 'no'}")
    print(f"truncated={'yes' if truncated else 'no'}")
    writer = csv.DictWriter(
        sys.stdout, fieldnames=["index", "module_ptr", "next", "name_ptr", "name"]
    )
    writer.writeheader()
    for index, row in enumerate(rows):
        writer.writerow(
            {
                "index": index,
                "module_ptr": format_u32(row["module_ptr"]),
                "next": format_u32(row["next"]),
                "name_ptr": format_u32(row["name_ptr"]),
                "name": row["name"],
            }
        )


def main():
    parser = argparse.ArgumentParser(
        description="Sample one Zune HD process's threads and modules using read-only Lyra reads."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--process-name", help="case-insensitive process name to sample")
    target.add_argument("--process-ptr", type=parse_u32, help="process pointer to sample")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--max-processes", type=int, default=16)
    parser.add_argument("--max-threads", type=int, default=16)
    parser.add_argument("--max-modules", type=int, default=32)
    parser.add_argument("--name-chars", type=int, default=64)
    args = parser.parse_args()

    for option, value in (
        ("--max-processes", args.max_processes),
        ("--max-threads", args.max_threads),
        ("--max-modules", args.max_modules),
        ("--name-chars", args.name_chars),
    ):
        if value < 1:
            parser.error(f"{option} must be at least 1")

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        banner = read_exact(sock, len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")

        head, proc = find_process(
            sock,
            args.process_ptr,
            args.process_name,
            args.max_processes,
            args.name_chars,
        )
        threads, threads_looped, threads_truncated = walk_threads(
            sock, proc["thread_next"], args.max_threads
        )
        module_list, module_head, modules, modules_looped, modules_truncated = walk_modules(
            sock, proc["proc_ptr"], args.max_modules, args.name_chars
        )

        print(f"banner={banner.decode().strip()}")
        print(f"process_list_head={format_u32(head)}")
        print_process(proc)
        print_threads(threads, threads_looped, threads_truncated)
        print_modules(
            module_list, module_head, modules, modules_looped, modules_truncated
        )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
