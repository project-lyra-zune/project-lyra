"""High-level wrapper around nativeapp's REPL dispatcher (opcode 40).

Each method opens one TCP connection, sends one opcode-40 invoke, returns the
parsed result. Handlers live in nativeapp's repl_primitives.cpp (linked into
the daemon directly, no plugin DLL deploy needed; no per-call LoadLibrary).
Latency dominated by the TCP round-trip.

Sub-opcode wire format (matches repl_primitives.cpp):
  arg layout: u32 sub_opcode | sub_args
  out layout: u32 status | response bytes

usage:
  from zune_repl import ZuneREPL
  r = ZuneREPL("192.168.0.100")
  procs = r.list_processes()
  for pid, name in procs:
      print(f"{pid:#010x}  {name}")
"""
from __future__ import annotations

import socket
import struct
from dataclasses import dataclass

REPL_OPCODE = 40            # nativeapp connection() REPL dispatch
DEFAULT_PORT = 1337
OUT_CAP = 8192

# sub-opcodes
READ_PROC_MEM   = 1
WRITE_PROC_MEM  = 2
VIRT_QUERY_EX   = 3
LIST_PROCESSES  = 4
LIST_MODULES    = 5
READ_MEM_LOCAL  = 6
KREAD_BYTES     = 7
KWRITE_BYTES    = 8
MAP_PTR_TO_PROC = 9
READ_PROC_KMEM  = 10
KCALL           = 11
USER_CALL       = 13
DUMP_VA_TO_FILE = 14
PULL_FILE       = 15

STATUS_OK = 0


class REPLError(RuntimeError):
    def __init__(self, status: int, msg: str = ""):
        self.status = status
        self.message = msg or f"REPL status=0x{status:08x}"
        super().__init__(self.message)


@dataclass
class MBI:
    base: int
    alloc_base: int
    alloc_protect: int
    region_size: int
    state: int
    protect: int
    type: int

    @property
    def is_committed(self) -> bool:
        return self.state == 0x1000  # MEM_COMMIT

    @property
    def is_readable(self) -> bool:
        readable = 0x02 | 0x04 | 0x20 | 0x40 | 0x08
        # PAGE_READONLY|RW|EXEC_READ|EXEC_RW|WRITECOPY
        no_access = 0x01 | 0x100  # PAGE_NOACCESS|GUARD
        return bool(self.protect & readable) and not (self.protect & no_access)

    @property
    def is_writable(self) -> bool:
        writable = 0x04 | 0x40 | 0x08  # RW|EXEC_RW|WRITECOPY
        no_access = 0x01 | 0x100
        return bool(self.protect & writable) and not (self.protect & no_access)


def _read_exact(sock: socket.socket, n: int) -> bytes:
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise RuntimeError(f"socket closed after {len(out)} of {n}")
        out.extend(chunk)
    return bytes(out)


class ZuneREPL:
    def __init__(self, ip: str, port: int = DEFAULT_PORT,
                 timeout: float = 30.0):
        self.ip = ip
        self.port = port
        self.timeout = timeout

    # ---------- raw invoke ----------

    def _invoke(self, sub_opcode: int, sub_args: bytes) -> tuple[int, bytes]:
        arg_b = struct.pack("<I", sub_opcode) + sub_args
        if len(arg_b) > 16384:
            raise ValueError(f"arg too long: {len(arg_b)} (max 16384)")

        with socket.create_connection((self.ip, self.port), timeout=self.timeout) as s:
            s.settimeout(self.timeout)
            banner = _read_exact(s, len(b"Hello\n"))
            if banner != b"Hello\n":
                raise RuntimeError(f"unexpected banner: {banner!r}")
            header = bytearray(32)
            header[0] = REPL_OPCODE
            header[1:5] = struct.pack("<I", len(arg_b))
            s.sendall(bytes(header))
            s.sendall(arg_b)

            resp = _read_exact(s, 32)
            if resp[0] != REPL_OPCODE:
                raise RuntimeError(f"opcode mismatch: 0x{resp[0]:02x}")
            rc = struct.unpack("<i", resp[1:5])[0]
            out_len = struct.unpack("<I", resp[5:9])[0]
            err = struct.unpack("<I", resp[9:13])[0]
            out = _read_exact(s, out_len) if out_len else b""

        if rc != 0 or err != 0:
            raise REPLError(err, f"REPL dispatch failed rc={rc} err=0x{err:08x}")
        if len(out) < 4:
            raise REPLError(0, f"response too short: {len(out)} bytes")
        status = struct.unpack("<I", out[:4])[0]
        return status, out[4:]

    def _check_ok(self, status: int):
        if status != STATUS_OK:
            raise REPLError(status)

    # ---------- typed methods ----------

    def list_processes(self) -> list[tuple[int, str]]:
        status, body = self._invoke(LIST_PROCESSES, b"")
        self._check_ok(status)
        results = []
        for off in range(0, len(body), 4 + 32):
            entry = body[off:off + 4 + 32]
            if len(entry) < 4 + 32:
                break
            pid = struct.unpack("<I", entry[:4])[0]
            name = entry[4:].split(b"\x00", 1)[0].decode("ascii", "replace")
            if pid == 0 and not name:
                continue
            results.append((pid, name))
        return results

    def list_modules(self, pid: int) -> list[tuple[int, int, str]]:
        status, body = self._invoke(LIST_MODULES, struct.pack("<I", pid))
        self._check_ok(status)
        results = []
        for off in range(0, len(body), 8 + 64):
            entry = body[off:off + 8 + 64]
            if len(entry) < 8 + 64:
                break
            base, size = struct.unpack("<II", entry[:8])
            name = entry[8:].split(b"\x00", 1)[0].decode("ascii", "replace")
            results.append((base, size, name))
        return results

    def virt_query_ex(self, pid: int, va: int) -> MBI:
        status, body = self._invoke(VIRT_QUERY_EX, struct.pack("<II", pid, va))
        self._check_ok(status)
        if len(body) < 28:
            raise REPLError(0, f"VQE response too short: {len(body)}")
        base, alloc_base, alloc_p, size, state, protect, mtype = struct.unpack(
            "<IIIIIII", body[:28])
        return MBI(base, alloc_base, alloc_p, size, state, protect, mtype)

    def read_proc_mem(self, pid: int, va: int, length: int) -> bytes:
        status, body = self._invoke(READ_PROC_MEM,
                                     struct.pack("<III", pid, va, length))
        self._check_ok(status)
        return body

    def write_proc_mem(self, pid: int, va: int, data: bytes) -> int:
        status, body = self._invoke(WRITE_PROC_MEM,
                                     struct.pack("<II", pid, va) + data)
        self._check_ok(status)
        if len(body) >= 4:
            return struct.unpack("<I", body[:4])[0]
        return 0

    def read_mem_local(self, va: int, length: int) -> bytes:
        status, body = self._invoke(READ_MEM_LOCAL,
                                     struct.pack("<II", va, length))
        self._check_ok(status)
        return body

    def kread(self, va: int, length: int) -> bytes:
        """Kernel read via GetFSHeapInfo exploit. Reads byte-at-a-time so
        a 4 KB read costs ~4 K kernel-side calls, slow but reaches any VA
        (per-slot, shared region, kernel range)."""
        status, body = self._invoke(KREAD_BYTES,
                                     struct.pack("<II", va, length))
        self._check_ok(status)
        return body

    def kwrite(self, va: int, data: bytes) -> int:
        """Kernel write via GetFSHeapInfo exploit. Same byte-at-a-time
        cost. Reaches any VA including read-only user-mode pages."""
        status, body = self._invoke(KWRITE_BYTES,
                                     struct.pack("<I", va) + data)
        self._check_ok(status)
        return struct.unpack("<I", body[:4])[0] if len(body) >= 4 else 0

    def map_ptr_to_process(self, pid: int, va: int) -> int:
        """Translate a per-process VA into a slot-encoded global address."""
        status, body = self._invoke(MAP_PTR_TO_PROC,
                                     struct.pack("<II", pid, va))
        self._check_ok(status)
        return struct.unpack("<I", body[:4])[0]

    def read_proc_kmem(self, pid: int, va: int, length: int) -> bytes:
        """Read any process's per-slot memory by translating VA via
        MapPtrToProcess + kernel-reading the result. Reaches xuidll @
        0x41800000 etc. that plain RPM can't see."""
        status, body = self._invoke(READ_PROC_KMEM,
                                     struct.pack("<III", pid, va, length))
        self._check_ok(status)
        return body

    def kcall(self, fn_ptr: int, arg0: int = 0, arg1: int = 0,
              arg2: int = 0, arg3: int = 0, arg4: int = 0,
              arg5: int = 0) -> tuple[int, int]:
        """Call an arbitrary kernel function via the extended hax()
        trampoline magic 0x1339. Up to 6 args (r0..r3 + 2 stack slots).
        Returns (result_r0, GetLastError)."""
        status, body = self._invoke(KCALL, struct.pack("<IIIIIII",
                                                          fn_ptr, arg0,
                                                          arg1, arg2, arg3,
                                                          arg4, arg5))
        self._check_ok(status)
        if len(body) < 8:
            raise REPLError(0, f"KCALL response short: {len(body)}")
        result, err = struct.unpack("<II", body[:8])
        return result, err

    def patch_target_code(self, target_proc: int, target_va: int,
                           new_bytes: bytes, *, throttle: float = 0.3) -> int:
        """Patch read-only code in target process via L2 PT-flip ("cydia-flip").

        CE 6 marks DLL code pages AP=110 (PL1 RO PL0 RO); even kernel-mode
        write faults. Workaround: temporarily flip the L2 entry's AP[2] bit
        (bit 9) to make the page PL1-writable, do the patch, restore AP[2].
        Helper v7 (planted at 0x80015380) handles the cross-process write +
        in-target-context cache management (DCCMVAU + ICIMVAU + DSB + ISB
        + BPIALL) so the patched bytes are coherent and the CPU re-fetches
        them on next execution.

        REQUIRES helpers v3 (0x80015220), tlb_flush (0x80015360), and v7
        (0x80015380) to be planted. See `project_xproc_vm_access.md`.

        Args:
          target_proc: kernel proc-struct VA (from NK process list walk)
          target_va: code VA to patch in the target's address space
          new_bytes: replacement bytes, max 64 bytes (2 cache lines).
                     For atomic patches, keep within one 32-byte line.
          throttle: seconds between TCP calls (default 0.3, daemon-safe)

        Returns: bytes-written count (= len(new_bytes) on success).

        Bracketed L2 restore is in a finally block so a crash during the
        patch leaves the page back at AP=110 (RO) for safety.
        """
        import struct as _s, time as _t
        if not 0 < len(new_bytes) <= 64:
            raise ValueError(f"patch size 1..64 bytes, got {len(new_bytes)}")
        KSCRATCH = 0x800152D0
        TLB_FLUSH = 0x80015360
        HELPER_V7 = 0x80015380

        # Locate the L2 PT entry for target_va in target's L1 PT
        proc_l1 = _s.unpack("<I", self.kread(target_proc + 0x2c, 4))[0]
        l1_idx = target_va >> 20
        e_l1 = _s.unpack("<I", self.kread(proc_l1 + l1_idx * 4, 4))[0]
        if (e_l1 & 3) != 1:
            raise RuntimeError(f"L1[{l1_idx}] not coarse PT (val=0x{e_l1:08x})")
        l2_kva = (e_l1 & 0xFFFFFC00) + 0x80000000
        l2_idx = (target_va - (l1_idx << 20)) >> 12
        l2_entry_kva = l2_kva + l2_idx * 4
        original_l2 = _s.unpack("<I", self.kread(l2_entry_kva, 4))[0]

        try:
            # Flip AP[2] → PL1 RW
            self.kwrite(l2_entry_kva, _s.pack("<I", original_l2 & ~0x200))
            _t.sleep(throttle)
            self.kcall(TLB_FLUSH)
            _t.sleep(throttle)
            # Stage + cross-process write (helper v7 handles cache mgmt)
            self.kwrite(KSCRATCH, new_bytes)
            _t.sleep(throttle)
            result, _ = self.kcall(HELPER_V7, target_proc, KSCRATCH,
                                     target_va, len(new_bytes))
            _t.sleep(throttle)
            return result
        finally:
            # Always restore L2 entry to its original AP (typically RO)
            self.kwrite(l2_entry_kva, _s.pack("<I", original_l2))
            _t.sleep(throttle)
            self.kcall(TLB_FLUSH)

    def xexec(self, target_proc: int, target_va: int, shellcode: bytes,
              *, throttle: float = 0.3) -> int:
        """Bracketed inject-and-execute shellcode in target process.

        4-step bracket via Python-side orchestration (5 TCP round-trips,
        ~1s wall time per call). Uses planted kernel helpers v3 (memcpy
        with TTBR swap, at 0x80015220) and v4 (bx with TTBR swap, at
        0x80015280).

          1. kwrite shellcode → KSCRATCH (0x800152D0)
          2. helper v3 read original target_va → KORIG (0x80015320)
          3. helper v3 write KSCRATCH → target_va
          4. helper v4 execute target_va (returns shellcode's r0)
          5. helper v3 restore KORIG → target_va

        REQUIRES helpers v3 (88 bytes) + v4 (68 bytes) planted at the
        addresses above. Re-plant per session. The kernel reuses the scratch
        region under load. See `project_xproc_vm_access.md`.

        Throttle (default 0.3s) sleeps between TCP calls. Without this the
        daemon's plugin handler degrades / crashes after a few rapid calls.

        Args:
          target_proc: kernel process-struct VA (from NK process list walk)
          target_va: VA in target process to inject + execute. Must be
                     writable+executable. CE 6 heap pages typically are
                     (AP[2:0]=011 + XN clear).
          shellcode: ARM bytecode, max 64 bytes. Must end with `bx lr`
                     (or `pop {..., pc}` with saved lr).
          throttle: seconds between TCP calls (default 0.3).

        Returns: shellcode's r0 result.
        """
        import time
        KSCRATCH = 0x800152D0
        KORIG    = 0x80015320
        HELPER_V3 = 0x80015220
        HELPER_V4 = 0x80015280
        if not 0 < len(shellcode) <= 64:
            raise ValueError(f"shellcode size must be 1..64 bytes, got {len(shellcode)}")
        size = len(shellcode)

        self.kwrite(KSCRATCH, shellcode)
        time.sleep(throttle)
        # save original at target_va
        self.kcall(HELPER_V3, target_proc, target_va, KORIG, size)
        time.sleep(throttle)
        # write shellcode to target_va
        self.kcall(HELPER_V3, target_proc, KSCRATCH, target_va, size)
        time.sleep(throttle)
        # execute
        result, _ = self.kcall(HELPER_V4, target_proc, target_va)
        time.sleep(throttle)
        # restore original
        self.kcall(HELPER_V3, target_proc, KORIG, target_va, size)
        return result

    def pull_file(self, path: str, offset: int = 0, length: int = 0x1F00) -> bytes:
        """Read up to `length` bytes from device file `path` starting at
        `offset`. Caller chunks for files larger than the per-call cap
        (~8 KB). Returns the bytes read (may be shorter than `length` at EOF)."""
        path_utf16 = path.encode("utf-16-le")
        args = struct.pack("<III", offset, length, len(path_utf16)) + path_utf16
        status, body = self._invoke(PULL_FILE, args)
        self._check_ok(status)
        return body

    def dump_va_to_file(self, va: int, length: int, path: str) -> tuple[int, int]:
        """Snapshot `length` bytes from kernel/user VA `va` into a file on the
        device at `path` (CE 6 path like '\\flash2\\zpod-audio.pcm').

        All work happens on-device, no per-chunk TCP read. The plugin uses
        PL1 kernel memcpy to copy from `va` into a local 32 KB scratch then
        WriteFile to flash, chunk by chunk. One TCP call covers the whole
        dump.

        Returns (status, total_written). status==0 on success; non-zero
        encodes a GetLastError in the low 16 bits."""
        path_utf16 = path.encode("utf-16-le")
        args = struct.pack("<III", va, length, len(path_utf16)) + path_utf16
        status, body = self._invoke(DUMP_VA_TO_FILE, args)
        total = struct.unpack("<I", body[:4])[0] if len(body) >= 4 else 0
        return status, total

    # ---------- helpers ----------

    def find_pid(self, name: str) -> int | None:
        for pid, n in self.list_processes():
            if n.lower().endswith(name.lower()):
                return pid
        return None

    def virt_walk(self, pid: int, start: int = 0x10000, end: int = 0x80000000):
        """Iterate over compositor's VA regions via VirtualQueryEx.
        Yields MBI for each region, advancing past RegionSize each step."""
        va = start
        while va < end:
            try:
                mbi = self.virt_query_ex(pid, va)
            except REPLError:
                break
            yield mbi
            nxt = (mbi.base or va) + max(mbi.region_size, 0x1000)
            if nxt <= va:
                va += 0x1000
            else:
                va = nxt
