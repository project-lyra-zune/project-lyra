#!/usr/bin/env python3
"""Read an arbitrary registry value from the live Zune HD in-memory hive.

General round-trip companion to the modkit `registry_write` capability:
write a value via a mod, then read it back here to verify. Mechanism mirrors
read-usb-registry.py / set-zune-endpoints.py: ARM shellcode (RegOpenKeyExW +
RegQueryValueExW + RegCloseKey) injected into compositor's slot-0 scratch and
invoked via kcall HELPER_V4. Read-only and safe (the Zune registry has no
persistence; RegFlushKey is a stub, see project_zune_registry_persistence.md).

Usage:
    read-registry.py HKLM "Software\\Xune\\ModkitTest" probe_dword probe_sz
    read-registry.py --ip 192.168.0.100 HKLM "Comm\\AR6K_SD1\\Parms" discTimeout
    read-registry.py HKLM "Software\\Xune\\ModkitTest" probe_dword --expect-dword 0xbeef
    read-registry.py HKLM "Comm\\AR6K_SD1\\Parms" scanAfterLinkLost --json

Exit code is non-zero on a --expect-* mismatch, so it composes in test scripts.
"""
from __future__ import annotations
import argparse, json, struct, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL

# Kernel + coredll (Pavo v4.5)
PROC_LIST_HEAD   = 0x80BEE010
HELPER_V4        = 0x80015280
COREDLL_BASE     = 0x40320000
REGOPENKEYEXW    = COREDLL_BASE + 0x178E8
REGQUERYVALUEEXW = COREDLL_BASE + 0x179BC
REGCLOSEKEY      = COREDLL_BASE + 0x17704

HIVES = {
    "HKCR": 0x80000000, "HKEY_CLASSES_ROOT": 0x80000000,
    "HKCU": 0x80000001, "HKEY_CURRENT_USER": 0x80000001,
    "HKLM": 0x80000002, "HKEY_LOCAL_MACHINE": 0x80000002,
    "HKU":  0x80000003, "HKEY_USERS": 0x80000003,
}
KEY_READ = 0x00020019

# Slot-0 scratch layout (identical to read-usb-registry.py)
READ_SC_VA   = 0x000F4000
RESULTS_VA   = 0x000F4400
HKEY_VA      = 0x000F440C
TYPE_VA      = 0x000F4410
CBSIZE_VA    = 0x000F4414
VALUENAME_VA = 0x000F4420
KEYPATH_VA   = 0x000F4500
BUF_VA       = 0x000F4600
BUF_LEN      = 256

REG_TYPE_NAMES = {
    0: "REG_NONE", 1: "REG_SZ", 2: "REG_EXPAND_SZ", 3: "REG_BINARY",
    4: "REG_DWORD", 5: "REG_DWORD_BIG_ENDIAN", 7: "REG_MULTI_SZ",
}


def utf16le_z(s: str) -> bytes:
    return s.encode("utf-16-le") + b"\x00\x00"


def find_comp(r: ZuneREPL) -> tuple[int, int]:
    head = struct.unpack("<I", r.kread(PROC_LIST_HEAD, 4))[0]
    cur = head
    while cur:
        blk = r.kread(cur, 0x40)
        nxt = struct.unpack_from("<I", blk, 0)[0]
        pid = struct.unpack_from("<I", blk, 0x0C)[0]
        np  = struct.unpack_from("<I", blk, 0x20)[0]
        if np:
            n = r.kread(np, 64).split(b"\x00\x00", 1)[0].decode("utf-16-le", "replace")
            if "compositor" in n.lower():
                return cur, pid
        cur = nxt
    raise RuntimeError("compositor not found")


def build_read_shellcode(hive_pseudo: int) -> bytes:
    """ARM shellcode: RegOpenKeyExW(hive,key) -> RegQueryValueExW(name) ->
    RegCloseKey. Identical to set-zune-endpoints.py's read path; only the
    hive pseudo-handle in the literal pool is parameterized."""
    code = [
        0xE92D40F0, 0xE24DD010, 0xE28F6078,
        0xE5960000, 0xE5961010, 0xE3A02000, 0xE5963004,
        0xE5964008, 0xE58D4000, 0xE5964024, 0xE12FFF34,
        0xE596700C, 0xE5870000, 0xE3500000, 0x1A000010,
        0xE5964008, 0xE5940000, 0xE5961014, 0xE3A02000,
        0xE5963018, 0xE5964020, 0xE58D4000, 0xE596401C,
        0xE58D4004, 0xE5964028, 0xE12FFF34, 0xE5870004,
        0xE5964008, 0xE5940000, 0xE596402C, 0xE12FFF34,
        0xE5870008,
        0xE28DD010, 0xE8BD80F0,
    ]
    lit_pool = [
        hive_pseudo, KEY_READ, HKEY_VA, RESULTS_VA,
        KEYPATH_VA, VALUENAME_VA, TYPE_VA, CBSIZE_VA, BUF_VA,
        REGOPENKEYEXW, REGQUERYVALUEEXW, REGCLOSEKEY,
    ]
    return b"".join(struct.pack("<I", w) for w in code + lit_pool)


def ensure_layout(r, comp_pid, hive_pseudo, keypath_str, throttle):
    read_sc = build_read_shellcode(hive_pseudo)
    keypath = utf16le_z(keypath_str)
    if r.read_proc_mem(comp_pid, READ_SC_VA, len(read_sc)) != read_sc:
        r.write_proc_mem(comp_pid, READ_SC_VA, read_sc)
        time.sleep(throttle)
        if r.read_proc_mem(comp_pid, READ_SC_VA, len(read_sc)) != read_sc:
            raise RuntimeError("read shellcode readback mismatch")
        time.sleep(throttle)
    pad_len = max(256, len(keypath))
    if pad_len > BUF_VA - KEYPATH_VA:
        raise RuntimeError(f"key path too long ({pad_len} bytes)")
    r.write_proc_mem(comp_pid, KEYPATH_VA,
                     keypath + b"\x00" * (pad_len - len(keypath)))
    time.sleep(throttle)


def query(r, comp_va, comp_pid, name: str, throttle: float) -> dict:
    valuename = utf16le_z(name)
    pad = 64 - len(valuename)
    if pad < 0:
        raise RuntimeError(f"value name too long: {name!r}")
    setup = (
        b"\x00" * 12 + b"\x00" * 4 + b"\x00" * 4
        + struct.pack("<I", BUF_LEN)
        + b"\x00" * 8
        + valuename + b"\x00" * pad
    )
    r.write_proc_mem(comp_pid, RESULTS_VA, setup)
    time.sleep(throttle)
    r.kcall(HELPER_V4, comp_va, READ_SC_VA)
    time.sleep(throttle)
    block = r.read_proc_mem(comp_pid, RESULTS_VA, 0x300)
    time.sleep(throttle)
    open_r, query_r, close_r = struct.unpack_from("<III", block, 0)
    vtype  = struct.unpack_from("<I", block, TYPE_VA - RESULTS_VA)[0]
    cbsize = struct.unpack_from("<I", block, CBSIZE_VA - RESULTS_VA)[0]
    buf = block[BUF_VA - RESULTS_VA : BUF_VA - RESULTS_VA + min(cbsize, BUF_LEN)]

    decoded = None
    if query_r == 0 and buf:
        if vtype == 4 and cbsize >= 4:
            decoded = struct.unpack_from("<I", buf, 0)[0]
        elif vtype in (1, 2):
            try: decoded = buf.decode("utf-16-le").rstrip("\x00")
            except Exception: decoded = buf.hex()
        elif vtype == 7:
            try: decoded = [s for s in buf.decode("utf-16-le").split("\x00") if s]
            except Exception: decoded = buf.hex()
        elif vtype == 3:
            decoded = buf.hex()

    return {
        "name": name, "open_ret": open_r, "query_ret": query_r, "close_ret": close_r,
        "type": vtype, "type_name": REG_TYPE_NAMES.get(vtype, f"0x{vtype:X}"),
        "cbsize": cbsize, "raw_hex": buf.hex(), "decoded": decoded,
    }


def read_values(ip: str, hive: str, key: str, names, throttle: float = 0.4):
    """Read one or more values under hive\\key. Returns (comp_pid, [records])."""
    hive_u = hive.upper()
    if hive_u not in HIVES:
        raise SystemExit(f"unknown hive {hive!r}; use one of {sorted(HIVES)}")
    r = ZuneREPL(ip)
    comp_va, comp_pid = find_comp(r)
    ensure_layout(r, comp_pid, HIVES[hive_u], key, throttle)
    return comp_pid, [query(r, comp_va, comp_pid, n, throttle) for n in names]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("hive", help="HKLM | HKCU | HKCR | HKU (or HKEY_* long form)")
    ap.add_argument("key", help=r"subkey path, e.g. Software\Xune\ModkitTest")
    ap.add_argument("values", nargs="+", help="one or more value names")
    ap.add_argument("--ip", default="192.168.0.100")
    ap.add_argument("--throttle", type=float, default=0.4)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--expect-dword", type=lambda s: int(s, 0),
                    help="assert the single value equals this DWORD (exit 1 on mismatch)")
    ap.add_argument("--expect-sz", help="assert the single value equals this string")
    args = ap.parse_args()

    comp_pid, recs = read_values(args.ip, args.hive, args.key, args.values, args.throttle)

    if args.json:
        print(json.dumps({"ip": args.ip, "hive": args.hive.upper(),
                          "key": args.key, "values": recs}, indent=2))
    else:
        print(f"{args.hive.upper()}\\{args.key}  (compositor pid=0x{comp_pid:08x})")
        for rec in recs:
            if rec["query_ret"] == 0:
                d = rec["decoded"]
                v = f"{d}  (0x{d:x})" if isinstance(d, int) else repr(d)
                print(f"  {rec['name']:24s} {rec['type_name']:14s} {v}")
            else:
                print(f"  {rec['name']:24s} NOT FOUND "
                      f"(open=0x{rec['open_ret']:X} query=0x{rec['query_ret']:X})")

    if args.expect_dword is not None or args.expect_sz is not None:
        if len(recs) != 1:
            print("  --expect-* requires exactly one value", file=sys.stderr)
            return 2
        got = recs[0]["decoded"]
        want = args.expect_dword if args.expect_dword is not None else args.expect_sz
        if got == want:
            print(f"  ROUND-TRIP OK: {recs[0]['name']} == {want!r}")
            return 0
        print(f"  ROUND-TRIP MISMATCH: {recs[0]['name']} = {got!r}, expected {want!r}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
