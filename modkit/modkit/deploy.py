"""Mod deployment to the device.

`zuxhook`'s C runtime owns all manifest lowering and capability execution
on-device (documented in the wiki, https://wiki.zune.moe). Host tooling does
not run capabilities; it mirrors each mod's source directory to
`\\flash2\\automation\\mods\\<id>\\` and lets zuxhook lower + apply on the next
boot. enabled.json is owned entirely by zuxhook's mod_scanner, not written here.

`deploy_mods` is the deploy entrypoint. `restart_gemstone` is a dev convenience
that re-runs zuxhook's per-gemstone Phase 2 without a full reboot.
"""
from __future__ import annotations
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Any
import struct
import sys

from .manifest import Mod, ManifestError
from .payload import deployable_files


@dataclass
class ApplyContext:
    """Device-deploy context: target IP, a lazy ZuneREPL, and the cached
    gemstone pid used by restart_gemstone."""
    device_ip: str
    repl: Any = None                          # ZuneREPL: lazy-init
    gem_pid: Optional[int] = None
    log: list[str] = field(default_factory=list)

    def emit(self, msg: str):
        self.log.append(msg)
        print(msg)

    def get_repl(self):
        if self.repl is None:
            tools_dir = str(Path(__file__).resolve().parents[2] / "tools" / "general")
            sys.path.insert(0, tools_dir)
            from zune_repl import ZuneREPL
            self.repl = ZuneREPL(self.device_ip, timeout=30.0)
        return self.repl

    def get_gem_pid(self) -> int:
        if self.gem_pid is not None:
            return self.gem_pid
        r = self.get_repl()
        for pid, name in r.list_processes():
            if "gemstone" in name.lower():
                self.gem_pid = pid
                return pid
        raise RuntimeError("gemstone pid not found")

    def invalidate_runtime_state(self):
        """Clear cached gemstone identifiers after a restart (new pid)."""
        self.gem_pid = None

    def get_compositor_proc(self) -> int:
        """Find compositor's kernel proc-struct VA. Used for cross-proc
        injection that needs to live OUTSIDE gemstone (e.g., to kill it
        cleanly via TerminateProcess from a stable process)."""
        import struct
        r = self.get_repl()
        PROC_LIST_HEAD = 0x80BEE010
        head = struct.unpack("<I", r.kread(PROC_LIST_HEAD, 4))[0]
        cur = head
        while cur:
            blk = r.kread(cur, 0x40)
            nxt, = struct.unpack_from("<I", blk, 0)
            np, = struct.unpack_from("<I", blk, 0x20)
            if np:
                n = r.kread(np, 64).split(b"\x00\x00", 1)[0].decode(
                    "utf-16-le", "replace")
                if "compositor" in n.lower():
                    return cur
            cur = nxt
        raise RuntimeError("compositor process not found")

_DAEMON_WRITE_FILE_MAX = 16384  # must match WRFILE_DATA_MAX in nativeapp


def _mkdir_on_device(device_ip: str, remote_path: str,
                     timeout: float = 10.0) -> None:
    """Create a single directory on the device via opcode 13. Returns
    silently on ERROR_ALREADY_EXISTS (183)."""
    import socket as _socket
    path_bytes = remote_path.encode("utf-8")
    with _socket.create_connection((device_ip, 1337), timeout=timeout) as sock:
        sock.settimeout(timeout)
        banner = b""
        while len(banner) < 6:
            ch = sock.recv(6 - len(banner))
            if not ch:
                raise RuntimeError("mkdir: socket closed before banner")
            banner += ch
        header = bytearray(32)
        header[0] = 13
        header[1:5] = struct.pack("<I", len(path_bytes))
        sock.sendall(bytes(header))
        sock.sendall(path_bytes)
        resp = b""
        while len(resp) < 32:
            ch = sock.recv(32 - len(resp))
            if not ch:
                raise RuntimeError("mkdir: socket closed during response")
            resp += ch
        if resp[0] != 13:
            raise RuntimeError(f"mkdir: bad opcode 0x{resp[0]:02x}")
        err = struct.unpack("<I", resp[1:5])[0]
        ok = resp[5] != 0
        if not ok and err != 183:  # 183 = ERROR_ALREADY_EXISTS
            raise RuntimeError(
                f"mkdir {remote_path}: err=0x{err:08x}")


def _mkdir_p_on_device(device_ip: str, remote_path: str) -> None:
    """mkdir -p semantics: create each parent in the path. `remote_path`
    is the directory itself, e.g. \\flash2\\automation\\mods\\<mod-id>."""
    parts = [p for p in remote_path.split("\\") if p]
    # Build cumulative paths starting from \drive\firstdir
    cur = ""
    for p in parts:
        cur = (cur + "\\" if cur else "\\") + p
        # Skip the drive root "\flash2" / "\flash"; CE 6 doesn't let us
        # CreateDirectoryW on a drive root anyway.
        if cur.count("\\") == 1:
            continue
        _mkdir_on_device(device_ip, cur)


def _movefile_on_device(device_ip: str, src: str, dst: str,
                        timeout: float = 10.0) -> int:
    """Move/rename a file on the device via opcode 17 (MoveFileW). Returns
    GetLastError. Caller decides whether non-zero err is fatal, e.g. a
    ERROR_FILE_NOT_FOUND on the move-aside path means the live file
    isn't there yet, which is fine."""
    import socket as _socket
    src_bytes = src.encode("utf-8")
    dst_bytes = dst.encode("utf-8")
    with _socket.create_connection((device_ip, 1337), timeout=timeout) as sock:
        sock.settimeout(timeout)
        banner = b""
        while len(banner) < 6:
            ch = sock.recv(6 - len(banner))
            if not ch:
                raise RuntimeError("movefile: socket closed before banner")
            banner += ch
        header = bytearray(32)
        header[0] = 17
        header[1:5] = struct.pack("<I", len(src_bytes))
        header[5:9] = struct.pack("<I", len(dst_bytes))
        sock.sendall(bytes(header))
        sock.sendall(src_bytes)
        sock.sendall(dst_bytes)
        resp = b""
        while len(resp) < 32:
            ch = sock.recv(32 - len(resp))
            if not ch:
                raise RuntimeError("movefile: socket closed during response")
            resp += ch
        if resp[0] != 17:
            raise RuntimeError(f"movefile: bad opcode 0x{resp[0]:02x}")
        err = struct.unpack("<I", resp[1:5])[0]
        ok = resp[5] != 0
        return 0 if ok else err


def _push_file_chunked(device_ip: str, local: Path, remote: str,
                        timeout: float = 30.0) -> None:
    """Push a single file to the device via nativeapp's opcode 12 wire
    protocol (same path as tools/general/lyra-write-file.py).

    On ERROR_SHARING_VIOLATION (0x20) at offset 0, falls back to
    move-then-copy (memory: feedback_zpod_deploy_move_then_copy): the
    live file gets renamed to `<remote>.bak-<timestamp>` via opcode 17
    and the write retries once against the freed path."""
    err = _push_file_chunked_once(device_ip, local, remote, timeout=timeout)
    if err == 0:
        return
    if err != 0x20:   # ERROR_SHARING_VIOLATION
        raise RuntimeError(
            f"deploy: write {remote} failed at +0: err=0x{err:08x}")
    # File in use: rename live aside, retry once.
    import time as _time
    bak = f"{remote}.bak-{_time.strftime('%Y%m%d-%H%M%S')}"
    mv_err = _movefile_on_device(device_ip, remote, bak)
    if mv_err != 0:
        raise RuntimeError(
            f"deploy: {remote} held open (0x20) and move-aside to {bak} "
            f"failed: err=0x{mv_err:08x}")
    err2 = _push_file_chunked_once(device_ip, local, remote, timeout=timeout)
    if err2 != 0:
        raise RuntimeError(
            f"deploy: write {remote} failed after move-aside to {bak}: "
            f"err=0x{err2:08x}")


def _push_file_chunked_once(device_ip: str, local: Path, remote: str,
                             timeout: float = 30.0) -> int:
    """Inner write loop. Returns 0 on success, GetLastError on first-chunk
    failure (so the caller can decide whether to retry move-then-copy).
    Mid-write failures still raise."""
    import socket as _socket
    data = local.read_bytes()
    path_bytes = remote.encode("utf-8")
    if len(path_bytes) > 256:
        raise RuntimeError(f"deploy: path too long ({len(path_bytes)} B)")

    with _socket.create_connection((device_ip, 1337), timeout=timeout) as sock:
        sock.settimeout(timeout)
        # Banner: "Hello\n"
        banner = b""
        while len(banner) < 6:
            ch = sock.recv(6 - len(banner))
            if not ch:
                raise RuntimeError("deploy: socket closed before banner")
            banner += ch
        if banner != b"Hello\n":
            raise RuntimeError(f"deploy: unexpected banner {banner!r}")

        offset = 0
        # Empty-file special case: still send one zero-length chunk so
        # CreateFile(CREATE_ALWAYS) fires.
        while offset < len(data) or (offset == 0 and len(data) == 0):
            chunk = data[offset:offset + _DAEMON_WRITE_FILE_MAX]
            header = bytearray(32)
            header[0] = 12
            header[1:5] = struct.pack("<I", len(path_bytes))
            header[5:9] = struct.pack("<I", offset)
            header[9:13] = struct.pack("<I", len(chunk))
            sock.sendall(bytes(header))
            sock.sendall(path_bytes)
            if chunk:
                sock.sendall(chunk)

            resp = b""
            while len(resp) < 32:
                ch = sock.recv(32 - len(resp))
                if not ch:
                    raise RuntimeError(
                        f"deploy: socket closed during response at +{offset}")
                resp += ch
            if resp[0] != 12:
                raise RuntimeError(
                    f"deploy: bad response opcode 0x{resp[0]:02x} at +{offset}")
            bw = struct.unpack("<I", resp[1:5])[0]
            err = struct.unpack("<I", resp[5:9])[0]
            ok = resp[9] != 0
            if not ok:
                if offset == 0:
                    return err   # let caller decide (move-then-copy retry)
                raise RuntimeError(
                    f"deploy: write {remote} failed at +{offset}: "
                    f"bw={bw} err=0x{err:08x}")
            offset += bw
            if len(data) == 0:
                break
    return 0


def _deploy_mod_dirs(mods: list[Mod], ctx: ApplyContext) -> None:
    """Deploy each active mod's payload (deployable_files: manifest + referenced
    blobs and assets) to \\flash2\\automation\\mods\\<id>\\ on the device. Lets
    zuxhook's boot-time scanner find manifests + blob files (incl. the binaries
    spawn_daemon points at). Authoring-only sources (.xui, .psd) are not
    referenced, so they never ship.

    enabled.json is deliberately NOT written here: zuxhook's mod_scanner
    is the sole authority on enable-state (it scans every mod dir, loads
    system mods unconditionally, and derives content-mod enablement from
    the on-device enabled.json it owns and rewrites). A freshly deployed
    content mod starts disabled until toggled on in the Mods tab.
    """
    if not mods:
        return
    ctx.emit(f"\n== Deploy mod dirs to device ==")
    # Make sure both roots exist before any per-mod operations.
    _mkdir_p_on_device(ctx.device_ip, "\\flash2\\automation\\mods")
    _mkdir_p_on_device(ctx.device_ip, "\\flash2\\automation\\platform")

    for m in mods:
        # Platform components (dirs under lyra/platform/) mirror to
        # \flash2\automation\platform\<id>; feature mods to \...\mods\<id>.
        # Same split the packager/installer use (Content/platform vs Content/mods).
        root = "platform" if m.source_dir.parent.name == "platform" else "mods"
        remote_root = f"\\flash2\\automation\\{root}\\{m.mod_id}"
        _mkdir_p_on_device(ctx.device_ip, remote_root)
        for f in sorted(deployable_files(m)):
            rel = f.relative_to(m.source_dir)
            remote = remote_root + "\\" + str(rel).replace("/", "\\")
            # Ensure any intermediate sub-directories exist.
            parent = "\\".join(remote.split("\\")[:-1])
            if parent != remote_root:
                _mkdir_p_on_device(ctx.device_ip, parent)
            _push_file_chunked(ctx.device_ip, f, remote)
            ctx.emit(f"    {m.mod_id}: {rel} -> {remote}")


def restart_gemstone(ctx: ApplyContext, *, timeout_s: float = 8.0) -> int:
    """Kill gemstone via TerminateProcess from compositor; wait for relaunch.

    Returns the NEW gemstone pid. Caller should re-apply Phase 2 after.
    Per exp 083, supervisor relaunches gemstone in ~2s. We poll up to
    timeout_s for the new pid (must differ from old).
    """
    import struct, time
    r = ctx.get_repl()
    old_pid = ctx.get_gem_pid()
    comp_proc = ctx.get_compositor_proc()
    ctx.emit(f"restart-gemstone: old gemstone pid 0x{old_pid:08x}")

    # Shellcode planted in compositor's heap: TerminateProcess(gem_pid, 0)
    TERMINATE_PROCESS_VA = 0x4033309c   # coredll.dll!TerminateProcess (v4.5)
    sc = struct.pack('<6I',
        0xe52de004,                       # push {lr}
        0xe59f000c,                       # ldr r0, [pc, #0x0c]  ; pool[0] = pid
        0xe3a01000,                       # mov r1, #0
        0xe59f2008,                       # ldr r2, [pc, #0x08]  ; pool[1] = TerminateProcess
        0xe12fff32,                       # blx r2
        0xe49df004,                       # pop {pc}
    ) + struct.pack('<2I', old_pid, TERMINATE_PROCESS_VA)

    SC_VA = 0x000F4000   # compositor heap scratch (validated in exp 067)
    KSCRATCH = 0x800152D0
    HELPER_V4 = 0x80015280
    HELPER_V7 = 0x80015380

    # Plant shellcode in compositor
    for off in range(0, len(sc), 64):
        chunk = sc[off:off+64]
        r.kwrite(KSCRATCH, chunk)
        time.sleep(0.4)
        n = r.kcall(HELPER_V7, comp_proc, KSCRATCH, SC_VA + off, len(chunk))[0]
        if n != len(chunk):
            raise RuntimeError(f"restart_gemstone: plant short write at +0x{off:x}")
    ctx.emit(f"  planted TerminateProcess shellcode in compositor @ 0x{SC_VA:08x}")

    # Execute. Note this may "fail" with EPIPE-style errors as gemstone
    # tears down concurrently; we expect a non-zero return from helper_v4
    # to mean "TerminateProcess returned TRUE".
    try:
        result, _ = r.kcall(HELPER_V4, comp_proc, SC_VA)
        ctx.emit(f"  TerminateProcess returned 0x{result:x}")
    except Exception as e:
        ctx.emit(f"  kcall returned exception (expected during teardown): {e}")

    # Poll for new gemstone pid
    start = time.time()
    new_pid = old_pid
    while time.time() - start < timeout_s:
        time.sleep(0.5)
        try:
            for pid, name in r.list_processes():
                if "gemstone" in name.lower() and pid != old_pid:
                    new_pid = pid
                    break
            if new_pid != old_pid:
                break
        except Exception:
            continue
    if new_pid == old_pid:
        raise RuntimeError(
            f"restart_gemstone: no new pid after {timeout_s}s (supervisor stuck?)")
    elapsed = time.time() - start
    ctx.emit(f"  new gemstone pid 0x{new_pid:08x} (relaunched in {elapsed:.2f}s)")

    # Invalidate cached state, since runtime patches in old gemstone are gone
    ctx.invalidate_runtime_state()
    return new_pid


# Public deploy entrypoint (raw mod-dir mirror; zuxhook owns enabled.json).
deploy_mods = _deploy_mod_dirs
