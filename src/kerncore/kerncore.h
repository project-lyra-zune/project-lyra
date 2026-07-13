#ifndef KERNCORE_H
#define KERNCORE_H

#include <windows.h>

/* Shared kernel-mode primitives for nativeapp + zuxhook + plugins.

   Once nativeapp's hax() bootstrap has patched the GetFSHeapInfo
   entries in both the trusted-syscall table (0x80060da0) and the
   untrusted-syscall table (0x80060fa8), every subsequent
   GetFSHeapInfo call from ANY process hits the gadget at
   0x80015020 which runs in PL1. That means:

     * Privilege is a global kernel state change, not per-process.
     * Any DLL that can call coredll's GetFSHeapInfo can do kernel
       read/write/call.
     * nativeapp does the one-time bootstrap; once it has run,
       zuxhook (loaded into gemstone) and plugins loaded into
       device.exe have the same capability via these primitives
       directly, with no IPC required.

   The two tables exist because CE 6 routes coredll syscalls through
   different vtables for trusted vs untrusted callers; patching only
   the trusted entry leaves untrusted processes silently falling
   through to the real GetFSHeapInfo (which returns small heap-info
   values, making kreadu32 < 0x80000000 → kerncore_is_ready returns
   false, exactly what untrusted callers observe pre-bootstrap).

   Wire magics used by the trampoline at 0x80015020:
     0x1337  byte write   (r0=va, r1=byte)
     0x1338  byte read    (r0=va) → returns byte in low 8 bits
     0x1339  kcall        (r0=u32[7] {fn, a0..a5}) → returns fn's r0

   All primitives are safe to call multiple times. They are SEH-safe
   internally where it matters. */

#ifdef __cplusplus
extern "C" {
#endif

/* True (1) if nativeapp's bootstrap has run and our primitives can
   read/write kernel memory. Probes by reading the NK process-list
   head pointer at 0x80BEE010 and checking it's in kernel range. */
int kerncore_is_ready(void);

/* Byte primitives - single byte at a time via the gadget. */
unsigned char kerncore_kreadb(DWORD va);
void          kerncore_kwriteb(DWORD va, unsigned char val);

/* Helper: read a little-endian u32 (4 kreadb calls). */
DWORD         kerncore_kreadu32(DWORD va);

/* Bulk read: `len` bytes from va → buf, one lock held for the range.
   Faults on a bad VA propagate to the caller's __except (lock released). */
void          kerncore_kread(DWORD va, void* buf, DWORD len);

/* Bulk write (KMEMCPY_F, u32-stride atomic). `len` bytes from buf → va. */
void          kerncore_kmemcpy(DWORD va, const void* buf, size_t len);

/* Call kernel function at `fn` with up to 6 args (r0..r3 + 2 stack
   slots). Returns the function's r0. */
DWORD         kerncore_kcall(DWORD fn,
                              DWORD a0, DWORD a1, DWORD a2, DWORD a3,
                              DWORD a4, DWORD a5);

/* As kerncore_kcall, but also reports the fn's GetLastError via out_lasterr
   (captured inside the lock, before the gadget returns). For callers that
   surface the kernel call's last-error (e.g. the daemon's kcall opcode).
   out_lasterr may be NULL. */
DWORD         kerncore_kcall_le(DWORD fn,
                                 DWORD a0, DWORD a1, DWORD a2, DWORD a3,
                                 DWORD a4, DWORD a5, DWORD* out_lasterr);

/* Plant the cross-process helpers (HELPER_V3/V4/V7 + TLB_FLUSH) in
   kernel scratch at 0x80015220-0x800153FF. Called from nativeapp's
   hax() after the bootstrap is complete. Idempotent, safe to call
   multiple times; subsequent calls just overwrite the same bytes. */
void          kerncore_plant_helpers(void);

/* Validate-and-replant. Reads the expected first instruction of each
   helper. If any is wrong, calls kerncore_plant_helpers() to replant
   the full set. Returns 1 if helpers are valid (after possible replant),
   0 if replant failed.

   NK clobbers kernel scratch (0x80015200-0x800153FF) under load. Call
   this before any operation that depends on v3/v4/v7/tlb. Cheap: 4 byte
   reads in the common (already-valid) case. */
int           kerncore_ensure_helpers(void);

/* Walk NK process list to find the kernel proc-struct VA for a pid.
   Used for cross-process operations. Returns the proc-struct VA, or
   0 if not found. */
DWORD         kerncore_find_proc_struct(DWORD pid);

/* Patch RO code via L2 PT-flip (the "cydia-flip"):
     1. walk L1/L2 page tables for target_va in target_proc
     2. clear AP[2] (bit 9) on the L2 entry → page PL1-writable
     3. TLB flush (kcall TLB_FLUSH)
     4. byte-by-byte write via the gadget (now succeeds in PL1)
     5. restore L2 entry
     6. TLB flush
   target_proc is the kernel proc-struct VA (from
   kerncore_find_proc_struct). `len` is 1..64. Requires
   kerncore_plant_helpers() to have run so TLB_FLUSH is callable.
   Returns 0 on success, -1 on failure (with L2 entry restored). */
int           kerncore_patch_code(DWORD target_proc, DWORD target_va,
                                   const void* bytes, int len);

/* Helper VA constants (so callers don't need to redefine). */
#define KERNCORE_HELPER_V3    0x80015220u
#define KERNCORE_HELPER_V4    0x80015280u
#define KERNCORE_KSCRATCH     0x800152D0u
#define KERNCORE_TLB_FLUSH    0x80015360u
#define KERNCORE_HELPER_V7    0x80015380u

#ifdef __cplusplus
}
#endif

#endif
