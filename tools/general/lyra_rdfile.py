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


def make_rdfile_request(path):
    payload = encode_field_bytes(1, path.encode("utf-8"))
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
    max_bytes,
    sink=None,
    progress_interval=0,
    resume_bytes=0,
):
    sock.settimeout(timeout)
    sock.sendall(make_rdfile_request(path))

    pending = bytearray()
    chunks = [] if sink is None else None
    full_size = None
    bytes_written_total = resume_bytes
    bytes_seen_total = 0
    next_progress = 0
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
                chunk = sock.recv(4096)
                if not chunk:
                    raise RuntimeError("socket closed while reading file")
                if legacy_check_pending and not pending and chunk[0] in _LEGACY_ERROR_LEAD_BYTES:
                    raise RuntimeError(parse_legacy_error_bytes(chunk))
                legacy_check_pending = False
                pending.extend(chunk)

        consumed = message["consumed"]
        del pending[:consumed]

        if message["kind"] == "eof":
            data = b"" if chunks is None else b"".join(chunks)
            total_len = bytes_written_total if sink is not None else len(data)
            if full_size is not None and total_len != full_size:
                if sink is not None and total_len == full_size:
                    return data, full_size
                raise RuntimeError(
                    f"short read for {path}: got {total_len} bytes, expected {full_size}"
                )
            return data, full_size if full_size is not None else total_len

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
            if full_size > max_bytes:
                raise RuntimeError(
                    f"{path} is {full_size} bytes, exceeds --max-bytes {max_bytes}"
                )
            if resume_bytes > full_size:
                raise RuntimeError(
                    f"resume offset {resume_bytes} exceeds file size {full_size}"
                )
            if progress_interval > 0 and resume_bytes > 0:
                next_progress = ((resume_bytes // progress_interval) + 1) * progress_interval
        elif full_size != message["full_size"]:
            raise RuntimeError(
                f"{path} size changed from {full_size} to {message['full_size']}"
            )
        bytes_seen_total += len(chunk)
        if resume_bytes > 0 and bytes_seen_total <= resume_bytes:
            continue
        if resume_bytes > 0 and bytes_seen_total - len(chunk) < resume_bytes:
            skip = resume_bytes - (bytes_seen_total - len(chunk))
            chunk = chunk[skip:]
        if sink is not None:
            sink.write(chunk)
            bytes_written_total += len(chunk)
            if progress_interval > 0 and full_size is not None:
                while bytes_written_total >= next_progress:
                    if next_progress == 0:
                        next_progress = progress_interval
                        continue
                    elapsed = time.monotonic() - started_at
                    print(
                        f"progress={bytes_written_total}/{full_size} "
                        f"elapsed={elapsed:.1f}s",
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
