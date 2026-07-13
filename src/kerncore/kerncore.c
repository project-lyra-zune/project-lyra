#include "kerncore.h"

#include <string.h>

/* GetFSHeapInfo signature: (DWORD r0, DWORD r1, DWORD r2) → DWORD r0.
   After nativeapp's hax() patches the kernel function pointer at
   0x80060da0, every call from any process hits the gadget at
   0x80015020+ which dispatches on the magic in r2. */
typedef DWORD (WINAPI *PfnGhi)(DWORD, DWORD, DWORD);

static PfnGhi resolve_ghi(void) {
    static PfnGhi g_ghi = NULL;
    if (g_ghi == NULL) {
        HMODULE coredll = GetModuleHandleW(L"coredll.dll");
        if (coredll)
            g_ghi = (PfnGhi)GetProcAddress(coredll, L"GetFSHeapInfo");
    }
    return g_ghi;
}

/* ── global serialization ────────────────────────────────────────────────
   kerncore is a process-global singleton: one fixed KERNCORE_KSCRATCH,
   one planted helper set, one GetFSHeapInfo gadget. zpod_auto_worker
   runs concurrently with the TCP request handler, and the modkit
   end-state has many mods. Every public entry below acquires this one
   recursive lock and holds it for the whole operation, so a multi-step
   patch_code (PT-flip → stage KSCRATCH → HELPER_V7 → restore) is atomic
   against any other kerncore user.

   The lock MUST be cross-process: KSCRATCH, the L2 PT-flip window, and the
   planted helpers are global KERNEL state shared by every process that links
   kerncore. gemstone and servicesd both run Phase-2 patching at boot
   concurrently; a process-local CRITICAL_SECTION serialised only within one
   process, so two processes' patch_code could interleave their KSCRATCH
   staging / PT-flip and corrupt the write, surfacing as an early-boot
   gemstone crash. A NAMED mutex serialises across processes, which is what
   "atomic against any other kerncore user" requires. CE mutexes are recursive
   for the owning thread, so the kreadu32→kreadb / patch_code→kwriteb nesting
   through the public API self-recurses without deadlock. One-time lazy init:
   boot is single-threaded; the CAS guard makes it correct even if the first
   calls ever race. */
#define KERNCORE_LOCK_NAME  L"zune-kerncore-lock"
static HANDLE           g_kc_mutex = NULL;
static volatile LONG    g_kc_state = 0;   /* 0 uninit, 1 init, 2 ready */

static void kc_lock(void) {
    if (InterlockedCompareExchange(&g_kc_state, 1, 0) == 0) {
        g_kc_mutex = CreateMutexW(NULL, FALSE, KERNCORE_LOCK_NAME);
        InterlockedExchange(&g_kc_state, 2);
    } else {
        while (g_kc_state != 2) Sleep(0);
    }
    if (g_kc_mutex) WaitForSingleObject(g_kc_mutex, INFINITE);
}

static void kc_unlock(void) {
    if (g_kc_mutex) ReleaseMutex(g_kc_mutex);
}

/* ── basic primitives (static cores; locked wrappers at EOF) ─────────────── */

/* CE 6 kernel-internal helpers (Pavo v4.5), reachable as ARM `bl`.
   KMEMCPY_F is used both by kc_kmemcpy_impl (one-shot u32-stride
   copy, hardware-atomic per dword) and by HELPER_V3/V7 below for
   their cross-VM work. */
#define KSWITCH_VM  0x8006C6E8u
#define KACQUIRE_B  0x8006C78Cu
#define KMEMCPY_F   0x80072318u

static unsigned char kc_kreadb_impl(DWORD va) {
    PfnGhi ghi = resolve_ghi();
    if (!ghi) return 0;
    return (unsigned char)(ghi(va, 0, 0x1338) & 0xFF);
}

static void kc_kwriteb_impl(DWORD va, unsigned char val) {
    PfnGhi ghi = resolve_ghi();
    if (!ghi) return;
    ghi(va, (DWORD)val, 0x1337);
}

static DWORD kc_kreadu32_impl(DWORD va) {
    DWORD d = kerncore_kreadb(va);
    DWORD c = kerncore_kreadb(va + 1);
    DWORD b = kerncore_kreadb(va + 2);
    DWORD a = kerncore_kreadb(va + 3);
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* CE 6 KMEMCPY_F (0x80072318): u32-stride memcpy. Each store is a
   single hardware-atomic STR opcode. One kernel transition for the
   whole copy (vs ~`len` transitions for a byte-by-byte ghi loop).
   Critical when dst is being read concurrently by hardware DMA: a
   byte-by-byte writer lets the reader observe partially-written
   dwords (frankenstein addresses, torn buf_ptrs) and DMA from
   invalid PAs. The KMEMCPY_F path eliminates that race by
   construction: every dword in dst is either old or new, never
   torn. */
static void kc_kmemcpy_impl(DWORD va, const void* buf, size_t len) {
    PfnGhi ghi = resolve_ghi();
    DWORD args[7];
    if (!ghi) return;
    args[0] = KMEMCPY_F;
    args[1] = va;                  /* dst */
    args[2] = (DWORD)buf;          /* src */
    args[3] = (DWORD)len;
    args[4] = 0; args[5] = 0; args[6] = 0;
    ghi((DWORD)args, 0, 0x1339);
}

static DWORD kc_kcall_impl(DWORD fn, DWORD a0, DWORD a1, DWORD a2, DWORD a3,
                      DWORD a4, DWORD a5) {
    PfnGhi ghi = resolve_ghi();
    DWORD args[7];
    if (!ghi) return 0;
    args[0] = fn;
    args[1] = a0; args[2] = a1; args[3] = a2;
    args[4] = a3; args[5] = a4; args[6] = a5;
    return ghi((DWORD)args, 0, 0x1339);
}

static int kc_is_ready_impl(void) {
    DWORD head;
    __try {
        head = kerncore_kreadu32(0x80BEE010u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    /* NK process list head is a kernel-VA pointer, so it must be in
       0x80000000-0xFFFFFFFF range. Anything else means our gadget
       isn't returning real kernel data. */
    return (head >= 0x80000000u) ? 1 : 0;
}

/* ── proc-struct lookup ──────────────────────────────────────────────── */

static DWORD kc_find_proc_struct_impl(DWORD pid) {
    DWORD cur;
    int safety;
    cur = kerncore_kreadu32(0x80BEE010u);
    if (cur < 0x80000000u) return 0;
    for (safety = 64; safety > 0; safety--) {
        DWORD nxt = kerncore_kreadu32(cur + 0x00);
        /* NK proc-struct fields:
             +0x00 next, +0x04 last, +0x0C id (pid),
             +0x10 thread_next, +0x14 thread_last, +0x18 base,
             +0x20 name_ptr */
        DWORD this_pid = kerncore_kreadu32(cur + 0x0C);
        if (this_pid == pid) return cur;
        if (nxt < 0x80000000u) return 0;
        cur = nxt;
    }
    return 0;
}

/* ── PT-flip code patching ───────────────────────────────────────────── */

static void kerncore_kwriteu32(DWORD va, DWORD value) {
    kerncore_kwriteb(va + 0, (unsigned char)(value & 0xFF));
    kerncore_kwriteb(va + 1, (unsigned char)((value >> 8) & 0xFF));
    kerncore_kwriteb(va + 2, (unsigned char)((value >> 16) & 0xFF));
    kerncore_kwriteb(va + 3, (unsigned char)((value >> 24) & 0xFF));
}

static int kc_patch_code_impl(DWORD target_proc, DWORD target_va,
                        const void* bytes, int len) {
    DWORD proc_l1, e_l1, l2_kva, l2_entry_kva, original_l2;
    DWORD l1_idx, l2_idx;
    int i;
    const unsigned char* p = (const unsigned char*)bytes;

    if (len <= 0 || len > 64) return -1;

    /* Walk L1 PT */
    proc_l1 = kerncore_kreadu32(target_proc + 0x2c);
    if (proc_l1 < 0x80000000u) return -1;
    l1_idx = target_va >> 20;
    e_l1 = kerncore_kreadu32(proc_l1 + l1_idx * 4);
    if ((e_l1 & 3) != 1) return -1;          /* not a coarse PT */
    l2_kva = (e_l1 & 0xFFFFFC00u) + 0x80000000u;
    l2_idx = (target_va - (l1_idx << 20)) >> 12;
    l2_entry_kva = l2_kva + l2_idx * 4;

    /* Save original L2 entry */
    original_l2 = kerncore_kreadu32(l2_entry_kva);

    /* Clear AP[2] (bit 9) → page PL1-writable */
    kerncore_kwriteu32(l2_entry_kva, original_l2 & ~0x200u);
    kerncore_kcall(KERNCORE_TLB_FLUSH, 0, 0, 0, 0, 0, 0);

    /* Stage bytes at KSCRATCH, then call HELPER_V7 which does the
       cross-process memcpy + DCCMVAU/ICIMVAU/barriers in the TARGET
       process's TTBR context. The I-cache invalidation is critical:
       without it, the CPU keeps executing the OLD instructions cached
       at target_va even though the underlying memory has the new
       bytes. */
    for (i = 0; i < len; i++) {
        kerncore_kwriteb(KERNCORE_KSCRATCH + (DWORD)i, p[i]);
    }
    kerncore_kcall(KERNCORE_HELPER_V7,
                    target_proc, KERNCORE_KSCRATCH, target_va,
                    (DWORD)len, 0, 0);

    /* Restore L2 entry */
    kerncore_kwriteu32(l2_entry_kva, original_l2);
    kerncore_kcall(KERNCORE_TLB_FLUSH, 0, 0, 0, 0, 0, 0);

    return 0;
}

/* ── validate-and-replant ────────────────────────────────────────────── */

/* Expected first instruction (prologue) of each helper. */
#define KERNCORE_V3_FIRST   0xE92D41F0u  /* push {r4-r8, lr} */
#define KERNCORE_V4_FIRST   0xE92D40F0u  /* push {r4-r7, lr} */
#define KERNCORE_TLB_FIRST  0xEE080F17u  /* mcr p15, 0, r0, c8, c7, 0 (TLBIALL) */
#define KERNCORE_V7_FIRST   0xE92D43F0u  /* push {r4-r9, lr} */

static int kc_ensure_helpers_impl(void) {
    DWORD v3, v4, tlb, v7;
    int valid;
    if (!kerncore_is_ready()) return 0;
    v3  = kerncore_kreadu32(KERNCORE_HELPER_V3);
    v4  = kerncore_kreadu32(KERNCORE_HELPER_V4);
    tlb = kerncore_kreadu32(KERNCORE_TLB_FLUSH);
    v7  = kerncore_kreadu32(KERNCORE_HELPER_V7);
    valid = (v3 == KERNCORE_V3_FIRST) && (v4 == KERNCORE_V4_FIRST)
         && (tlb == KERNCORE_TLB_FIRST) && (v7 == KERNCORE_V7_FIRST);
    if (valid) return 1;
    /* Replant and re-verify. */
    kerncore_plant_helpers();
    v3  = kerncore_kreadu32(KERNCORE_HELPER_V3);
    v7  = kerncore_kreadu32(KERNCORE_HELPER_V7);
    return (v3 == KERNCORE_V3_FIRST && v7 == KERNCORE_V7_FIRST) ? 1 : 0;
}

/* ── helper-shellcode planting ───────────────────────────────────────── */

static DWORD bl_inst(DWORD from_va, DWORD target_va) {
    int diff = (int)(target_va - (from_va + 8));
    return 0xEB000000u | (((DWORD)(diff >> 2)) & 0xFFFFFFu);
}

static void plant_words(DWORD base, const DWORD* words, int n) {
    int i;
    for (i = 0; i < n; i++) {
        DWORD w = words[i];
        unsigned char bytes[4];
        bytes[0] = (unsigned char)(w & 0xFF);
        bytes[1] = (unsigned char)((w >> 8) & 0xFF);
        bytes[2] = (unsigned char)((w >> 16) & 0xFF);
        bytes[3] = (unsigned char)((w >> 24) & 0xFF);
        kerncore_kmemcpy(base + (DWORD)i * 4u, bytes, 4);
    }
}

static void kc_plant_helpers_impl(void) {
    /* helper_v3: memcpy with TTBR swap (23 words) */
    {
        DWORD v3[23];
        int k = 0;
        v3[k++] = 0xE92D41F0u;
        v3[k++] = 0xE1A04001u;
        v3[k++] = 0xE1A05002u;
        v3[k++] = 0xE1A06003u;
        v3[k++] = 0xE1A08000u;
        v3[k++] = bl_inst(KERNCORE_HELPER_V3 + 0x14, KSWITCH_VM);
        v3[k++] = 0xE1A07000u;
        v3[k++] = 0xE1A00008u;
        v3[k++] = bl_inst(KERNCORE_HELPER_V3 + 0x20, KACQUIRE_B);
        v3[k++] = 0xE1A08000u;
        v3[k++] = 0xEE12CF10u;
        v3[k++] = 0xE585C010u;
        v3[k++] = 0xE1A00005u;
        v3[k++] = 0xE1A01004u;
        v3[k++] = 0xE1A02006u;
        v3[k++] = bl_inst(KERNCORE_HELPER_V3 + 0x3C, KMEMCPY_F);
        v3[k++] = 0xE1A05000u;
        v3[k++] = 0xE1A00008u;
        v3[k++] = bl_inst(KERNCORE_HELPER_V3 + 0x48, KACQUIRE_B);
        v3[k++] = 0xE1A00007u;
        v3[k++] = bl_inst(KERNCORE_HELPER_V3 + 0x50, KSWITCH_VM);
        v3[k++] = 0xE1A00005u;
        v3[k++] = 0xE8BD81F0u;
        plant_words(KERNCORE_HELPER_V3, v3, k);
    }

    /* helper_v4: bx-to-shellcode with TTBR swap (17 words) */
    {
        DWORD v4[17];
        int k = 0;
        v4[k++] = 0xE92D40F0u;
        v4[k++] = 0xE1A04001u;
        v4[k++] = 0xE1A05000u;
        v4[k++] = bl_inst(KERNCORE_HELPER_V4 + 0x0C, KSWITCH_VM);
        v4[k++] = 0xE1A06000u;
        v4[k++] = 0xE1A00005u;
        v4[k++] = bl_inst(KERNCORE_HELPER_V4 + 0x18, KACQUIRE_B);
        v4[k++] = 0xE1A07000u;
        v4[k++] = 0xE1A0E00Fu;
        v4[k++] = 0xE12FFF14u;
        v4[k++] = 0xE1A05000u;
        v4[k++] = 0xE1A00007u;
        v4[k++] = bl_inst(KERNCORE_HELPER_V4 + 0x30, KACQUIRE_B);
        v4[k++] = 0xE1A00006u;
        v4[k++] = bl_inst(KERNCORE_HELPER_V4 + 0x38, KSWITCH_VM);
        v4[k++] = 0xE1A00005u;
        v4[k++] = 0xE8BD80F0u;
        plant_words(KERNCORE_HELPER_V4, v4, k);
    }

    /* tlb_flush: TLBIALL + BPIALL + DSB + ISB + bx lr (5 words) */
    {
        DWORD tlb[5];
        tlb[0] = 0xEE080F17u;
        tlb[1] = 0xEE070FD5u;
        tlb[2] = 0xEE070F9Au;
        tlb[3] = 0xEE070F95u;
        tlb[4] = 0xE12FFF1Eu;
        plant_words(KERNCORE_TLB_FLUSH, tlb, 5);
    }

    /* helper_v7: cross-proc write + in-target cache mgmt (32 words) */
    {
        DWORD v7[32];
        int k = 0;
        v7[k++] = 0xE92D43F0u;
        v7[k++] = 0xE1A04001u;
        v7[k++] = 0xE1A05002u;
        v7[k++] = 0xE1A06003u;
        v7[k++] = 0xE1A09000u;
        v7[k++] = bl_inst(KERNCORE_HELPER_V7 + 0x14, KSWITCH_VM);
        v7[k++] = 0xE1A07000u;
        v7[k++] = 0xE1A00009u;
        v7[k++] = bl_inst(KERNCORE_HELPER_V7 + 0x20, KACQUIRE_B);
        v7[k++] = 0xE1A08000u;
        v7[k++] = 0xE1A00005u;
        v7[k++] = 0xE1A01004u;
        v7[k++] = 0xE1A02006u;
        v7[k++] = bl_inst(KERNCORE_HELPER_V7 + 0x34, KMEMCPY_F);
        v7[k++] = 0xE1A00005u;
        v7[k++] = 0xEE070F3Bu;
        v7[k++] = 0xE2800020u;
        v7[k++] = 0xEE070F3Bu;
        v7[k++] = 0xEE070F9Au;
        v7[k++] = 0xE1A00005u;
        v7[k++] = 0xEE070F35u;
        v7[k++] = 0xE2800020u;
        v7[k++] = 0xEE070F35u;
        v7[k++] = 0xEE070F9Au;
        v7[k++] = 0xEE070FD5u;
        v7[k++] = 0xEE070F95u;
        v7[k++] = 0xE1A00008u;
        v7[k++] = bl_inst(KERNCORE_HELPER_V7 + 0x6C, KACQUIRE_B);
        v7[k++] = 0xE1A00007u;
        v7[k++] = bl_inst(KERNCORE_HELPER_V7 + 0x74, KSWITCH_VM);
        v7[k++] = 0xE1A00006u;
        v7[k++] = 0xE8BD83F0u;
        plant_words(KERNCORE_HELPER_V7, v7, k);
    }
}

/* ── public API: serialized wrappers ─────────────────────────────────────
   Each holds the recursive lock for the whole op. Internal impl→impl
   calls go through these public names (header-declared), so nesting
   re-enters the same thread's lock and stays atomic vs other threads.

   The gadget faults on an unmapped/bad VA (routine while probing kernel
   addresses). __finally releases the lock during the unwind so the fault
   propagates to the caller's __except WITHOUT leaking the cross-process
   mutex; otherwise one bad read would wedge every kerncore user
   device-wide. */
unsigned char kerncore_kreadb(DWORD va) {
    unsigned char r = 0;
    kc_lock();
    __try { r = kc_kreadb_impl(va); } __finally { kc_unlock(); }
    return r;
}
void kerncore_kwriteb(DWORD va, unsigned char val) {
    kc_lock();
    __try { kc_kwriteb_impl(va, val); } __finally { kc_unlock(); }
}
DWORD kerncore_kreadu32(DWORD va) {
    DWORD r = 0;
    kc_lock();
    __try { r = kc_kreadu32_impl(va); } __finally { kc_unlock(); }
    return r;
}
void kerncore_kread(DWORD va, void* buf, DWORD len) {
    unsigned char* p = (unsigned char*)buf;
    DWORD i;
    kc_lock();
    __try {
        for (i = 0; i < len; i++) p[i] = kc_kreadb_impl(va + i);
    } __finally { kc_unlock(); }
}
void kerncore_kmemcpy(DWORD va, const void* buf, size_t len) {
    kc_lock();
    __try { kc_kmemcpy_impl(va, buf, len); } __finally { kc_unlock(); }
}
DWORD kerncore_kcall_le(DWORD fn, DWORD a0, DWORD a1, DWORD a2, DWORD a3,
                        DWORD a4, DWORD a5, DWORD* out_lasterr) {
    DWORD r = 0;
    kc_lock();
    __try {
        r = kc_kcall_impl(fn, a0, a1, a2, a3, a4, a5);
        if (out_lasterr) *out_lasterr = GetLastError();
    } __finally { kc_unlock(); }
    return r;
}
DWORD kerncore_kcall(DWORD fn, DWORD a0, DWORD a1, DWORD a2, DWORD a3,
                     DWORD a4, DWORD a5) {
    return kerncore_kcall_le(fn, a0, a1, a2, a3, a4, a5, NULL);
}
int kerncore_is_ready(void) {
    int r = 0;
    kc_lock();
    __try { r = kc_is_ready_impl(); } __finally { kc_unlock(); }
    return r;
}
DWORD kerncore_find_proc_struct(DWORD pid) {
    DWORD r = 0;
    kc_lock();
    __try { r = kc_find_proc_struct_impl(pid); } __finally { kc_unlock(); }
    return r;
}
int kerncore_patch_code(DWORD target_proc, DWORD target_va,
                        const void* bytes, int len) {
    int r = -1;
    kc_lock();
    __try {
        r = kc_patch_code_impl(target_proc, target_va, bytes, len);
    } __finally { kc_unlock(); }
    return r;
}
int kerncore_ensure_helpers(void) {
    int r = 0;
    kc_lock();
    __try { r = kc_ensure_helpers_impl(); } __finally { kc_unlock(); }
    return r;
}
void kerncore_plant_helpers(void) {
    kc_lock();
    __try { kc_plant_helpers_impl(); } __finally { kc_unlock(); }
}
