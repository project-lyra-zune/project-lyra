#!/usr/bin/env python3
"""Walk a Windows CE flat ROM image's ROMHDR and extract every module + data file.

Input is a flat image produced by ce-b000ff-decompress.py. The ROMHDR is located
by scanning for the pTOC pointer at flat offset 0x40 (CE convention) and
converting from kernel-virtual to file offset via the partition's `start`.

For each TOC entry (module, DLL/EXE), the loaded image is reconstructed in
memory by walking its O32 section descriptors and copying section data from the
flat image into a buffer sized by max(o32_rva + o32_vsize). The result is the
post-ROM-loader memory image (what `coredll.dll` looks like loaded into a
process), sufficient for disassembly and hash comparison against
device dumps. It is **not** a strict on-disk PE file (rebuilding the original
.text/.data section alignment requires the eimgfs reconstruction pipeline).

For each FILES entry (data file), the bytes are extracted verbatim from
ulLoadOffset for nRealFileSize bytes. Compressed data files (where
nCompFileSize < nRealFileSize) are LZX-compressed and emit `.lzx` placeholders
that need decompression by an external tool, out of scope for this extractor.

Outputs under <out_dir>/:
  manifest.csv  partition,kind,name,size,load_addr,sha256,error
  modules/      one file per TOC entry (named after lpszFileName)
  files/        one file per FILES entry
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import struct
import sys
from pathlib import Path


ROMHDR_SIZE = 0x54
TOC_ENTRY_SIZE = 32

# E32 struct (CE 6 ARM, 32-bit). Layout per Microsoft pkfuncs.h / romldr docs:
#   0x00 u16 e32_objcnt
#   0x02 u16 e32_imageflags
#   0x04 u32 e32_entryrva
#   0x08 u32 e32_vbase
#   0x0c u16 e32_subsysmajor
#   0x0e u16 e32_subsysminor
#   0x10 u32 e32_stackmax
#   0x14 u32 e32_vsize
#   0x18 u32 e32_sect14rva
#   0x1c u32 e32_sect14size
#   0x20 u32 e32_timestamp
#   0x24 [9] u32 e32_unit (info_t array, one per data directory)
#   0x48 u16 e32_subsys
# Total: 0x4c (76 bytes)
E32_SIZE = 0x4c

# O32 entry (CE 6, 24 bytes):
#   0x00 u32 o32_vsize
#   0x04 u32 o32_rva
#   0x08 u32 o32_psize
#   0x0c u32 o32_realaddr
#   0x10 u32 o32_access (or reserved)
#   0x14 u32 o32_flags
# Total: 0x18 (24 bytes)
O32_SIZE = 0x18


def read_cstring(data: bytes, offset: int, max_len: int = 256) -> str:
    if offset < 0 or offset >= len(data):
        return ""
    end = data.find(b"\x00", offset, min(offset + max_len, len(data)))
    if end == -1:
        end = min(offset + max_len, len(data))
    return data[offset:end].decode("ascii", errors="replace")


def find_romhdr(data: bytes, start: int) -> tuple[int, int]:
    """Return (romhdr_file_offset, virt_base) by reading the pTOC pointer at 0x40."""
    if data[0x40:0x44] != b"ECEC":
        raise RuntimeError("flat[0x40..0x44] is not 'ECEC'; not a CE imgfs ROM")
    ptoc = struct.unpack("<I", data[0x44:0x48])[0]
    virt_base = 0x80000000 | start
    rh_off = ptoc - virt_base
    if rh_off < 0 or rh_off + ROMHDR_SIZE > len(data):
        raise RuntimeError(
            f"pTOC {ptoc:#x} maps to file offset {rh_off:#x} which is out of range"
        )
    return rh_off, virt_base


def parse_romhdr(data: bytes, rh_off: int) -> dict:
    f = struct.unpack("<13I", data[rh_off:rh_off + 52])
    cpu = struct.unpack("<H", data[rh_off + 0x44:rh_off + 0x46])[0]
    misc = struct.unpack("<H", data[rh_off + 0x46:rh_off + 0x48])[0]
    return {
        "dllfirst": f[0],
        "dlllast": f[1],
        "physfirst": f[2],
        "physlast": f[3],
        "nummods": f[4],
        "ulRAMStart": f[5],
        "ulRAMFree": f[6],
        "ulRAMEnd": f[7],
        "ulCopyEntries": f[8],
        "ulCopyOffset": f[9],
        "ulProfileLen": f[10],
        "ulProfileOffset": f[11],
        "numfiles": f[12],
        "usCPUType": cpu,
        "usMiscFlags": misc,
    }


def virt_to_file(virt: int, virt_base: int, file_size: int) -> int | None:
    rel = virt - virt_base
    if 0 <= rel < file_size:
        return rel
    return None


def reconstruct_module(
    flat: bytes,
    e32_off: int,
    o32_off: int,
    load_addr: int,
    virt_base: int,
) -> tuple[bytes, list[dict]]:
    """Reconstruct the loaded-memory image of a module by walking O32 sections.

    Returns (image_bytes, sections_summary). Bytes start at the module's load
    address (offset 0 in the returned buffer corresponds to ulLoadOffset).
    """
    if e32_off + E32_SIZE > len(flat):
        raise RuntimeError(f"E32 truncated at {e32_off:#x}")
    e32_objcnt = struct.unpack("<H", flat[e32_off:e32_off + 2])[0]
    e32_vsize = struct.unpack("<I", flat[e32_off + 0x14:e32_off + 0x18])[0]

    if o32_off + e32_objcnt * O32_SIZE > len(flat):
        raise RuntimeError(
            f"O32 array truncated at {o32_off:#x} (count {e32_objcnt})"
        )

    image = bytearray(e32_vsize)
    sections: list[dict] = []

    for i in range(e32_objcnt):
        entry = flat[o32_off + i * O32_SIZE:o32_off + (i + 1) * O32_SIZE]
        vsize, rva, psize, realaddr, access, flags = struct.unpack("<6I", entry)
        sections.append({
            "vsize": vsize, "rva": rva, "psize": psize,
            "realaddr": realaddr, "flags": flags,
        })
        if psize == 0:
            continue
        # realaddr is a kernel-virtual pointer to the on-flash bytes for this
        # section. Convert to file offset and copy psize bytes into image[rva].
        src_file = virt_to_file(realaddr, virt_base, len(flat))
        if src_file is None:
            raise RuntimeError(
                f"section {i}: realaddr {realaddr:#x} not in flat range"
            )
        if src_file + psize > len(flat):
            raise RuntimeError(
                f"section {i}: source data extends past flat end"
            )
        if rva + psize > len(image):
            # Some CE images have psize > vsize for sections w/ uninitialized tail
            # but let's just clip rather than blow up.
            psize = max(0, len(image) - rva)
        image[rva:rva + psize] = flat[src_file:src_file + psize]

    return bytes(image), sections


def safe_filename(name: str) -> str:
    # Filenames are ASCII; just strip path-traversal and whitespace.
    cleaned = name.strip().replace("/", "_").replace("\\", "_")
    return cleaned or "(unnamed)"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("flat", type=Path, help="flat ROM image (from ce-b000ff-decompress.py)")
    parser.add_argument("--start", type=lambda x: int(x, 0), required=True,
                        help="partition's physical start address (e.g. 0x14000 for NK.bin)")
    parser.add_argument("--out", type=Path, required=True, help="output directory")
    parser.add_argument("--label", default="", help="partition label for the manifest")
    args = parser.parse_args()

    flat = args.flat.read_bytes()
    rh_off, virt_base = find_romhdr(flat, args.start)
    rh = parse_romhdr(flat, rh_off)

    out_dir = args.out
    out_dir.mkdir(parents=True, exist_ok=True)
    modules_dir = out_dir / "modules"
    files_dir = out_dir / "files"
    modules_dir.mkdir(exist_ok=True)
    files_dir.mkdir(exist_ok=True)

    label = args.label or args.flat.stem
    manifest_path = out_dir / "manifest.csv"
    manifest = manifest_path.open("w", newline="")
    writer = csv.DictWriter(
        manifest,
        fieldnames=["partition", "kind", "name", "size", "load_addr", "sha256", "error"],
    )
    writer.writeheader()

    # TOC entries (modules) immediately follow ROMHDR.
    toc_off = rh_off + ROMHDR_SIZE
    nummods = rh["nummods"]
    numfiles = rh["numfiles"]

    print(f"label={label}")
    print(f"romhdr_offset={rh_off:#x}")
    print(f"virt_base={virt_base:#x}")
    print(f"nummods={nummods} numfiles={numfiles}")
    print(f"output_dir={out_dir}")

    mod_ok = mod_err = 0
    for i in range(nummods):
        rec = flat[toc_off + i * TOC_ENTRY_SIZE:toc_off + (i + 1) * TOC_ENTRY_SIZE]
        attr, ftlo, fthi, sz, name_ptr, e32_ptr, o32_ptr, load = struct.unpack("<8I", rec)
        name_off = virt_to_file(name_ptr, virt_base, len(flat))
        name = read_cstring(flat, name_off) if name_off is not None else ""
        e32_off = virt_to_file(e32_ptr, virt_base, len(flat))
        o32_off = virt_to_file(o32_ptr, virt_base, len(flat))
        try:
            if e32_off is None or o32_off is None:
                raise RuntimeError(
                    f"E32/O32 pointers out of range "
                    f"(e32={e32_ptr:#x} o32={o32_ptr:#x})"
                )
            image, sections = reconstruct_module(flat, e32_off, o32_off, load, virt_base)
            sha = hashlib.sha256(image).hexdigest()
            out_path = modules_dir / safe_filename(name)
            out_path.write_bytes(image)
            writer.writerow({
                "partition": label, "kind": "module", "name": name,
                "size": len(image), "load_addr": f"{load:#010x}",
                "sha256": sha, "error": "",
            })
            mod_ok += 1
        except Exception as exc:
            writer.writerow({
                "partition": label, "kind": "module", "name": name,
                "size": "", "load_addr": f"{load:#010x}",
                "sha256": "", "error": str(exc),
            })
            mod_err += 1

    # FILES entries follow TOC. CE 6 FILES entry is 28 bytes:
    #   0x00 u32 dwFileAttributes
    #   0x04 FILETIME ftTime (8 bytes)
    #   0x0c u32 nRealFileSize
    #   0x10 u32 nCompFileSize
    #   0x14 u32 lpszFileName
    #   0x18 u32 ulLoadOffset
    FILES_ENTRY_SIZE = 28
    files_off = toc_off + nummods * TOC_ENTRY_SIZE
    file_ok = file_err = 0
    for i in range(numfiles):
        rec = flat[files_off + i * FILES_ENTRY_SIZE:files_off + (i + 1) * FILES_ENTRY_SIZE]
        if len(rec) < FILES_ENTRY_SIZE:
            break
        attr, ftlo, fthi, real_sz, comp_sz, name_ptr, load = struct.unpack("<7I", rec)
        name_off = virt_to_file(name_ptr, virt_base, len(flat))
        name = read_cstring(flat, name_off) if name_off is not None else ""
        try:
            data_off = virt_to_file(load, virt_base, len(flat))
            if data_off is None:
                raise RuntimeError(f"load addr {load:#x} out of file range")
            # CE FILES uses LZX compression when comp_sz < real_sz.
            # We don't decompress here; emit raw bytes either way with a marker.
            extracted_size = comp_sz if comp_sz else real_sz
            if data_off + extracted_size > len(flat):
                raise RuntimeError("file data extends past flat end")
            blob = flat[data_off:data_off + extracted_size]
            sha = hashlib.sha256(blob).hexdigest()
            suffix = ".lzx" if (0 < comp_sz < real_sz) else ""
            out_name = safe_filename(name) + suffix
            out_path = files_dir / out_name
            out_path.write_bytes(blob)
            error = "" if not suffix else "lzx-compressed"
            writer.writerow({
                "partition": label, "kind": "file", "name": name,
                "size": real_sz, "load_addr": f"{load:#010x}",
                "sha256": sha, "error": error,
            })
            file_ok += 1
        except Exception as exc:
            writer.writerow({
                "partition": label, "kind": "file", "name": name,
                "size": "", "load_addr": f"{load:#010x}",
                "sha256": "", "error": str(exc),
            })
            file_err += 1

    manifest.close()

    print(f"modules: {mod_ok} ok, {mod_err} errors")
    print(f"files:   {file_ok} ok, {file_err} errors")
    print(f"manifest: {manifest_path}")
    return 0 if mod_err == 0 and file_err == 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
