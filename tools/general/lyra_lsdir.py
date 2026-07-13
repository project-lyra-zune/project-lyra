"""Lyra lsdir (protobuf command 0x10, CMD_LSDIR 1) streaming wire core.

Shared by lyra-lsdir.py (the CLI), lyra-tree.py, and snapshot-device.py.
Single source of truth for the lsdir framing. Mirrors lyra_rdfile.py.

The daemon streams one CommandResp per entry (RSP_LSDIR_ENT), terminated by
RSP_LSDIR_EOF. Protobuf does not self-frame, so parse_one_response consumes
exactly one message and reports how many bytes it took.
"""
from lyra_proto import (
    decode_resp_err_payload,
    decode_varint,
    encode_field_bytes,
    encode_field_varint,
    parse_legacy_error_bytes,
)


CMD_LSDIR = 1
RSP_LSDIR_ENT = 1
RSP_LSDIR_EOF = 5
RSP_ERR = 4

_LEGACY_ERROR_LEAD_BYTES = (0xCD, 0xCE, 0xCF, 0xDE, 0xDF, 0xFF)


def make_lsdir_request(path):
    payload = encode_field_bytes(1, path.encode("utf-8"))
    message = encode_field_varint(1, CMD_LSDIR)
    message += encode_field_bytes(2, payload)
    return bytes([16]) + message


def parse_lsdir_entry(data):
    offset = 0
    is_dir = None
    path = None
    while offset < len(data):
        key, offset = decode_varint(data, offset)
        field = key >> 3
        wire_type = key & 7
        if field == 1 and wire_type == 0:
            value, offset = decode_varint(data, offset)
            is_dir = value != 0
        elif field == 2 and wire_type == 2:
            size, offset = decode_varint(data, offset)
            if offset + size > len(data):
                raise ValueError(
                    f"truncated lsdir entry path (need {size}, have {len(data)-offset})"
                )
            path = data[offset : offset + size].decode("utf-8", errors="replace")
            offset += size
        else:
            raise ValueError(f"unexpected RespLsdir field {field} wire {wire_type}")
    return {"is_dir": is_dir, "path": path}


def parse_one_response(data):
    """Parse a single CommandResp from the head of `data`. Returns a dict
    with 'kind' plus 'consumed' (bytes taken from the start). Caller advances
    its pending buffer by 'consumed'. Raises ValueError("truncated ...") when
    more bytes are needed."""
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

        if field == 2 and wire_type == 2:
            size, offset = decode_varint(data, offset)
            if offset + size > len(data):
                raise ValueError("truncated lsdir entry")
            payload = data[offset : offset + size]
            offset += size
            entry = parse_lsdir_entry(payload)
            if cmd != RSP_LSDIR_ENT:
                raise ValueError(f"unexpected lsdir entry cmd: {cmd}")
            return {"kind": "ent", "entry": entry, "consumed": offset}

        if field == 4 and wire_type == 2:
            size, offset = decode_varint(data, offset)
            if offset + size > len(data):
                raise ValueError("truncated eof payload")
            offset += size
            if cmd != RSP_LSDIR_EOF:
                raise ValueError(f"unexpected eof cmd: {cmd}")
            return {"kind": "eof", "consumed": offset}

        if field == 5 and wire_type == 2:
            size, offset = decode_varint(data, offset)
            if offset + size > len(data):
                raise ValueError("truncated error payload")
            payload = data[offset : offset + size]
            offset += size
            return {
                "kind": "error",
                "error": decode_resp_err_payload(payload),
                "consumed": offset,
            }

        raise ValueError(
            f"unexpected response field {field} wire {wire_type} at {key_start}"
        )

    raise ValueError("truncated response")


def read_one_response(sock, pending, timeout):
    """Read until exactly one CommandResp is parsed. Returns (resp, new_pending).
    new_pending carries over bytes recv'd past the parsed message boundary."""
    sock.settimeout(timeout)
    while True:
        if pending:
            if pending[0] in _LEGACY_ERROR_LEAD_BYTES:
                raise RuntimeError(parse_legacy_error_bytes(bytes(pending)))
            try:
                resp = parse_one_response(bytes(pending))
                consumed = resp.pop("consumed")
                new_pending = bytearray(pending[consumed:])
                return resp, new_pending
            except ValueError as exc:
                if "truncated" not in str(exc):
                    raise
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("socket closed mid-response")
        pending.extend(chunk)


def normalize_path(path):
    if path.endswith("\\*"):
        return path
    if path.endswith("\\"):
        return path + "*"
    return path + "\\*"


def join_path(base, name):
    if base.endswith("\\"):
        return base + name
    return base + "\\" + name


def list_dir(sock, path, timeout):
    """Yield {'is_dir', 'name', 'full_path'} for each entry under `path`.

    Sends one lsdir request and streams responses until EOF. Raises
    RuntimeError on a device error response. The socket is left drained at
    EOF, so successive calls on one connection are safe.
    """
    query = normalize_path(path)
    sock.sendall(make_lsdir_request(query))
    base = query[:-2] if query.endswith("\\*") else query
    pending = bytearray()
    while True:
        resp, pending = read_one_response(sock, pending, timeout)
        if resp["kind"] == "error":
            err = resp.get("error") or {}
            raise RuntimeError(
                f"device lsdir error code={err.get('code')} msg={err.get('msg')!r}"
            )
        if resp["kind"] == "eof":
            return
        entry = resp["entry"] or {}
        name = entry.get("path") or ""
        yield {
            "is_dir": bool(entry.get("is_dir")),
            "name": name,
            "full_path": join_path(base, name),
        }
