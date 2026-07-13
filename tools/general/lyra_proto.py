#!/usr/bin/env python3
"""Protobuf proto2/nanopb varint and field codec plus legacy-error decoding for the Lyra file/RPC protocol."""
from __future__ import annotations


LEGACY_ERROR_MARKERS = {
    0xCD: "pb_decode_error",
    0xCE: "lsdir_findfirstfile_error",
    0xCF: "pb_encode_lsdir_error",
    0xDE: "pb_encode_eof_error",
    0xDF: "pb_encode_rdfile_error",
    0xFF: "unknown_command_error",
}


def encode_varint(value: int) -> bytes:
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def decode_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        if offset >= len(data):
            raise ValueError("truncated varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7
        if shift > 35:
            raise ValueError("varint too long")


def encode_field_varint(field: int, value: int) -> bytes:
    return encode_varint((field << 3) | 0) + encode_varint(value)


def encode_field_bytes(field: int, value: bytes) -> bytes:
    return encode_varint((field << 3) | 2) + encode_varint(len(value)) + value


def decode_resp_err_payload(payload: bytes) -> dict[str, object]:
    offset = 0
    code = None
    msg = None
    while offset < len(payload):
        key, offset = decode_varint(payload, offset)
        field = key >> 3
        wire_type = key & 7
        if field == 1 and wire_type == 0:
            code, offset = decode_varint(payload, offset)
        elif field == 2 and wire_type == 2:
            size, offset = decode_varint(payload, offset)
            if offset + size > len(payload):
                raise ValueError("truncated bytes")
            msg = payload[offset : offset + size].decode("utf-8", errors="replace")
            offset += size
        else:
            raise ValueError(f"unexpected RespErr field {field} wire {wire_type}")
    return {"code": code, "msg": msg}


def parse_legacy_error_bytes(chunk: bytes) -> str:
    marker = chunk[0]
    label = LEGACY_ERROR_MARKERS.get(marker, f"marker_0x{marker:02x}")
    detail = ""
    if len(chunk) > 2:
        raw = chunk[2:]
        try:
            detail = raw.decode("utf-16le", errors="ignore").split("\x00", 1)[0]
        except Exception:
            detail = ""
    if detail:
        return f"{label}: {detail}"
    return label

