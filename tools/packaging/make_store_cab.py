#!/usr/bin/env python3
"""Create a minimal uncompressed Microsoft Cabinet file.

The XNA `.ccgame` format is a Cabinet with `XCabInfo.resources` plus numeric
payload members. This writer intentionally supports only the subset we need:
one folder, no compression, no reserved areas, and one cabinet.
"""

from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import struct
import sys


MAX_CFDATA = 32768


def dos_datetime(now: dt.datetime) -> tuple[int, int]:
    year = max(1980, min(now.year, 2107))
    date = ((year - 1980) << 9) | (now.month << 5) | now.day
    time = (now.hour << 11) | (now.minute << 5) | (now.second // 2)
    return date, time


def parse_member(value: str) -> tuple[str, pathlib.Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("members must be NAME=PATH")
    name, path = value.split("=", 1)
    if not name:
        raise argparse.ArgumentTypeError("member name cannot be empty")
    source = pathlib.Path(path)
    if not source.is_file():
        raise argparse.ArgumentTypeError(f"member source is not a file: {source}")
    if "\0" in name or "/" in name or "\\" in name:
        raise argparse.ArgumentTypeError("member name must be a flat CAB filename")
    return name, source


def build_cab(members: list[tuple[str, pathlib.Path]]) -> bytes:
    now = dt.datetime.now()
    dos_date, dos_time = dos_datetime(now)

    file_records = bytearray()
    data = bytearray()
    offset = 0

    for name, path in members:
        payload = path.read_bytes()
        file_records += struct.pack(
            "<IIHHHH",
            len(payload),
            offset,
            0,  # iFolder
            dos_date,
            dos_time,
            0x20,  # archive attribute
        )
        file_records += name.encode("ascii") + b"\0"
        data += payload
        offset += len(payload)

    chunks = [data[i : i + MAX_CFDATA] for i in range(0, len(data), MAX_CFDATA)]
    cfdata = bytearray()
    for chunk in chunks:
        cfdata += struct.pack("<IHH", 0, len(chunk), len(chunk))
        cfdata += chunk

    header_size = 36
    cffolder_size = 8
    coff_files = header_size + cffolder_size
    coff_cab_start = coff_files + len(file_records)
    cab_size = coff_cab_start + len(cfdata)

    header = bytearray()
    header += b"MSCF"
    header += struct.pack("<I", 0)  # reserved1
    header += struct.pack("<I", cab_size)
    header += struct.pack("<I", 0)  # reserved2
    header += struct.pack("<I", coff_files)
    header += struct.pack("<I", 0)  # reserved3
    header += struct.pack("<BB", 3, 1)  # version minor, major
    header += struct.pack("<HH", 1, len(members))
    header += struct.pack("<H", 0)  # flags
    header += struct.pack("<HH", 0, 0)  # setID, iCabinet

    folder = struct.pack("<IHH", coff_cab_start, len(chunks), 0)
    return bytes(header + folder + file_records + cfdata)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("members", nargs="+", type=parse_member)
    args = parser.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(build_cab(args.members))
    return 0


if __name__ == "__main__":
    sys.exit(main())

