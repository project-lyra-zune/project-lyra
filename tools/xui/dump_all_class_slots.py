#!/usr/bin/env python3
"""Dump every class registered via xuidll's static slots.

Avoids the registry hash-table walk entirely; that approach had timeouts
that truncated chains and missed classes. Instead, we bulk-read the .data
slot regions (one TCP call per region) and dereference each non-null slot.

Writes all_class_slots.log (terminal mirror) and all_class_slots.json into
--out-dir (default: current directory).
"""
import argparse
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL


# Known slot regions in xuidll's .data section. Each is a contiguous run
# of 4-byte class_info pointers populated by Register* functions at boot.
SLOT_RANGES = [
    (0x41871d30, 0x10,  "Html family (Control/Presenter/Element)"),
    (0x418726f0, 0x140, "Main xuidll registry"),
    (0x41874560, 0x14,  "Slider/ProgressBar"),
    (0x41874cc0, 0x10,  "SoundXAudio"),
]
THROTTLE = 0.6


class TeeLog:
    def __init__(self, p):
        self.f = open(p, "w", buffering=1)

    def w(self, msg):
        print(msg)
        self.f.write(msg + "\n")
        self.f.flush()

    def close(self):
        self.f.close()


def read_with_retry(r, pid, va, length, log, retries=3):
    for attempt in range(retries):
        try:
            buf = r.read_proc_mem(pid, va, length)
            time.sleep(THROTTLE)
            return buf
        except Exception as e:
            log.w(f"  RETRY {attempt+1}/{retries} for {va:#x}: {e}")
            time.sleep(2.0 * (attempt + 1))
    log.w(f"  GIVE UP {va:#x}")
    return None


def read_utf16(r, pid, va, log):
    if not va:
        return ""
    if not (
        (0x00010000 <= va <= 0x00200000)
        or (0x41800000 <= va <= 0x41a00000)
    ):
        return f"<oor:{va:#x}>"
    buf = read_with_retry(r, pid, va, 64, log)
    if buf is None:
        return "<read-failed>"
    return buf.decode("utf-16-le", errors="replace").split("\x00", 1)[0]


def dump_class(r, pid, slot_va, ci_ptr, log):
    if not ci_ptr:
        return None
    ci = read_with_retry(r, pid, ci_ptr, 0x3C, log)
    if ci is None:
        return None
    name_p = struct.unpack_from("<I", ci, 0)[0]
    parent_p = struct.unpack_from("<I", ci, 4)[0]
    arr = struct.unpack_from("<I", ci, 0x1C)[0]
    cnt = struct.unpack_from("<I", ci, 0x20)[0]
    flags = struct.unpack_from("<I", ci, 0x38)[0]
    name = read_utf16(r, pid, name_p, log)
    parent = read_utf16(r, pid, parent_p, log)

    log.w(f"\n=== {name!r} (slot {slot_va:#x} → ci {ci_ptr:#x}) ===")
    log.w(f"  parent={parent!r}, count={cnt}, array={arr:#x}, flags={flags:#x}")

    propdefs = []
    if cnt and cnt < 100 and arr:
        arr_buf = read_with_retry(r, pid, arr, cnt * 0x34, log)
        if arr_buf is None:
            log.w("  ARRAY READ FAILED")
            return {"name": name, "parent": parent, "count": cnt, "propdefs": None}
        type_names = {
            1: "bool", 2: "integer", 3: "unsigned", 4: "float",
            5: "string", 6: "color", 7: "vector", 8: "quaternion",
            9: "object", 10: "custom",
        }
        for i in range(cnt):
            e = arr_buf[i * 0x34:(i + 1) * 0x34]
            in_off = struct.unpack_from("<I", e, 0x08)[0]
            name_p = struct.unpack_from("<I", e, 0x10)[0]
            type_c = struct.unpack_from("<I", e, 0x14)[0]
            pname = read_utf16(r, pid, name_p, log)
            tname = type_names.get(type_c, f"?{type_c}")
            log.w(f"  {i:>2} {in_off:>#6x} {pname:<28} {tname}")
            propdefs.append({"i": i, "in_off": in_off, "name": pname, "type": tname, "type_code": type_c})
    return {
        "slot_va": slot_va, "ci_ptr": ci_ptr,
        "name": name, "parent": parent,
        "count": cnt, "array": arr, "flags": flags,
        "propdefs": propdefs,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.0.100")
    ap.add_argument("--out-dir", default=".",
                    help="Directory for all_class_slots.{log,json} (default: cwd)")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    log = TeeLog(out_dir / "all_class_slots.log")
    log.w(f"Bulk slot dump, throttle={THROTTLE}s")

    r = ZuneREPL(args.ip, timeout=20.0)
    pid = next(p for p, n in r.list_processes() if n.lower() == "gemstone.exe")
    log.w(f"gemstone PID = {pid:#010x}")
    time.sleep(THROTTLE)

    # Phase 1: bulk-read each slot range, collect non-null ptrs.
    all_slots = []  # (slot_va, ci_ptr)
    for base, length, desc in SLOT_RANGES:
        log.w(f"\nRange {base:#x}..{base + length:#x} ({desc})")
        buf = read_with_retry(r, pid, base, length, log)
        if buf is None:
            continue
        n_ptrs = length // 4
        for i in range(n_ptrs):
            ptr = struct.unpack_from("<I", buf, i * 4)[0]
            slot_va = base + i * 4
            if ptr:
                all_slots.append((slot_va, ptr))
                log.w(f"  slot {slot_va:#x} → {ptr:#x}")
            else:
                log.w(f"  slot {slot_va:#x} → 0 (empty)")

    log.w(f"\n{len(all_slots)} non-null slots to dump")

    # Phase 2: dump each class.
    results = []
    for slot_va, ci_ptr in all_slots:
        rec = dump_class(r, pid, slot_va, ci_ptr, log)
        if rec:
            results.append(rec)

    # Save structured JSON for downstream tooling.
    out = out_dir / "all_class_slots.json"
    out.write_text(json.dumps(results, indent=2))
    log.w(f"\nDumped {len(results)} classes to {out}")
    log.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
