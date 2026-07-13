#!/usr/bin/env python3
"""Walk an arbitrary subtree of HKLM on the Zune HD via shellcode-injected
RegEnumKeyExW + RegEnumValueW. Produces a JSON tree:

  {
    "path": "Software\\Microsoft\\Zune",
    "values": [
      {"name": "...", "type": 1, "type_name": "REG_SZ", "size": N, "decoded": "..."}
    ],
    "subkeys": [
      { "path": "Software\\Microsoft\\Zune\\X", ...recursively... }
    ]
  }

Strategy:
- ONE shellcode per invocation, which takes the keypath at KEYPATH_VA + an
  operation arg and an index. Three ops:
     0 = enum subkey at index    -> returns name (UTF-16) into NAME_BUF
     1 = enum value at index     -> returns name + type + size + data[0:128]
- Python iterates index until ERROR_NO_MORE_ITEMS (0x103).
- Python recurses into subkeys.

Daemon load is the hard constraint. Per key visited:
  - 1 + N(subkey) calls to enum subkey (each: write idx, kcall, readback = 3 ops)
  - 1 + M(value) calls to enum value
  Plus open + close per key (combined into the shellcode itself).

For broad survey, walk depth-first with depth + count limits.
"""
from __future__ import annotations
import argparse, json, struct, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "general"))
from zune_repl import ZuneREPL

# Kernel
PROC_LIST_HEAD     = 0x80BEE010
HELPER_V3          = 0x80015220
HELPER_V4          = 0x80015280
KSCRATCH           = 0x800152D0

# Coredll (Pavo v4.5)
COREDLL_BASE       = 0x40320000
REGOPENKEYEXW      = COREDLL_BASE + 0x178E8
REGENUMKEYEXW      = COREDLL_BASE + 0x17848
REGENUMVALUEW      = COREDLL_BASE + 0x177C0
REGCLOSEKEY        = COREDLL_BASE + 0x17704

HKLM_PSEUDO        = 0x80000002
KEY_READ           = 0x00020019

# Memory layout in compositor's slot-0 page 0x000F4000-0x000F4FFF.
SC_VA              = 0x000F4000  # shellcode (~280B)
RESULTS_VA         = 0x000F4400  # results: [open_ret, op_ret, close_ret, op_id]
HKEY_VA            = 0x000F4410
TYPE_VA            = 0x000F4414  # value type out
CCHNAME_VA         = 0x000F4418  # in/out: name buffer length (CHARS)
CBDATA_VA          = 0x000F441C  # in/out: data buffer size (BYTES)
OPID_VA            = 0x000F4420  # 0=enum_subkey, 1=enum_value
INDEX_VA           = 0x000F4424
NAMEBUF_VA         = 0x000F4480  # 128 bytes (64 chars max)
KEYPATH_VA         = 0x000F4500  # up to 256 bytes utf16
DATABUF_VA         = 0x000F4600  # 256 bytes: value data (or nothing for subkey)

NAMEBUF_LEN_BYTES  = 128
NAMEBUF_LEN_CHARS  = 64
DATABUF_LEN        = 256

THROTTLE = 1.0

REG_TYPE_NAMES = {
    0: "REG_NONE", 1: "REG_SZ", 2: "REG_EXPAND_SZ", 3: "REG_BINARY",
    4: "REG_DWORD", 5: "REG_DWORD_BIG_ENDIAN", 6: "REG_LINK",
    7: "REG_MULTI_SZ", 11: "REG_QWORD",
}


def find_comp(r):
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


def utf16le_z(s: str) -> bytes:
    return s.encode("utf-16-le") + b"\x00\x00"


def build_shellcode() -> bytes:
    """ARM shellcode: open key, dispatch on OPID, single enum call, close key.

    Layout (offsets from SC_VA):
      +0x00  prologue: push, sub sp, adr r6
      +0x0C  RegOpenKeyExW(HKLM, keypath, 0, KEY_READ, &hkey)
      +0x40  ldr op_id, branch on it
      +0x60  enum_subkey path: RegEnumKeyExW(hkey, idx, name, &cchname,
                                              NULL, NULL, NULL, NULL)
      +0xB0  enum_value  path: RegEnumValueW(hkey, idx, name, &cchname,
                                              NULL, &type, data, &cbdata)
      +0x100 close + epilogue
      +0x130 lit_pool

    The shellcode is one-shot: caller has set up the keypath and index.
    Result codes -> RESULTS_VA[0..2]. The actual outputs (name buffer,
    type, size, data) are read by the caller from their respective VAs.

    For simplicity: ONE 8-stack-arg buffer for the largest call
    (RegEnumValueW takes 5 stack args after r0-r3).
    """
    # We need:
    #   stack[0] = phkResult         (open call)
    #   stack[0]..stack[4*5] for RegEnumValueW: NULL, &type, data, &cbdata, NULL?
    # RegEnumKeyExW signature:
    #   r0 hKey, r1 idx, r2 lpName, r3 lpcchName, [sp+0]=NULL,
    #   [sp+4]=NULL (lpClass), [sp+8]=NULL (lpcchClass),
    #   [sp+0xC]=NULL (lpftLastWriteTime)
    # RegEnumValueW signature:
    #   r0 hKey, r1 idx, r2 lpValueName, r3 lpcchValueName,
    #   [sp+0]=NULL (lpReserved), [sp+4]=&dwType,
    #   [sp+8]=lpData, [sp+0xC]=&cbData
    #
    # Both fit in 16 bytes of stack args. Allocate sub sp, sp, #16.

    HKEY_OFF      = HKEY_VA      - 0x000F4400  # = 0x10
    TYPE_OFF      = TYPE_VA      - 0x000F4400  # = 0x14
    CCHNAME_OFF   = CCHNAME_VA   - 0x000F4400  # = 0x18
    CBDATA_OFF    = CBDATA_VA    - 0x000F4400  # = 0x1C
    OPID_OFF      = OPID_VA      - 0x000F4400  # = 0x20
    INDEX_OFF     = INDEX_VA     - 0x000F4400  # = 0x24
    # results @ +0, hkey @ +0x10, type @ +0x14, cchname @ +0x18, cbdata @ +0x1C, opid @ +0x20, idx @ +0x24

    # Build code as ARM word list. Lit pool at end.
    # r6 = lit_pool base (set via adr).
    # r7 = RESULTS_VA base (loaded from lit_pool[3]), used for in/out variables.
    # r8 = hkey value
    code = []
    # +0x00 prologue
    code += [
        0xE92D40F0,          # push {r4-r7, lr}
        0xE92D0700,          # push {r8, r9, r10}, caller-saved we use; total 8 regs * 4 = 32 -> sp dec by 32
        0xE24DD010,          # sub sp, sp, #16
    ]
    # adr r6, lit_pool. We don't know lit_pool offset yet; placeholder. Patch later.
    ADR_PLACEHOLDER_IDX = len(code)
    code.append(0xE28F6000)  # add r6, pc, #0 (will be patched)

    # Load r7 = RESULTS_VA   (lit[3])
    code.append(0xE596700C)  # ldr r7, [r6, #0x0C]

    # === RegOpenKeyExW(HKLM, keypath, 0, KEY_READ, &hkey) ===
    code += [
        0xE5960000,          # ldr r0, [r6, #0x00]    HKLM
        0xE5961008,          # ldr r1, [r6, #0x08]    keypath_va
        0xE3A02000,          # mov r2, #0
        0xE5963004,          # ldr r3, [r6, #0x04]    KEY_READ
        # &hkey = RESULTS_VA + HKEY_OFF
        0xE2874000 | HKEY_OFF,  # add r4, r7, #HKEY_OFF
        0xE58D4000,          # str r4, [sp, #0]
        0xE5964010,          # ldr r4, [r6, #0x10]    &RegOpenKeyExW
        0xE12FFF34,          # blx r4
        0xE5870000,          # str r0, [r7, #0]       results[0] = open_ret
        0xE3500000,          # cmp r0, #0
        0x1A00002A,          # bne <cleanup_no_close> (offset placeholder, will patch)
    ]
    OPEN_BNE_IDX = len(code) - 1

    # Load hkey -> r8
    code += [
        0xE2874000 | HKEY_OFF,  # add r4, r7, #HKEY_OFF
        0xE5948000,             # ldr r8, [r4]
    ]

    # Load opid -> r0; load idx -> r9
    code += [
        0xE2874000 | OPID_OFF,
        0xE5940000,            # ldr r0, [r4]   r0 = opid
        0xE2874000 | INDEX_OFF,
        0xE5949000,            # ldr r9, [r4]   r9 = idx
        0xE3500001,            # cmp r0, #1
        0x0A000010,            # beq <enum_value_path>  (offset placeholder)
    ]
    OPID_BEQ_IDX = len(code) - 1

    # === enum_subkey path: RegEnumKeyExW(hkey, idx, name_buf, &cchname,
    #                                     NULL, NULL, NULL, NULL) ===
    # init *cchname = NAMEBUF_LEN_CHARS
    code += [
        0xE3A04000 | NAMEBUF_LEN_CHARS,   # mov r4, #64    (small enough imm)
        0xE2875000 | CCHNAME_OFF,         # add r5, r7, #CCHNAME_OFF
        0xE5854000,                       # str r4, [r5]
    ]
    # Args: r0=hkey, r1=idx, r2=&namebuf, r3=&cchname, [sp..sp+12]=NULLs
    code += [
        0xE1A00008,                       # mov r0, r8
        0xE1A01009,                       # mov r1, r9
        0xE5962014,                       # ldr r2, [r6, #0x14]   namebuf_va
        0xE1A03005,                       # mov r3, r5            &cchname (still in r5)
        0xE3A04000,                       # mov r4, #0
        0xE58D4000,                       # str r4, [sp, #0]
        0xE58D4004,                       # str r4, [sp, #4]
        0xE58D4008,                       # str r4, [sp, #8]
        0xE58D400C,                       # str r4, [sp, #0xC]
        0xE596401C,                       # ldr r4, [r6, #0x1C]   &RegEnumKeyExW
        0xE12FFF34,                       # blx r4
        0xE5870004,                       # str r0, [r7, #4]      results[1] = op_ret
        0xEA000013,                       # b   <close>           (placeholder)
    ]
    SUBKEY_B_IDX = len(code) - 1

    # === enum_value path: RegEnumValueW(hkey, idx, name_buf, &cchname,
    #                                     NULL, &type, data, &cbdata) ===
    ENUM_VALUE_PATH_START = len(code)
    code += [
        # init *cchname = NAMEBUF_LEN_CHARS, *cbdata = DATABUF_LEN
        0xE3A04000 | NAMEBUF_LEN_CHARS,
        0xE2875000 | CCHNAME_OFF,
        0xE5854000,
        0xE59F4000 | 0x100,               # ldr r4, [pc, #0x100]: placeholder; patch later
    ]
    # Actually instead of a literal, use a mov-imm with rotation. DATABUF_LEN=256=0x100=imm8(1)<<8 = ROR by 24
    # 0xE3A04000 | imm12 where imm12 = (12<<8) | 1 = 0xC01
    # encoding: 0xE3A04C01
    # Replace the previous LDR with a clean MOV:
    code[-1] = 0xE3A04C01                 # mov r4, #0x100 (256)
    code += [
        0xE2875000 | CBDATA_OFF,
        0xE5854000,                       # str r4, [r5]    *cbdata = 256
    ]
    code += [
        0xE1A00008,                       # mov r0, r8           hkey
        0xE1A01009,                       # mov r1, r9           idx
        0xE5962014,                       # ldr r2, [r6, #0x14]  namebuf_va
        0xE2875000 | CCHNAME_OFF,
        0xE1A03005,                       # mov r3, r5           &cchname
        # Stack args: [sp+0]=NULL, [sp+4]=&type, [sp+8]=data_va, [sp+C]=&cbdata
        0xE3A04000,
        0xE58D4000,                       # str r4(=0), [sp+0]   NULL
        0xE2874000 | TYPE_OFF,
        0xE58D4004,                       # str r4, [sp+4]       &type
        0xE5964018,                       # ldr r4, [r6, #0x18]  data_va
        0xE58D4008,                       # str r4, [sp+8]       data
        0xE2874000 | CBDATA_OFF,
        0xE58D400C,                       # str r4, [sp+C]       &cbdata
        0xE5964020,                       # ldr r4, [r6, #0x20]  &RegEnumValueW
        0xE12FFF34,                       # blx r4
        0xE5870004,                       # str r0, [r7, #4]
    ]

    # === close ===
    code += [
        0xE1A00008,                       # mov r0, r8
        0xE5964024,                       # ldr r4, [r6, #0x24]  &RegCloseKey
        0xE12FFF34,                       # blx r4
        0xE5870008,                       # str r0, [r7, #8]
    ]
    # cleanup_no_close target:
    code += [
        0xE28DD010,                       # add sp, sp, #16
        0xE8BD0700,                       # pop {r8, r9, r10}
        0xE8BD80F0,                       # pop {r4-r7, pc}
    ]

    # ---- patch placeholders ----
    def encode_imm12(v):
        """ARM data-processing imm12 = (rotate<<8) | imm8 where actual = imm8 ROR (2*rotate)."""
        for rot in range(16):
            ror_bits = (32 - rot*2) % 32
            rotated = ((v >> ror_bits) | (v << (32 - ror_bits) if ror_bits else 0)) & 0xFFFFFFFF
            if rotated <= 0xFF:
                return (rot << 8) | rotated
        return None

    code_bytes_count = len(code) * 4
    lit_pool_offset = code_bytes_count
    adr_inst_off = ADR_PLACEHOLDER_IDX * 4
    pc_at_adr = adr_inst_off + 8
    delta = lit_pool_offset - pc_at_adr
    assert delta >= 0 and (delta % 4 == 0), f"adr delta invalid: {delta}"
    imm12 = encode_imm12(delta)
    assert imm12 is not None, f"adr delta {delta} not encodable as ARM imm12"
    code[ADR_PLACEHOLDER_IDX] = 0xE28F6000 | imm12

    # bne cleanup_no_close (after open failure)
    cleanup_no_close_idx = len(code) - 3   # add sp, pop r8/r9/r10, pop r4-r7/pc
    bne_inst_off = OPEN_BNE_IDX * 4
    target_off = cleanup_no_close_idx * 4
    pc_at_bne = bne_inst_off + 8
    branch_offset_words = (target_off - pc_at_bne) // 4
    assert -0x800000 <= branch_offset_words < 0x800000
    imm24 = branch_offset_words & 0xFFFFFF
    code[OPEN_BNE_IDX] = 0x1A000000 | imm24    # bne

    # beq enum_value_path
    enum_value_off = ENUM_VALUE_PATH_START * 4
    beq_inst_off = OPID_BEQ_IDX * 4
    pc_at_beq = beq_inst_off + 8
    branch_offset_words = (enum_value_off - pc_at_beq) // 4
    imm24 = branch_offset_words & 0xFFFFFF
    code[OPID_BEQ_IDX] = 0x0A000000 | imm24

    # b <close> (after subkey enum)
    close_idx = ENUM_VALUE_PATH_START + 17    # the close section; count entries from ENUM_VALUE_PATH_START
    # Actually, let's compute close start by counting from where "close" comment was added.
    # Looking at the code: ENUM_VALUE_PATH_START..close_start where close_start is the
    # 'mov r0, r8' before RegCloseKey. Let me recount.
    # Entries between ENUM_VALUE_PATH_START and close (exclusive):
    #   1: mov r4, #64
    #   2: add r5, r7, #CCHNAME_OFF
    #   3: str r4, [r5]
    #   4: mov r4, #0x100
    #   5: add r5, r7, #CBDATA_OFF
    #   6: str r4, [r5]
    #   7: mov r0, r8
    #   8: mov r1, r9
    #   9: ldr r2, [r6, #0x14]
    #   10: add r5, r7, #CCHNAME_OFF
    #   11: mov r3, r5
    #   12: mov r4, #0
    #   13: str r4, [sp+0]
    #   14: add r4, r7, #TYPE_OFF
    #   15: str r4, [sp+4]
    #   16: ldr r4, [r6, #0x18]
    #   17: str r4, [sp+8]
    #   18: add r4, r7, #CBDATA_OFF
    #   19: str r4, [sp+C]
    #   20: ldr r4, [r6, #0x20]
    #   21: blx r4
    #   22: str r0, [r7, #4]
    # So close starts at ENUM_VALUE_PATH_START + 22
    close_start_idx = ENUM_VALUE_PATH_START + 22
    b_inst_off = SUBKEY_B_IDX * 4
    target_off = close_start_idx * 4
    pc_at_b = b_inst_off + 8
    branch_offset_words = (target_off - pc_at_b) // 4
    imm24 = branch_offset_words & 0xFFFFFF
    code[SUBKEY_B_IDX] = 0xEA000000 | imm24

    # Build literal pool
    # Layout (4-byte aligned):
    # +0x00  HKLM
    # +0x04  KEY_READ
    # +0x08  KEYPATH_VA
    # +0x0C  RESULTS_VA
    # +0x10  &RegOpenKeyExW
    # +0x14  NAMEBUF_VA
    # +0x18  DATABUF_VA
    # +0x1C  &RegEnumKeyExW
    # +0x20  &RegEnumValueW
    # +0x24  &RegCloseKey
    lit_pool = [
        HKLM_PSEUDO,
        KEY_READ,
        KEYPATH_VA,
        RESULTS_VA,
        REGOPENKEYEXW,
        NAMEBUF_VA,
        DATABUF_VA,
        REGENUMKEYEXW,
        REGENUMVALUEW,
        REGCLOSEKEY,
    ]

    sc = b"".join(struct.pack("<I", w) for w in code + lit_pool)
    return sc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.0.100")
    ap.add_argument("--root", default="Software\\Microsoft\\Zune",
                    help="HKLM-relative starting key")
    ap.add_argument("--depth", type=int, default=3, help="recursion depth limit")
    ap.add_argument("--max-children", type=int, default=32,
                    help="max subkeys + values to enumerate per key")
    ap.add_argument("--out", default=None,
                    help="JSON output path; default dumps/registry-walk-<root>-<timestamp>.json")
    ap.add_argument("--throttle", type=float, default=THROTTLE)
    args = ap.parse_args()

    r = ZuneREPL(args.ip, timeout=60.0)
    comp_va, comp_pid = find_comp(r)
    print(f"compositor: 0x{comp_va:08X} pid=0x{comp_pid:08X}")

    sc = build_shellcode()
    print(f"shellcode size: {len(sc)} bytes")
    print(f"writing shellcode to 0x{SC_VA:08X}")
    r.write_proc_mem(comp_pid, SC_VA, sc)
    time.sleep(args.throttle)
    rb = r.read_proc_mem(comp_pid, SC_VA, len(sc))
    if rb != sc:
        print("shellcode readback mismatch; abort")
        return 1
    print("shellcode verified")

    visited = set()
    def walk(path, depth):
        if depth < 0: return None
        if path in visited: return {"path": path, "cycle": True}
        visited.add(path)

        node = {"path": path, "subkeys": [], "values": []}

        # Write keypath
        kp_bytes = utf16le_z(path)
        kp_pad = 256 - len(kp_bytes)
        if kp_pad < 0:
            node["error"] = f"keypath too long ({len(kp_bytes)}B)"
            return node
        r.write_proc_mem(comp_pid, KEYPATH_VA, kp_bytes + b"\x00"*kp_pad)
        time.sleep(args.throttle)

        def do_op(opid: int, idx: int):
            """One enum step: write setup + kcall + read 0x300 block.
            Returns (open_r, op_r, close_r, vtype, cchname, cbdata, namebuf_bytes, databuf_bytes)."""
            setup = (b"\x00" * 32) + struct.pack("<I", opid) + struct.pack("<I", idx)
            assert len(setup) == 40
            r.write_proc_mem(comp_pid, RESULTS_VA, setup)
            time.sleep(args.throttle)
            r.kcall(HELPER_V4, comp_va, SC_VA)
            time.sleep(args.throttle)
            # 0x300 covers RESULTS(40) + NAMEBUF(128) + KEYPATH(256) + DATABUF(256), single call.
            blk = r.read_proc_mem(comp_pid, RESULTS_VA, 0x300)
            time.sleep(args.throttle)
            open_r, op_r, close_r = struct.unpack_from("<III", blk, 0)
            vtype  = struct.unpack_from("<I", blk, TYPE_VA - RESULTS_VA)[0]
            cchname = struct.unpack_from("<I", blk, CCHNAME_VA - RESULTS_VA)[0]
            cbdata  = struct.unpack_from("<I", blk, CBDATA_VA - RESULTS_VA)[0]
            # Extract namebuf bytes from the block (offset NAMEBUF_VA - RESULTS_VA = 0x80)
            nm_off = NAMEBUF_VA - RESULTS_VA
            namebuf = blk[nm_off : nm_off + NAMEBUF_LEN_BYTES]
            db_off = DATABUF_VA - RESULTS_VA
            databuf = blk[db_off : db_off + DATABUF_LEN]
            return open_r, op_r, close_r, vtype, cchname, cbdata, namebuf, databuf

        # Enumerate subkeys
        recursed_overwrote_keypath = False
        for idx in range(args.max_children):
            if recursed_overwrote_keypath:
                r.write_proc_mem(comp_pid, KEYPATH_VA, kp_bytes + b"\x00"*kp_pad)
                time.sleep(args.throttle)
                recursed_overwrote_keypath = False
            open_r, op_r, close_r, _, cchname, _, namebuf, _ = do_op(0, idx)
            if open_r != 0:
                node["open_error"] = f"0x{open_r:X}"
                return node
            if op_r == 0x103 or op_r == 0x6:
                break
            if op_r != 0:
                node["enum_subkey_error"] = f"0x{op_r:X} at idx={idx}"
                break
            name = namebuf[: cchname*2 + 2].decode("utf-16-le", "replace").split("\x00", 1)[0]
            child_path = f"{path}\\{name}" if path else name
            print(f"  {'  '*(args.depth-depth)}[K] {child_path}")
            if depth > 0:
                child_node = walk(child_path, depth - 1)
                if child_node:
                    node["subkeys"].append(child_node)
                recursed_overwrote_keypath = True
            else:
                node["subkeys"].append({"path": child_path, "_truncated_at_depth": True})

        # Enumerate values
        for idx in range(args.max_children):
            open_r, op_r, close_r, vtype, cchname, cbdata, namebuf, databuf = do_op(1, idx)
            if open_r != 0: break
            if op_r == 0x103 or op_r == 0x6: break
            if op_r != 0:
                node["enum_value_error"] = f"0x{op_r:X} at idx={idx}"
                break
            name = namebuf[: cchname*2 + 2].decode("utf-16-le", "replace").split("\x00", 1)[0]
            data_bytes = databuf[: min(cbdata, DATABUF_LEN)]

            decoded = None
            if vtype in (1, 2):
                try: decoded = data_bytes.decode("utf-16-le").split("\x00", 1)[0]
                except: pass
            elif vtype == 4 and len(data_bytes) >= 4:
                decoded = struct.unpack_from("<I", data_bytes, 0)[0]
            elif vtype == 7:
                try:
                    decoded = [s for s in data_bytes.decode("utf-16-le").split("\x00") if s]
                except: pass

            print(f"  {'  '*(args.depth-depth)}[V] {name}  {REG_TYPE_NAMES.get(vtype,vtype)}  {decoded!r}")
            node["values"].append({
                "name": name, "type": vtype,
                "type_name": REG_TYPE_NAMES.get(vtype, f"0x{vtype:X}"),
                "size": cbdata,
                "raw_hex": data_bytes.hex(),
                "decoded": decoded,
            })

        return node

    print(f"\nwalking HKLM\\{args.root}  depth={args.depth}  max_children={args.max_children}")
    tree = walk(args.root, args.depth)

    # Save
    if not args.out:
        import datetime
        slug = args.root.replace("\\","_").replace("/","_")
        stamp = datetime.datetime.now().strftime("%Y%m%dT%H%M%SZ")
        args.out = f"dumps/registry-walk-{slug}-{stamp}.json"
    Path(args.out).parent.mkdir(exist_ok=True)
    Path(args.out).write_text(json.dumps(tree, indent=2))
    print(f"\nsaved: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
