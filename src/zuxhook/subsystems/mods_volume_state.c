#include "mods_volume_state.h"
#include "mods_detour.h"
#include "mods_log.h"
#include "kerncore.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* The single ZAM volume-write choke point (zam_serv.dll, firmware v4.5). Both
   on-screen change paths funnel through it: relative (ZAM0:/0x204 method 0x13,
   the HUD Vol+/- touch elements) via sub_0x41893d34, and absolute (method 0x11)
   via sub_0x41899940. At entry r1 = new volume (0..30), r2 = max (30); the
   function then curve-maps and writes the codec (waveOut-backed) level. */
#define ZAM_VOLUME_WRITER_VA  0x41893c18u

#define VOLUME_STATE_LOG  L"\\flash2\\automation\\mods\\volume-state.log"

static VolumeStateBlock* g_block   = NULL;
static HANDLE            g_section = NULL;
static HANDLE            g_event   = NULL;
static volatile LONG     g_installed = 0;

static void VSlogf(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mods_vflashlog(VOLUME_STATE_LOG, fmt, ap);
    va_end(ap);
}

static VolumeStateBlock* vs_map(void) {
    HANDLE h;
    VolumeStateBlock* p;
    if (g_block) return g_block;
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                           sizeof(VolumeStateBlock), VOLUME_STATE_SECTION);
    if (!h) return NULL;
    p = (VolumeStateBlock*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) { CloseHandle(h); return NULL; }
    if (p->version == 0) { p->version = 1; p->max = 30; }  /* section is zero-filled on first create */
    g_section = h;
    g_block = p;
    return g_block;
}

/* Detour handler - runs on the ZAM thread at the writer's entry.
   r1 = new volume (0..30), r2 = max. Sole writer of the slot, so no lock: write
   the values, bump seq, then SetEvent; the SetEvent kernel transition is the
   release barrier that makes the writes visible before the consumer wakes. */
static void vs_on_volume(DWORD r0, DWORD r1, DWORD r2, DWORD r3) {
    VolumeStateBlock* b = g_block;
    int v, m;
    (void)r0; (void)r3;
    if (!b) return;
    /* the writer clamps signed [0,max]; a caller (current +/- delta) may pass a
       transiently out-of-range value, so clamp the same way before publishing. */
    v = (int)r1;
    m = r2 ? (int)r2 : (int)b->max;
    if (v < 0) v = 0;
    if (v > m) v = m;
    b->vol = (DWORD)v;
    b->max = (DWORD)m;
    b->seq++;
    if (g_event) SetEvent(g_event);
}

static DWORD WINAPI vs_install_thread(LPVOID param) {
    int tries;
    (void)param;
    for (tries = 0; tries < 240; tries++) {        /* up to ~120s */
        if (kerncore_is_ready() && kerncore_ensure_helpers()) {
            if (ModDetourInstallObserve(ZAM_VOLUME_WRITER_VA, vs_on_volume) == 0)
                VSlogf(L"writer detour installed @0x%08x", ZAM_VOLUME_WRITER_VA);
            else
                VSlogf(L"writer detour FAILED @0x%08x", ZAM_VOLUME_WRITER_VA);
            return 0;
        }
        Sleep(500);
    }
    VSlogf(L"kerncore not ready after wait - detour not installed");
    return 0;
}

static int vs_is_servicesd(void) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* base;
    if (len == 0 || len >= MAX_PATH) return 0;
    base = path + len;
    while (base > path && *(base - 1) != L'\\') base--;
    return _wcsicmp(base, L"servicesd.exe") == 0;
}

void VolumeStateInstall(void) {
    if (!vs_is_servicesd()) return;
    if (InterlockedExchange(&g_installed, 1) != 0) return;

    if (!vs_map()) { VSlogf(L"section map failed"); return; }
    g_event = CreateEventW(NULL, FALSE, FALSE, VOLUME_STATE_EVENT);  /* auto-reset */

    {
        HANDLE h = CreateThread(NULL, 0, vs_install_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    VSlogf(L"section+event up; writer detour deferred to kerncore-ready");
}
