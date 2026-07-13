"""Lyra read-file (protobuf command 0x10) wire core.

Shared by `lyra-read-file.py` (the CLI) and the modkit deploy path,
which reads small device files (e.g. enabled.json) to merge rather than
clobber them. Single source of truth for the rdfile framing.
"""
import socket
import sys
import time

from lyra_proto import (
    decode_resp_err_payload,
    decode_varint,
    encode_field_bytes,
    encode_field_varint,
    parse_legacy_error_bytes,
)


CMD_RDFILE = 2
RSP_RDFILE_DATA = 2
RSP_RDFILE_EOF = 3
RSP_ERR = 4

_LEGACY_ERROR_LEAD_BYTES = (0xCD, 0xCE, 0xCF, 0xDE, 0xDF, 0xFF)


class FileTooLarge(Exception):
    """File size exceeds max_bytes; a distinct type so a retrying caller skips it."""

    def __init__(self, path, size):
        super().__init__(f"{path} is {size} bytes, exceeds max_bytes")
        self.size = size


def make_rdfile_request(path, offset=0, length=0):
    """length 0 = read to EOF."""
    payload = encode_field_bytes(1, path.encode("utf-8"))
    if offset:
        payload += encode_field_varint(2, offset)
    if length:
        payload += encode_field_varint(3, length)
    message = encode_field_varint(1, CMD_RDFILE)
    message += encode_field_bytes(3, payload)
    return bytes([16]) + message


def parse_rdfile_payload(payload):
    offset = 0
    data = b""
    full_size = None

    while offset < len(payload):
        key, offset = decode_varint(payload, offset)
        field = key >> 3
        wire_type = key & 7
        if field == 1 and wire_type == 2:
            size, offset = decode_varint(payload, offset)
            if offset + size > len(payload):
                raise ValueError("truncated bytes")
            data = payload[offset : offset + size]
            offset += size
        elif field == 2 and wire_type == 0:
            full_size, offset = decode_varint(payload, offset)
        else:
            raise ValueError(f"unexpected RespRdfile field {field} wire {wire_type}")

    if full_size is None:
        raise ValueError("missing fullsz")
    return data, full_size


def parse_one_response(data):
    offset = 0
    cmd = None

    while offset < len(data):
        key_start = offset
        key, offset = decode_varint(data, offset)
        field = key >> 3
        wire_type = key & 7

        if field == 1 and wire_type == 0:
            cmd, offset = decode_varint(data, offset)
            continue

        if field == 3 and wire_type == 2:
            size, offset = decode_varint(data, offset)
            if offset + size > len(data):
                raise ValueError("truncated bytes")
            payload = data[offset : offset + size]
            offset += size
            chunk, full_size = parse_rdfile_payload(payload)
            if cmd != RSP_RDFILE_DATA:
                raise ValueError(f"unexpected rdfile data cmd: {cmd}")
            return {
                "kind": "data",
                "data": chunk,
                "full_size": full_size,
                "consumed": offset,
            }

        if field == 4 and wire_type == 2:
            size, offset = decode_varint(data, offset)
            if offset + size > len(data):
                raise ValueError("truncated bytes")
            offset += size
            if cmd != RSP_RDFILE_EOF:
                raise ValueError(f"unexpected eof cmd: {cmd}")
            return {"kind": "eof", "consumed": offset}

        if field == 5 and wire_type == 2:
            size, offset = decode_varint(data, offset)
            if offset + size > len(data):
                raise ValueError("truncated bytes")
            payload = data[offset : offset + size]
            offset += size
            if cmd == RSP_ERR:
                return {
                    "kind": "error",
                    "error": decode_resp_err_payload(payload),
                    "consumed": offset,
                }
            return {"kind": "error", "data": payload, "consumed": offset}

        raise ValueError(
            f"unexpected response field {field} wire {wire_type} at {key_start}"
        )

    raise ValueError("truncated response")


def read_file(
    sock,
    path,
    timeout,
    max_bytes=None,
    sink=None,
    offset=0,
    length=0,
    progress_interval=0,
):
    """Read [offset, offset+length) of a device file (length 0 = to EOF) in one
    request, draining to EOF so the connection stays resynced. Streams to `sink`
    (returns bytes written) or returns the bytes; the second value is the total
    file size. Raises FileTooLarge if the file exceeds max_bytes."""
    sock.settimeout(timeout)
    sock.sendall(make_rdfile_request(path, offset, length))

    pending = bytearray()
    chunks = [] if sink is None else None
    full_size = None
    read_this_request = 0
    next_progress = progress_interval
    started_at = time.monotonic()
    legacy_check_pending = True

    while True:
        while True:
            try:
                message = parse_one_response(bytes(pending))
                break
            except ValueError as exc:
                if "truncated" not in str(exc):
                    raise
                chunk = sock.recv(65536)
                if not chunk:
                    raise RuntimeError("socket closed while reading file")
                if legacy_check_pending and not pending and chunk[0] in _LEGACY_ERROR_LEAD_BYTES:
                    raise RuntimeError(parse_legacy_error_bytes(chunk))
                legacy_check_pending = False
                pending.extend(chunk)

        del pending[:message["consumed"]]

        if message["kind"] == "eof":
            if sink is not None:
                return read_this_request, (full_size if full_size is not None else offset + read_this_request)
            data = b"".join(chunks)
            return data, (full_size if full_size is not None else len(data))

        if message["kind"] == "error":
            err = message.get("error")
            if err is not None:
                raise RuntimeError(
                    f"device returned error code={err.get('code')} msg={err.get('msg')!r}"
                )
            raise RuntimeError(f"device returned rdfile error payload: {message['data']!r}")

        chunk = message["data"]
        if full_size is None:
            full_size = message["full_size"]
            if max_bytes is not None and full_size > max_bytes:
                raise RuntimeError(
                    f"{path} is {full_size} bytes, exceeds max_bytes {max_bytes}"
                )
        elif full_size != message["full_size"]:
            raise RuntimeError(
                f"{path} size changed from {full_size} to {message['full_size']}"
            )

        read_this_request += len(chunk)
        if sink is not None:
            sink.write(chunk)
            if progress_interval > 0:
                pos = offset + read_this_request
                while pos >= next_progress:
                    elapsed = time.monotonic() - started_at
                    print(
                        f"progress={pos}/{full_size} elapsed={elapsed:.1f}s",
                        file=sys.stderr,
                    )
                    next_progress += progress_interval
        else:
            chunks.append(chunk)


def connect(ip, port, timeout):
    """Open a Lyra connection and consume the "Hello\\n" banner."""
    sock = socket.create_connection((ip, port), timeout=timeout)
    banner = sock.recv(len(b"Hello\n"))
    if banner != b"Hello\n":
        sock.close()
        raise RuntimeError(f"unexpected banner: {banner!r}")
    return sock
