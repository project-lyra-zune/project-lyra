#!/usr/bin/env python3
"""Scan a process address space for readable page ranges via Lyra opcodes 2 and 3 (OpenProcess, ReadProcessMemory)."""
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


def request(sock, opcode, *values, response_size=32):
    payload = bytearray(32)
    payload[0] = opcode
    offset = 1
    for value in values:
        payload[offset : offset + 4] = struct.pack("<I", value)
        offset += 4
    sock.sendall(payload)
    response = read_exact(sock, response_size)
    if response[0] != opcode:
        raise RuntimeError(
            f"unexpected response opcode: got 0x{response[0]:02x}, expected 0x{opcode:02x}"
        )
    return response


def read_kernel_u32(sock, address):
    response = request(sock, 1, address)
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
        "id": read_kernel_u32(sock, address + 0x0C),
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


def open_process(sock, process_id):
    response = request(sock, 2, process_id)
    return struct.unpack("<I", response[1:5])[0]


def read_process_u32(sock, process_handle, address):
    response = request(sock, 3, process_handle, address)
    value = struct.unpack("<I", response[1:5])[0]
    bytes_read = struct.unpack("<I", response[5:9])[0]
    ok = response[9] != 0
    error = struct.unpack("<I", response[10:14])[0]
    return ok, value, bytes_read, error


def add_range(ranges, start, end):
    if start is not None and end >= start:
        ranges.append((start, end))


def scan_pages(sock, process_handle, start, end, step):
    ranges = []
    current_start = None
    last_good = None
    first_values = {}
    failures = {}

    address = start
    while address < end:
        ok, value, bytes_read, error = read_process_u32(sock, process_handle, address)
        if ok and bytes_read == 4:
            if current_start is None:
                current_start = address
                first_values[address] = value
            last_good = address
        else:
            failures[address] = error
            if current_start is not None:
                add_range(ranges, current_start, last_good)
                current_start = None
                last_good = None
        address += step

    if current_start is not None:
        add_range(ranges, current_start, last_good)

    return ranges, first_values, failures


def main():
    parser = argparse.ArgumentParser(
        description="Scan readable process pages through Lyra OpenProcess/ReadProcessMemory."
    )
    parser.add_argument("ip", help="Zune HD IP address")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--process-name", help="case-insensitive process name to scan")
    target.add_argument("--process-ptr", type=parse_u32, help="process pointer to scan")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--max-processes", type=int, default=16)
    parser.add_argument("--name-chars", type=int, default=64)
    parser.add_argument("--start", type=parse_u32, default=0)
    parser.add_argument("--end", type=parse_u32, default=0x00800000)
    parser.add_argument("--step", type=parse_u32, default=0x1000)
    args = parser.parse_args()

    if args.max_processes < 1:
        parser.error("--max-processes must be at least 1")
    if args.name_chars < 1:
        parser.error("--name-chars must be at least 1")
    if args.step == 0:
        parser.error("--step must be non-zero")
    if args.end <= args.start:
        parser.error("--end must be greater than --start")

    with socket.create_connection((args.ip, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        banner = read_exact(sock, len(b"Hello\n"))
        if banner != b"Hello\n":
            raise RuntimeError(f"unexpected banner: {banner!r}")

        process_head, proc = find_process(
            sock,
            args.process_ptr,
            args.process_name,
            args.max_processes,
            args.name_chars,
        )
        handle = open_process(sock, proc["id"])
        if handle == 0:
            raise RuntimeError(f"OpenProcess returned null for pid {format_u32(proc['id'])}")

        ranges, first_values, failures = scan_pages(
            sock, handle, args.start, args.end, args.step
        )

        print(f"banner={banner.decode().strip()}")
        print(f"process_list_head={format_u32(process_head)}")
        print(
            "process="
            f"{format_u32(proc['proc_ptr'])},"
            f"id={format_u32(proc['id'])},"
            f"base={format_u32(proc['base'])},"
            f"name={proc['name']}"
        )
        print(f"process_handle={format_u32(handle)}")
        print(f"scan_start={format_u32(args.start)}")
        print(f"scan_end={format_u32(args.end)}")
        print(f"scan_step={format_u32(args.step)}")
        print(f"range_count={len(ranges)}")

        writer = csv.DictWriter(
            sys.stdout, fieldnames=["index", "start", "end", "pages", "first_u32"]
        )
        writer.writeheader()
        for index, (range_start, range_end) in enumerate(ranges):
            pages = ((range_end - range_start) // args.step) + 1
            writer.writerow(
                {
                    "index": index,
                    "start": format_u32(range_start),
                    "end": format_u32(range_end),
                    "pages": pages,
                    "first_u32": format_u32(first_values.get(range_start, 0)),
                }
            )

        distinct_errors = sorted(set(failures.values()))
        if distinct_errors:
            print(
                "failure_errors="
                + ",".join(format_u32(error) for error in distinct_errors)
            )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
