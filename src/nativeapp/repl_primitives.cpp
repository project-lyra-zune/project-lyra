#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

#include "kerncore.h"
#include "bytepack.h"

typedef unsigned int u32;

// CE 6 cross-process RPM works via OpenProcess+ReadProcessMemory but only
// for slot-0 addresses of the target process (0x00010000..0x02000000).
// Per-slot addresses (xuidll at 0x41800000 etc.) are not RPM-accessible
// without slot remapping, which the existing daemon doesn't do.

// REPL primitives linked directly into nativeapp. Each call dispatches
// one (sub_opcode, args) command and returns one (status, response)
// pair. Runs inside nativeapp.exe, kernel-privileged after hax(),
// full cross-process access to gemstone / compositor / etc.
//
// Called from nativeapp's connection() opcode 40 (REPL_DISPATCH) wire
// handler.
//
// Wire format:
//   arg layout (LE u32 fields):
//     +0  sub_opcode
//     +4  args (variable)
//   out layout:
//     +0  status (u32; 0 = ok, else GetLastError or sub-specific error)
//     +4  response bytes (variable)
//   out_used = 4 + len(response)
//
// Sub-opcodes:
//   1  READ_PROC_MEM   (pid u32, va u32, len u32)  -> bytes (up to len)
//   2  WRITE_PROC_MEM  (pid u32, va u32, bytes)    -> (empty)
//   3  VIRT_QUERY_EX   (pid u32, va u32)           -> MBI 28 bytes
//   4  LIST_PROCESSES  (no args)                   -> [pid u32, name 32B]*
//   5  LIST_MODULES    (pid u32)                   -> [base u32, size u32, name 64B]*
//   6  READ_MEM_LOCAL  (va u32, len u32)           -> bytes
//   7  KREAD_BYTES     (va u32, len u32)           -> bytes
//   8  KWRITE_BYTES    (va u32, bytes)             -> u32 written
//   9  MAP_PTR_TO_PROC (pid u32, va u32)           -> u32 (global VA)
//   10 READ_PROC_KMEM  (pid u32, va u32, len u32)  -> bytes
//   11 KCALL           (fn u32, a0..a5 u32)        -> result u32, lasterr u32 (PL1 trampoline)
//   13 USER_CALL       (fn u32, a0..a11 u32)       -> result u32, lasterr u32 (user-mode, in nativeapp)
//   14 DUMP_VA_TO_FILE (va u32, len u32, path_len u32, path_utf16le)
//                                                  -> total_written u32
//      All on-device: opens path, repeatedly memcpys (via PL1 kernel
//      memcpy at 0x80072318 invoked through 0x1339) from `va` into a
//      32 KB scratch then WriteFiles to flash. Eliminates the
//      ~17 KB/s TCP-per-chunk bottleneck of read+host-write for live
//      ring-buffer snapshots (AVP audio capture etc.).
//   15 PULL_FILE       (offset u32, length u32, path_len u32, path_utf16le)
//                                                  -> bytes (up to ~8 KB/call)
//      Companion to DUMP_VA_TO_FILE; reads `length` bytes from `path`
//      starting at `offset`. Caller chunks for files larger than the
//      ~8 KB per-call output cap.
//
// Output cap is the op_40 caller's out_max (rpc_server.cpp): 256 KB.
// Caller chunks reads larger than that into multiple calls.

#define SUB_READ_PROC_MEM   1
#define SUB_WRITE_PROC_MEM  2
#define SUB_VIRT_QUERY_EX   3
#define SUB_LIST_PROCESSES  4
#define SUB_LIST_MODULES    5
#define SUB_READ_MEM_LOCAL  6
#define SUB_KREAD_BYTES     7
#define SUB_KWRITE_BYTES    8
#define SUB_MAP_PTR_TO_PROC 9
#define SUB_READ_PROC_KMEM  10
#define SUB_KCALL           11
#define SUB_USER_CALL       13
#define SUB_DUMP_VA_TO_FILE 14
#define SUB_PULL_FILE       15

// CE 6 kernel memcpy (Pavo v4.5). Invoked via the SUB_KCALL trampoline
// (magic 0x1339) so it runs in PL1 with the target proc's TTBR0, so it
// can read kernel-heap source (0xd0xxxxxx) and write to the plugin's user
// heap dest in one call.
#define KMEMCPY_FN          0x80072318u

#define STATUS_OK                0u
#define STATUS_BAD_ARG_LEN       0xE0000001u
#define STATUS_UNKNOWN_OPCODE    0xE0000002u
#define STATUS_OUT_TOO_SMALL     0xE0000003u
#define STATUS_OPENPROC_FAIL     0xE0000010u
#define STATUS_RPM_FAIL          0xE1000000u
#define STATUS_WPM_FAIL          0xE2000000u
#define STATUS_VQE_FAIL          0xE3000000u
#define STATUS_SNAPSHOT_FAIL     0xE0000020u

// Set the (status, resp_len) header at out and return out_used.
static int finish(unsigned char* out, u32 status, u32 resp_len) {
    write_u32(out, status);
    return (int)(4 + resp_len);
}

// ---------- handlers ----------

static int handle_read_proc_mem(const unsigned char* arg, int arg_len,
                                 unsigned char* out, int out_max) {
    if (arg_len < 12) return finish(out, STATUS_BAD_ARG_LEN, 0);
    u32 pid = read_u32(arg + 0);
    u32 va  = read_u32(arg + 4);
    u32 len = read_u32(arg + 8);
    if (len > (u32)(out_max - 4)) len = (u32)(out_max - 4);

    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (h == NULL) return finish(out, STATUS_OPENPROC_FAIL | (GetLastError() & 0xFFFF), 0);

    DWORD got = 0;
    BOOL r = ReadProcessMemory(h, (LPCVOID)va, out + 4, len, &got);
    DWORD err = r ? 0 : GetLastError();
    CloseHandle(h);

    if (!r) return finish(out, STATUS_RPM_FAIL | (err & 0xFFFF), 0);
    return finish(out, STATUS_OK, (u32)got);
}

static int handle_write_proc_mem(const unsigned char* arg, int arg_len,
                                  unsigned char* out, int out_max) {
    if (arg_len < 8) return finish(out, STATUS_BAD_ARG_LEN, 0);
    u32 pid = read_u32(arg + 0);
    u32 va  = read_u32(arg + 4);
    int data_len = arg_len - 8;
    if (data_len < 0) data_len = 0;

    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (h == NULL) return finish(out, STATUS_OPENPROC_FAIL | (GetLastError() & 0xFFFF), 0);

    DWORD wrote = 0;
    BOOL r = WriteProcessMemory(h, (LPVOID)va, (LPVOID)(arg + 8), data_len, &wrote);
    DWORD err = r ? 0 : GetLastError();
    CloseHandle(h);

    if (!r) return finish(out, STATUS_WPM_FAIL | (err & 0xFFFF), 0);
    write_u32(out + 4, (u32)wrote);
    return finish(out, STATUS_OK, 4);
}

static int handle_virt_query_ex(const unsigned char* arg, int arg_len,
                                 unsigned char* out, int out_max) {
    if (arg_len < 8) return finish(out, STATUS_BAD_ARG_LEN, 0);
    if (out_max < 4 + 28) return finish(out, STATUS_OUT_TOO_SMALL, 0);
    u32 pid = read_u32(arg + 0);
    u32 va  = read_u32(arg + 4);

    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (h == NULL) return finish(out, STATUS_OPENPROC_FAIL | (GetLastError() & 0xFFFF), 0);

    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T got = VirtualQueryEx(h, (LPCVOID)va, &mbi, sizeof(mbi));
    DWORD err = (got == 0) ? GetLastError() : 0;
    CloseHandle(h);

    if (got == 0) return finish(out, STATUS_VQE_FAIL | (err & 0xFFFF), 0);

    // Layout: BaseAddress, AllocationBase, AllocationProtect, RegionSize,
    // State, Protect, Type - 7 u32s = 28 bytes.
    write_u32(out + 4 + 0,  (u32)mbi.BaseAddress);
    write_u32(out + 4 + 4,  (u32)mbi.AllocationBase);
    write_u32(out + 4 + 8,  mbi.AllocationProtect);
    write_u32(out + 4 + 12, (u32)mbi.RegionSize);
    write_u32(out + 4 + 16, mbi.State);
    write_u32(out + 4 + 20, mbi.Protect);
    write_u32(out + 4 + 24, mbi.Type);
    return finish(out, STATUS_OK, 28);
}

static int handle_list_processes(unsigned char* out, int out_max) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return finish(out, STATUS_SNAPSHOT_FAIL | (GetLastError() & 0xFFFF), 0);
    }
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    int written = 0;
    int max_entries = (out_max - 4) / (4 + 32);
    int entries = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (entries >= max_entries) break;
            unsigned char* dst = out + 4 + entries * (4 + 32);
            write_u32(dst, pe.th32ProcessID);
            // Truncate to 32 chars (UTF-16, so 16 wchar_t pairs = 32 bytes).
            // We expose as ASCII for simplicity.
            for (int i = 0; i < 32; i++) {
                wchar_t c = pe.szExeFile[i];
                dst[4 + i] = (c < 0x80) ? (unsigned char)c : '?';
                if (c == 0) {
                    for (int j = i + 1; j < 32; j++) dst[4 + j] = 0;
                    break;
                }
            }
            entries++;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return finish(out, STATUS_OK, (u32)(entries * (4 + 32)));
}

static int handle_list_modules(const unsigned char* arg, int arg_len,
                                unsigned char* out, int out_max) {
    if (arg_len < 4) return finish(out, STATUS_BAD_ARG_LEN, 0);
    u32 pid = read_u32(arg + 0);

    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_GETALLMODS, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        return finish(out, STATUS_SNAPSHOT_FAIL | (GetLastError() & 0xFFFF), 0);
    }
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    int max_entries = (out_max - 4) / (8 + 64);
    int entries = 0;
    if (Module32First(snap, &me)) {
        do {
            if (entries >= max_entries) break;
            unsigned char* dst = out + 4 + entries * (8 + 64);
            write_u32(dst + 0, (u32)me.modBaseAddr);
            write_u32(dst + 4, me.modBaseSize);
            for (int i = 0; i < 64; i++) {
                wchar_t c = me.szModule[i];
                dst[8 + i] = (c < 0x80) ? (unsigned char)c : '?';
                if (c == 0) {
                    for (int j = i + 1; j < 64; j++) dst[8 + j] = 0;
                    break;
                }
            }
            entries++;
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return finish(out, STATUS_OK, (u32)(entries * (8 + 64)));
}

static int handle_read_mem_local(const unsigned char* arg, int arg_len,
                                  unsigned char* out, int out_max) {
    if (arg_len < 8) return finish(out, STATUS_BAD_ARG_LEN, 0);
    u32 va  = read_u32(arg + 0);
    u32 len = read_u32(arg + 4);
    if (len > (u32)(out_max - 4)) len = (u32)(out_max - 4);

    __try {
        memcpy(out + 4, (const void*)va, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return finish(out, STATUS_RPM_FAIL, 0);
    }
    return finish(out, STATUS_OK, len);
}


static int handle_kread_bytes(const unsigned char* arg, int arg_len,
                               unsigned char* out, int out_max) {
    if (arg_len < 8) return finish(out, STATUS_BAD_ARG_LEN, 0);
    u32 va  = read_u32(arg + 0);
    u32 len = read_u32(arg + 4);
    if (len > (u32)(out_max - 4)) len = (u32)(out_max - 4);

    __try {
        kerncore_kread(va, out + 4, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return finish(out, STATUS_RPM_FAIL, 0);
    }
    return finish(out, STATUS_OK, len);
}

// MapPtrToProcess: translate a per-process VA in target process to a
// slot-encoded global address that can be dereferenced from anywhere.
typedef LPVOID (WINAPI *PfnMapPtrToProcess)(LPVOID lpv, HANDLE hProc);

static int handle_map_ptr_to_proc(const unsigned char* arg, int arg_len,
                                    unsigned char* out, int out_max) {
    if (arg_len < 8) return finish(out, STATUS_BAD_ARG_LEN, 0);
    u32 pid = read_u32(arg + 0);
    u32 va  = read_u32(arg + 4);

    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (h == NULL) return finish(out, STATUS_OPENPROC_FAIL | (GetLastError() & 0xFFFF), 0);

    HMODULE coredll = GetModuleHandleW(L"coredll.dll");
    PfnMapPtrToProcess Map = (PfnMapPtrToProcess)GetProcAddress(coredll, L"MapPtrToProcess");
    if (Map == NULL) {
        CloseHandle(h);
        return finish(out, STATUS_UNKNOWN_OPCODE, 0);
    }

    LPVOID translated = Map((LPVOID)va, h);
    CloseHandle(h);

    write_u32(out + 4, (u32)translated);
    return finish(out, STATUS_OK, 4);
}

// KCALL: invoke an arbitrary kernel function via the extended hax()
// trampoline magic 0x1339. Args struct: {fn_ptr, arg0..arg5}.
// Trampoline (in kernel mode) loads them, calls fn_ptr(arg0..arg5)
// (r0-r3 plus two stack slots), returns the function's r0. Requires
// nativeapp built with the extended-trampoline hax().
static int handle_kcall(const unsigned char* arg, int arg_len,
                         unsigned char* out, int out_max) {
    if (arg_len < 28) return finish(out, STATUS_BAD_ARG_LEN, 0);
    DWORD fn_args[7];
    fn_args[0] = read_u32(arg + 0);   // fn_ptr
    fn_args[1] = read_u32(arg + 4);   // arg0
    fn_args[2] = read_u32(arg + 8);   // arg1
    fn_args[3] = read_u32(arg + 12);  // arg2
    fn_args[4] = read_u32(arg + 16);  // arg3
    fn_args[5] = read_u32(arg + 20);  // arg4 (5th, stack sp+0)
    fn_args[6] = read_u32(arg + 24);  // arg5 (6th, stack sp+4)

    DWORD result = 0;
    DWORD err = 0;
    __try {
        result = kerncore_kcall_le(fn_args[0], fn_args[1], fn_args[2],
                                    fn_args[3], fn_args[4], fn_args[5],
                                    fn_args[6], &err);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return finish(out, STATUS_RPM_FAIL, 0);
    }
    write_u32(out + 4, result);
    write_u32(out + 8, err);
    return finish(out, STATUS_OK, 8);
}

// Combined: translate target_va via MapPtrToProcess, then kread the
// resulting slot-encoded global address. Lets us read any process's
// per-slot memory in a single call.
static int handle_read_proc_kmem(const unsigned char* arg, int arg_len,
                                   unsigned char* out, int out_max) {
    if (arg_len < 12) return finish(out, STATUS_BAD_ARG_LEN, 0);
    u32 pid = read_u32(arg + 0);
    u32 va  = read_u32(arg + 4);
    u32 len = read_u32(arg + 8);
    if (len > (u32)(out_max - 4)) len = (u32)(out_max - 4);

    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (h == NULL) return finish(out, STATUS_OPENPROC_FAIL | (GetLastError() & 0xFFFF), 0);

    HMODULE coredll = GetModuleHandleW(L"coredll.dll");
    PfnMapPtrToProcess Map = (PfnMapPtrToProcess)GetProcAddress(coredll, L"MapPtrToProcess");
    if (Map == NULL) {
        CloseHandle(h);
        return finish(out, STATUS_UNKNOWN_OPCODE, 0);
    }

    LPVOID translated = Map((LPVOID)va, h);
    CloseHandle(h);

    if (translated == NULL) return finish(out, STATUS_VQE_FAIL, 0);

    u32 base_va = (u32)translated;
    __try {
        kerncore_kread(base_va, out + 4, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return finish(out, STATUS_RPM_FAIL, 0);
    }
    return finish(out, STATUS_OK, len);
}

// SUB_USER_CALL: call any user-mode function from the plugin's own thread
// context. Unlike SUB_KCALL (which routes through the GetFSHeapInfo 0x1339
// trampoline and runs in PL1 from NK's kernel thread), this is a direct
// C call from inside nativeapp.exe user mode, required for CE APIs that
// track per-thread/per-process state (Storage Manager, handle tables,
// per-thread error etc.).
//
// Args: fn (u32), a0..a11 (up to 12 u32s). Returns: result (u32),
// last_error (u32). 12 args covers RegCreateKeyExW (9 args), every other
// CE coredll surface we've needed, plus headroom. r0-r3 hold args 0-3;
// args 4-11 spill onto the stack per AAPCS. Callers may supply fewer
// than 12 args (down to 0); missing trailing args default to 0.
typedef DWORD (*PfnUser12)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                            DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);

static int handle_user_call(const unsigned char* arg, int arg_len,
                             unsigned char* out, int out_max) {
    if (arg_len < 4) return finish(out, STATUS_BAD_ARG_LEN, 0);
    if (out_max < 12) return finish(out, STATUS_OUT_TOO_SMALL, 0);
    DWORD fn = read_u32(arg + 0);
    DWORD a[12] = {0};
    int n_avail = (arg_len - 4) / 4;
    if (n_avail > 12) n_avail = 12;
    for (int i = 0; i < n_avail; i++) {
        a[i] = read_u32(arg + 4 + i * 4);
    }
    DWORD result = 0;
    DWORD err = 0;
    __try {
        SetLastError(0);
        result = ((PfnUser12)fn)(a[0], a[1], a[2], a[3], a[4], a[5],
                                  a[6], a[7], a[8], a[9], a[10], a[11]);
        err = GetLastError();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return finish(out, STATUS_RPM_FAIL, 0);
    }
    write_u32(out + 4, result);
    write_u32(out + 8, err);
    return finish(out, STATUS_OK, 8);
}

// SUB_DUMP_VA_TO_FILE: snapshot `len` bytes from `va` straight into a
// flash file at `path`. All on-device, used for capturing live ring
// buffers (e.g. the AVP audio output at 0xd0cfc000) faster than the
// daemon's per-chunk TCP read can manage.
//
// Each chunk: PL1 kernel-memcpy from `va+off` into a local 32 KB scratch
// (fast - single instruction-bandwidth-bound copy), then WriteFile that
// scratch to flash. WriteFile speed dominates; expected ~30 ms/chunk on
// exFAT. Full 512 KB capture completes well inside the ~1.5 s AVP
// ring-buffer wrap window.
//
// Args (12 + path bytes):
//   +0  va u32
//   +4  len u32
//   +8  path_byte_len u32  (UTF-16 LE, bytes - not chars)
//   +12 path bytes
// Returns (4 bytes): total_written u32. Status carries the failing
// GetLastError in low 16 bits on partial success.
static int handle_dump_va_to_file(const unsigned char* arg, int arg_len,
                                    unsigned char* out, int out_max) {
    if (arg_len < 12) return finish(out, STATUS_BAD_ARG_LEN, 0);
    if (out_max < 8) return finish(out, STATUS_OUT_TOO_SMALL, 0);
    DWORD va = read_u32(arg + 0);
    DWORD len = read_u32(arg + 4);
    DWORD path_byte_len = read_u32(arg + 8);
    if (arg_len < (int)(12 + path_byte_len)) return finish(out, STATUS_BAD_ARG_LEN, 0);
    if (path_byte_len > 512) return finish(out, STATUS_BAD_ARG_LEN, 0);

    wchar_t path[260];
    int n_wchars = (int)(path_byte_len / 2);
    if (n_wchars > 259) n_wchars = 259;
    for (int i = 0; i < n_wchars; i++) {
        path[i] = (wchar_t)(arg[12 + i*2] | (arg[12 + i*2 + 1] << 8));
    }
    path[n_wchars] = 0;

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        write_u32(out + 4, 0);
        return finish(out, STATUS_RPM_FAIL | (err & 0xFFFF), 4);
    }

    static unsigned char chunk[32768];
    DWORD total = 0;
    DWORD err_out = 0;

    __try {
        DWORD off = 0;
        while (off < len) {
            DWORD this_chunk = (len - off) > sizeof(chunk) ? sizeof(chunk) : (len - off);
            // PL1 kernel-memcpy (KMEMCPY_FN) reading kernel `va+off` into the
            // local chunk. Direct kcall, not kerncore_kmemcpy, which is the
            // opposite direction (user buf → kernel va).
            kerncore_kcall(KMEMCPY_FN, (DWORD)chunk, va + off, this_chunk, 0, 0, 0);

            DWORD written = 0;
            BOOL ok = WriteFile(h, chunk, this_chunk, &written, NULL);
            if (!ok || written != this_chunk) {
                err_out = GetLastError();
                if (err_out == 0) err_out = 1;
                break;
            }
            total += written;
            off += this_chunk;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (err_out == 0) err_out = GetLastError();
        if (err_out == 0) err_out = 0xFFFF;
    }

    CloseHandle(h);

    write_u32(out + 4, total);
    return finish(out, err_out ? (STATUS_RPM_FAIL | (err_out & 0xFFFF)) : STATUS_OK, 4);
}

// SUB_PULL_FILE: read `length` bytes from `path` starting at `offset`,
// return them inline. Caller chunks for large files (output cap ~8 KB).
//
// Args:
//   +0  offset u32
//   +4  length u32  (caller-requested; clamped to out_max - 4)
//   +8  path_byte_len u32
//   +12 path bytes (UTF-16 LE)
// Returns: file bytes.
static int handle_pull_file(const unsigned char* arg, int arg_len,
                              unsigned char* out, int out_max) {
    if (arg_len < 12) return finish(out, STATUS_BAD_ARG_LEN, 0);
    DWORD offset = read_u32(arg + 0);
    DWORD length = read_u32(arg + 4);
    DWORD path_byte_len = read_u32(arg + 8);
    if (arg_len < (int)(12 + path_byte_len)) return finish(out, STATUS_BAD_ARG_LEN, 0);
    if (path_byte_len > 512) return finish(out, STATUS_BAD_ARG_LEN, 0);
    if (length > (DWORD)(out_max - 4)) length = (DWORD)(out_max - 4);

    wchar_t path[260];
    int n_wchars = (int)(path_byte_len / 2);
    if (n_wchars > 259) n_wchars = 259;
    for (int i = 0; i < n_wchars; i++) {
        path[i] = (wchar_t)(arg[12 + i*2] | (arg[12 + i*2 + 1] << 8));
    }
    path[n_wchars] = 0;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return finish(out, STATUS_RPM_FAIL | (GetLastError() & 0xFFFF), 0);
    }
    DWORD pos = SetFilePointer(h, offset, NULL, FILE_BEGIN);
    if (pos == INVALID_SET_FILE_POINTER) {
        CloseHandle(h);
        return finish(out, STATUS_RPM_FAIL | (GetLastError() & 0xFFFF), 0);
    }
    DWORD got = 0;
    BOOL ok = ReadFile(h, out + 4, length, &got, NULL);
    CloseHandle(h);
    if (!ok) {
        return finish(out, STATUS_RPM_FAIL | (GetLastError() & 0xFFFF), 0);
    }
    return finish(out, STATUS_OK, got);
}

static int handle_kwrite_bytes(const unsigned char* arg, int arg_len,
                                unsigned char* out, int out_max) {
    if (arg_len < 4) return finish(out, STATUS_BAD_ARG_LEN, 0);
    u32 va  = read_u32(arg + 0);
    int data_len = arg_len - 4;
    if (data_len < 0) data_len = 0;

    __try {
        for (int i = 0; i < data_len; i++) {
            kerncore_kwriteb(va + i, arg[4 + i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return finish(out, STATUS_WPM_FAIL, 0);
    }
    write_u32(out + 4, (u32)data_len);
    return finish(out, STATUS_OK, 4);
}

// ---------- entry ----------

#include "repl_primitives.h"

int repl_dispatch(const void* arg, int arg_len, void* out_v, int out_max, int* out_used) {
    if (arg_len < 4 || out_max < 8) {
        if (out_used) *out_used = 0;
        return -1;
    }
    const unsigned char* a = (const unsigned char*)arg;
    unsigned char* out = (unsigned char*)out_v;
    u32 sub = read_u32(a + 0);
    const unsigned char* sub_args = a + 4;
    int sub_args_len = arg_len - 4;

    int used = 0;
    switch (sub) {
        case SUB_READ_PROC_MEM:
            used = handle_read_proc_mem(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_WRITE_PROC_MEM:
            used = handle_write_proc_mem(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_VIRT_QUERY_EX:
            used = handle_virt_query_ex(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_LIST_PROCESSES:
            used = handle_list_processes(out, out_max);
            break;
        case SUB_LIST_MODULES:
            used = handle_list_modules(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_READ_MEM_LOCAL:
            used = handle_read_mem_local(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_KREAD_BYTES:
            used = handle_kread_bytes(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_KWRITE_BYTES:
            used = handle_kwrite_bytes(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_MAP_PTR_TO_PROC:
            used = handle_map_ptr_to_proc(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_READ_PROC_KMEM:
            used = handle_read_proc_kmem(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_KCALL:
            used = handle_kcall(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_USER_CALL:
            used = handle_user_call(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_DUMP_VA_TO_FILE:
            used = handle_dump_va_to_file(sub_args, sub_args_len, out, out_max);
            break;
        case SUB_PULL_FILE:
            used = handle_pull_file(sub_args, sub_args_len, out, out_max);
            break;
        default:
            used = finish(out, STATUS_UNKNOWN_OPCODE, 0);
            break;
    }
    if (out_used) *out_used = used;
    return 0;
}

