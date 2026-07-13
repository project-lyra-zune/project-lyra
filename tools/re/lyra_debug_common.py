#!/usr/bin/env python3
"""Shared raw-socket Lyra debug and process-control protocol helpers (opcodes 1 to 8 and 26)."""
import argparse
import socket
import struct


NK_PROCESS_LIST_PTR = 0x80BEE010


def parse_u32(value):
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if parsed < 0 or parsed > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in u32")
    return parsed


def format_u32(value):
    return f"0x{value:08x}"


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


def write_process_u32(sock, process_handle, address, value):
    response = request(sock, 4, process_handle, address, value)
    bytes_written = struct.unpack("<I", response[1:5])[0]
    ok = response[5] != 0
    error = struct.unpack("<I", response[6:10])[0]
    return ok, bytes_written, error


def debug_attach(sock, process_id):
    response = request(sock, 5, process_id)
    return struct.unpack("<I", response[1:5])[0] != 0


def debug_detach(sock, process_id):
    """Daemon opcode 26 = DebugActiveProcessStop(process_id).
    Symmetric with debug_attach; lets a host tool release a debug session
    without rebooting the device. Safe to call even if not attached."""
    response = request(sock, 26, process_id)
    return struct.unpack("<I", response[1:5])[0] != 0


def debug_wait(sock):
    response = request(sock, 6)
    ok = struct.unpack("<I", response[1:5])[0] != 0
    code = struct.unpack("<I", response[5:9])[0]
    process_id = struct.unpack("<I", response[9:13])[0]
    thread_id = struct.unpack("<I", response[13:17])[0]
    aux = struct.unpack("<I", response[17:21])[0]
    return ok, code, process_id, thread_id, aux


def debug_continue(sock, process_id, thread_id):
    request(sock, 7, process_id, thread_id)


def read_thread_regs(sock, thread_id):
    response = request(sock, 8, thread_id, response_size=80)
    values = struct.unpack("<7I", response[1:29])
    regs = {
        "r0": values[0],
        "r1": values[1],
        "r2": values[2],
        "r3": values[3],
        "pc": values[4],
        "lr": values[5],
        "sp": values[6],
    }
    extra = struct.unpack("<9I", response[29:65])
    for idx, name in enumerate(
        ("r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12")
    ):
        regs[name] = extra[idx]
    return regs


def read_process_utf16z(sock, process_handle, address, max_chars):
    if address == 0:
        return ""
    chars = []
    for offset in range(0, max_chars * 2, 2):
        ok, value, bytes_read, _error = read_process_u32(
            sock, process_handle, address + offset
        )
        if not ok or bytes_read != 4:
            break
        code_unit = value & 0xFFFF
        if code_unit == 0:
            break
        if 0xD800 <= code_unit <= 0xDFFF:
            chars.append("?")
        else:
            chars.append(chr(code_unit))
    return "".join(chars)


def connect(ip, port, timeout):
    sock = socket.create_connection((ip, port), timeout=timeout)
    sock.settimeout(timeout)
    banner = read_exact(sock, len(b"Hello\n"))
    if banner != b"Hello\n":
        raise RuntimeError(f"unexpected banner: {banner!r}")
    return sock, banner
