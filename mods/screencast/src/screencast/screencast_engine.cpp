#include <windows.h>
#include <string.h>
#include "screencast_engine.h"

extern "C" {
#include "kerncore.h"
}

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* ── device-validated constants (notes/re-2026-05-28-screencap) ─────────── */
#define FB_PTR_FIELD  0xC07EFFE8u   /* libnvddk_disp .data: live front-buffer PA */
#define CARVEOUT_PA   0x06000000u
#define CARVEOUT_KVA  0xD0970000u
#define CARVEOUT_SIZE 0x02000000u
#define KMEMCPY_F     0x80072318u   /* CE6 u32-stride kernel memcpy(dst,src,len) */

/* ── kernel reads via kerncore (KMEMCPY_F: dst <- src) ──────────────────── */
/* Each KMEMCPY_F is a PL1 kernel transition that may briefly hold a kernel
 * memory lock the compositor also needs for display-surface alloc/free. Copying
 * the whole 522 KB in one call holds that lock long enough to deadlock the UI
 * during navigation, so we chunk small and Sleep(0) between chunks: every
 * kernel call is short and releases the lock, letting the compositor interleave. */
#define READ_CHUNK 0x2000u
static void kread_bulk(void* dst, u32 src_va, u32 len) {
    u32 off = 0;
    while (off < len) {
        u32 n = (len - off > READ_CHUNK) ? READ_CHUNK : (len - off);
        kerncore_kcall(KMEMCPY_F, (u32)dst + off, src_va + off, n, 0, 0, 0);
        off += n;
        Sleep(0);
    }
}
static u32 kread_u32(u32 va) { u32 v = 0; kerncore_kcall(KMEMCPY_F, (u32)&v, va, 4, 0, 0, 0); return v; }

/* Bulk write to a kernel VA via KMEMCPY_F (one kernel transition). */
static void kwrite_bulk(u32 dst_va, const void* src, u32 len) {
    kerncore_kcall(KMEMCPY_F, dst_va, (u32)src, len, 0, 0, 0);
}

/* ── capture ────────────────────────────────────────────────────────────── */
int sc_capture(unsigned char* argb) {
    u32 pa = kread_u32(FB_PTR_FIELD);
    if (pa < CARVEOUT_PA || pa >= CARVEOUT_PA + CARVEOUT_SIZE) return 0;
    kread_bulk(argb, pa - CARVEOUT_PA + CARVEOUT_KVA, SC_FB_BYTES);
    return 1;
}

void sc_argb_to_rgb565(const unsigned char* argb, u16* rgb565) {
    for (u32 i = 0; i < SC_NPIX; i++) {
        u8 b = argb[i * 4 + 0], g = argb[i * 4 + 1], r = argb[i * 4 + 2];
        rgb565[i] = (u16)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    }
}

/* ── delta encoder: runs of [u16 skip][u16 copy][copy * u16] ─────────────── */
/* Unchanged pixels (cur==prev) are skipped; the host keeps them from its prev
 * frame. Returns encoded length, or -1 if it would exceed out_cap. */
int sc_encode_delta(const u16* cur, const u16* prev, u8* out, int out_cap) {
    int p = 0, o = 0;
    while (p < (int)SC_NPIX) {
        int skip = 0;
        while (p + skip < (int)SC_NPIX && cur[p + skip] == prev[p + skip] && skip < 65535) skip++;
        p += skip;
        int copy = 0;
        while (p + copy < (int)SC_NPIX && cur[p + copy] != prev[p + copy] && copy < 65535) copy++;
        if (o + 4 + copy * 2 > out_cap) return -1;
        out[o++] = (u8)(skip & 0xff); out[o++] = (u8)(skip >> 8);
        out[o++] = (u8)(copy & 0xff); out[o++] = (u8)(copy >> 8);
        for (int i = 0; i < copy; i++) {
            out[o++] = (u8)(cur[p + i] & 0xff);
            out[o++] = (u8)(cur[p + i] >> 8);
        }
        p += copy;
        if (skip == 0 && copy == 0) break;
    }
    return o;
}

/* ── touch injection via the native ZAM input ring (device-validated;
 * notes/re-2026-05-29-zam-input-inject §DV.10). A tap is a 0x40-byte type-3
 * record written into servicesd's ci3 (touch) channel ring; signalling
 * gemstone's input event makes gemstone's own input thread dequeue + dispatch
 * it, which wakes the render loop natively.
 *
 * The ci3 struct and its ring live in servicesd's address space (the 0x41xxxxxx
 * region is per-process, unreachable via the kerncore gadget), so they are
 * read/written with RPM/WPM on a servicesd handle. The event signal runs
 * EventModify in gemstone's context via helper_v4. */
#define ZAM_CI3        0x418a7bf0u           /* servicesd: ci3 touch-channel struct */
#define ZAM_RING_HEAD  (ZAM_CI3 + 0x20u)     /* head/tail; dequeue resets to base when drained */
#define ZAM_RING_BASE  (ZAM_CI3 + 0x1cu)     /* ring base VA */
#define GEM_INPUT_EVT  0x009e0803u           /* gemstone input event (SET wakes its dispatch wait) */
#define EVENTMODIFY    0x4032ff60u           /* coredll!EventModify (shared base across processes) */
#define SIG_STUB_KVA   0x800157c0u           /* kernel scratch, clear of helpers */
#define NK_PROC_LIST   0x80bee010u

static DWORD  g_gem_proc = 0, g_gem_pid = 0; /* gemstone proc-struct VA + PID (signal target) */
static DWORD  g_svc_proc = 0, g_svc_pid = 0; /* servicesd proc-struct VA + PID (ring host) */
static HANDLE g_svc_h    = NULL;             /* servicesd handle for RPM/WPM */
static DWORD  g_ring_base = 0;               /* cached [ci3+0x1c] */
static int    g_held = 0;                    /* a finger is down (between down and up) */
static int    g_hold_x = 0, g_hold_y = 0;    /* its last position (phase-2 hold stream) */
static DWORD  g_hold_ms = 140;               /* phase-2 hold-stream interval once settled */
static DWORD  g_settle_ms = 400;             /* stationary time before the hold stream begins */
static DWORD  g_last_move_tick = 0;          /* GetTickCount of the last down/move */

int sc_held(void) { return g_held; }

void sc_log(const char* msg) {
    HANDLE f = CreateFileW(L"\\flash2\\automation\\screencast.log", GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    DWORD w; WriteFile(f, msg, (DWORD)strlen(msg), &w, NULL);
    CloseHandle(f);
}

/* Read a kernel-resident wide string, lowercased into an ASCII buffer. */
static void kread_name(DWORD wstr_va, char* out, int cap) {
    int i = 0;
    for (; i < cap - 1 && wstr_va; i++) {
        u16 ch = (u16)(kerncore_kreadu32(wstr_va + i * 2) & 0xffff);
        if (!ch) break;
        out[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : (char)ch;
    }
    out[i] = 0;
}

/* Walk the NK process list (next@+0, id@+0xC, name_ptr@+0x20) for a process
 * whose name contains `want`; return its proc-struct VA and (via out_pid) PID. */
static DWORD find_proc(const char* want, DWORD* out_pid) {
    DWORD head = kerncore_kreadu32(NK_PROC_LIST), cur = head;
    for (int i = 0; i < 64 && cur; i++) {
        char nm[24];
        kread_name(kerncore_kreadu32(cur + 0x20), nm, sizeof(nm));
        if (strstr(nm, want)) { *out_pid = kerncore_kreadu32(cur + 0x0c); return cur; }
        DWORD nxt = kerncore_kreadu32(cur + 0x00);
        if (nxt == head) break;
        cur = nxt;
    }
    *out_pid = 0; return 0;
}

/* Cache gemstone (signal target) and servicesd (ring host) proc-structs/PIDs, a
 * servicesd handle, and the ring base. Re-resolved only when a cached PID stops
 * matching (process restart), never per tap. */
static void resolve_targets(void) {
    g_gem_proc = find_proc("gemstone", &g_gem_pid);
    DWORD svc = find_proc("servicesd", &g_svc_pid);
    if (svc != g_svc_proc || !g_svc_h) {
        if (g_svc_h) { CloseHandle(g_svc_h); g_svc_h = NULL; }
        g_svc_proc = svc; g_ring_base = 0;
        if (g_svc_pid) g_svc_h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_svc_pid);
    }
    if (g_svc_h && !g_ring_base) {
        DWORD n = 0; ReadProcessMemory(g_svc_h, (void*)ZAM_RING_BASE, &g_ring_base, 4, &n);
    }
}

/* Cheap per-tap staleness check: one kernel u32 read of each cached PID. */
static int targets_alive(void) {
    return g_gem_proc && kerncore_kreadu32(g_gem_proc + 0x0c) == g_gem_pid
        && g_svc_proc && kerncore_kreadu32(g_svc_proc + 0x0c) == g_svc_pid
        && g_svc_h && g_ring_base;
}

/* EventModify(GEM_INPUT_EVT, EVENT_SET), planted once in kernel scratch and run
 * in gemstone's context via helper_v4 (so the SET resolves gemstone's handle). */
static void plant_sig_stub(void) {
    DWORD stub[9] = {
        0xe52de004u,  /* str lr, [sp, #-4]!            */
        0xe59f000cu,  /* ldr r0, [pc, #0xc] -> EVT     */
        0xe59f100cu,  /* ldr r1, [pc, #0xc] -> SET     */
        0xe59fc00cu,  /* ldr ip, [pc, #0xc] -> EVENTMODIFY */
        0xe12fff3cu,  /* blx ip                        */
        0xe49df004u,  /* ldr pc, [sp], #4              */
        GEM_INPUT_EVT, EVENT_SET, EVENTMODIFY,
    };
    kwrite_bulk(SIG_STUB_KVA, stub, sizeof(stub));
}

/* Write one 0x40 type-3 touch record at the ci3 ring head and signal gemstone.
 * phase is the ci3 sub-code gemstone dispatches on: 1 = down/up (WM_LBUTTON*,
 * with `contact` in [+0x14]: 1=down, 0=up), 3 = move (WM_MOUSEMOVE), 2 = hold.
 * The cross-process write can at worst race servicesd's accelerometer producer
 * for one ring slot (the head pointer is single-u32-atomic), dropping one accel
 * sample, never corruption. */
static void zam_emit(int x, int y, DWORD contact, DWORD phase) {
    DWORD head = 0, n = 0;
    if (!ReadProcessMemory(g_svc_h, (void*)ZAM_RING_HEAD, &head, 4, &n) || n != 4) return;
    DWORD rec[16] = { 0 };
    rec[0] = 3; rec[1] = 1; rec[2] = phase;
    rec[3] = (DWORD)(x & 0xffff); rec[4] = (DWORD)(y & 0xffff);
    rec[5] = contact;
    WriteProcessMemory(g_svc_h, (void*)head, rec, sizeof(rec), &n);
    DWORD next = head + 0x40;
    WriteProcessMemory(g_svc_h, (void*)ZAM_RING_HEAD, &next, 4, &n);
    kerncore_kcall(KERNCORE_HELPER_V4, g_gem_proc, SIG_STUB_KVA, 0, 0, 0, 0);
}

/* Spin until gemstone's input thread drains the ring (head back to base) so a
 * follow-up record isn't written before the prior one is read. */
static void zam_wait_drained(int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 8) {
        DWORD h = 0, n = 0;
        if (ReadProcessMemory(g_svc_h, (void*)ZAM_RING_HEAD, &h, 4, &n) && n == 4 && h == g_ring_base)
            return;
        Sleep(8);
    }
}

int sc_engine_ready(void) { return kerncore_is_ready(); }

void sc_engine_init(void) {
    kerncore_ensure_helpers();
    resolve_targets();
    plant_sig_stub();
}

void sc_inject(u8 action, u32 dx, u32 dy) {
    if (!targets_alive()) resolve_targets();   /* cheap PID check; full walk only on restart */
    if (!g_gem_proc || !g_svc_h || !g_ring_base) { sc_log("inject: targets not found\n"); return; }
    kerncore_ensure_helpers();
    int x = (int)dx, y = (int)dy;
    switch (action) {
        case 1: g_held = 1; g_hold_x = x; g_hold_y = y; g_last_move_tick = GetTickCount(); zam_emit(x, y, 1, 1); break;  /* down */
        case 2: g_hold_x = x; g_hold_y = y; g_last_move_tick = GetTickCount(); zam_emit(x, y, 0, 3); break;             /* move (resets settle) */
        case 3: g_held = 0; zam_emit(x, y, 0, 1); break;                                                                /* up */
        case 0: g_held = 0; zam_emit(x, y, 1, 1); zam_wait_drained(200); zam_emit(x, y, 0, 1); break;                   /* tap: down, drain, up */
    }
}

/* While a finger is held still, the panel keeps streaming phase-2 stationary
 * samples; gemstone's long-press detection needs that live stream (a bare
 * LBUTTONDOWN held without samples never registers a hold). The settle gate
 * suppresses the stream until the finger has been stationary for g_settle_ms,
 * and every move resets that timer, so a drag never accumulates a false hold. */
void sc_feed_hold(void) {
    if (!g_held || !g_gem_proc || !g_svc_h) return;
    if (GetTickCount() - g_last_move_tick < g_settle_ms) return;
    kerncore_ensure_helpers();
    zam_emit(g_hold_x, g_hold_y, 1, 2);
}
