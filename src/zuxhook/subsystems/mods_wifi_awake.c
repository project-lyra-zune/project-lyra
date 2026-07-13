#include "mods_wifi_awake.h"
#include "mods_state_block.h"
#include "mods_state_event.h"   /* ModStateEventPublish - wake the UI to re-tint */
#include "mods_log.h"
#include "kerncore.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* The device-proven on-battery keepalive patch set (servicesd .text;
   znet_serv.dll + zam_serv.dll). Applied as a unit while keepalive is
   demanded, restored to stock when it is not. */
typedef struct { DWORD va; DWORD stock; DWORD fix; const wchar_t* label; } WifiPatch;
static const WifiPatch WIFI_PATCHES[] = {
    { 0x41a7feb0u, 0x0a000001u, 0xe1a00000u, L"znet-timer" },  /* beq -> nop: kill the battery retry/re-eval timer arm */
    { 0x41a772bcu, 0x13a01000u, 0x13a01001u, L"znet-idle"  },  /* movne r1,#0 -> #1: treat POWER_STATE_IDLE as active */
    { 0x4189eb88u, 0xe3a03003u, 0xe3a03004u, L"zam-thresh" },  /* mov r3,#3 -> #4: ZAM selector-8 suspend gate D3 -> D4 */
};
#define WIFI_PATCH_N ((int)(sizeof(WIFI_PATCHES) / sizeof(WIFI_PATCHES[0])))

#define WIFI_AWAKE_EVENT   L"zune-wifi-awake-evt"   /* authority wake (Notify signals it) */
#define WIFI_AWAKE_LOG     L"\\flash2\\automation\\mods\\wifi-awake.log"
/* The effective-status output this authority publishes: keepalive demanded by
   any registered demand source. The wifiIcon tint binds here, so it reflects
   "Wi-Fi is being kept awake", not just the manual toggle. */
#define WIFI_AWAKE_STATUS_KEY  "status/wifi-power/keepalive"

static volatile LONG   g_authority_started = 0;  /* EnsureActive spawn guard */
static DWORD           g_proc = 0;               /* servicesd proc-struct VA (cached) */

/* Declarative keepalive demand sources: each entry is a ModStateBlock slot key
   whose active state (>0) demands wifi_awake. The authority ORs them every
   reconcile, a pull over stable state, replacing the imperative lease table. A
   mod declares a demand with `holds: ["wifi_awake"]` on a setting. */
#define WIFI_DEMAND_MAX 16
static char g_demand[WIFI_DEMAND_MAX][MOD_STATE_ID_LEN + 1];
static int  g_demand_n = 0;

/* ── logging (independent of mods_log's single global handle) ─────────── */

/* Prepends a monotonic tick stamp so keepalive timing is readable across the
   authority's reconcile passes. */
static void WAlogf(const wchar_t* fmt, ...) {
    wchar_t msg[480];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, sizeof(msg)/sizeof(msg[0]) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg)/sizeof(msg[0]) - 1] = 0;
    mods_flashlog(WIFI_AWAKE_LOG, L"[t=%lu] %s", (unsigned long)GetTickCount(), msg);
}

/* ── keepalive demand registry ────────────────────────────────────────── */

static void wa_signal_authority(void) {
    HANDLE e = CreateEventW(NULL, FALSE, FALSE, WIFI_AWAKE_EVENT);
    if (e) { SetEvent(e); CloseHandle(e); }
}

void WifiAwakeRegisterDemand(const char* state_key) {
    int i, j;
    if (!state_key || !state_key[0]) return;
    for (i = 0; i < g_demand_n; i++)
        if (strncmp(g_demand[i], state_key, MOD_STATE_ID_LEN) == 0) return;   /* already registered */
    if (g_demand_n >= WIFI_DEMAND_MAX) {
        WAlogf(L"demand registry full; dropped %S", state_key);
        return;
    }
    for (j = 0; j < MOD_STATE_ID_LEN && state_key[j]; j++) g_demand[g_demand_n][j] = state_key[j];
    g_demand[g_demand_n][j] = 0;
    g_demand_n++;
    WAlogf(L"demand registered: %S", state_key);
    wa_signal_authority();   /* re-evaluate now that a demand source exists */
}

/* Pull: keepalive is demanded iff any registered demand slot is active (>0). A
   setting's quick-toggle writing its slot wakes us via WifiAwake_Notify. */
static int wa_demanded(void) {
    int i;
    for (i = 0; i < g_demand_n; i++)
        if (ModStateGetState(g_demand[i]) > 0) return 1;
    return 0;
}

/* ── keepalive patch lever ────────────────────────────────────────────── */

static DWORD wa_proc(void) {
    if (g_proc == 0) g_proc = kerncore_find_proc_struct(GetCurrentProcessId());
    return g_proc;
}

/* Drive one site to `target` (its stock or fix word). Idempotent: no-op if
   already there. Refuses to write unless the current word is the opposite
   known value; a firmware shift would match neither, so leave it alone
   rather than corrupt an unknown instruction. Returns 0 on success /
   already-correct, -1 otherwise. */
static int wa_patch_site(const WifiPatch* p, DWORD target, DWORD proc) {
    DWORD cur;
    DWORD other = (target == p->fix) ? p->stock : p->fix;
    DWORD bytes[1];
    __try { cur = *(volatile DWORD*)p->va; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        WAlogf(L"patch %s @0x%08x: read faulted (module not mapped?)", p->label, p->va);
        return -1;
    }
    if (cur == target) return 0;
    if (cur != other) {
        WAlogf(L"patch %s @0x%08x: unexpected 0x%08x (expected 0x%08x) - skip",
               p->label, p->va, cur, other);
        return -1;
    }
    bytes[0] = target;
    if (kerncore_patch_code(proc, p->va, bytes, 4) != 0) {
        WAlogf(L"patch %s @0x%08x: kerncore_patch_code failed", p->label, p->va);
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), (void*)p->va, 4);
    __try { cur = *(volatile DWORD*)p->va; } __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
    if (cur != target) {
        WAlogf(L"patch %s @0x%08x: write didn't stick (0x%08x)", p->label, p->va, cur);
        return -1;
    }
    WAlogf(L"patch %s @0x%08x -> 0x%08x", p->label, p->va, target);
    return 0;
}

/* Apply (on=1) or restore (on=0) the whole keepalive patch set. Requires
   kerncore's PT-flip gadget. Returns 0 only if every site reached its target. */
static int wa_keepalive_set(int on) {
    DWORD proc;
    int i, ok = 0;
    if (!kerncore_is_ready() || !kerncore_ensure_helpers()) {
        WAlogf(L"keepalive %s: kerncore not ready - defer", on ? L"apply" : L"restore");
        return -1;
    }
    proc = wa_proc();
    if (proc == 0) { WAlogf(L"keepalive: no servicesd proc-struct"); return -1; }
    for (i = 0; i < WIFI_PATCH_N; i++) {
        DWORD target = on ? WIFI_PATCHES[i].fix : WIFI_PATCHES[i].stock;
        if (wa_patch_site(&WIFI_PATCHES[i], target, proc) != 0) ok = -1;
    }
    WAlogf(L"keepalive %s: %s", on ? L"apply" : L"restore", ok == 0 ? L"OK" : L"incomplete");
    return ok;
}

/* ── orphaned retry-timer cancel ──────────────────────────────────────── */

/* WIFI_PATCHES[0] NOPs the site that *arms* znet's battery retry timer (10 min
   on normal battery), but the NOP only blocks future arms. A timer that armed
   before keepalive was demanded - the device idled on battery, then keepalive
   was enabled mid-session (the "Keep WiFi on" toggle flipped late, or any
   zune-cast session) - is invisible to the NOP and fires anyway, dropping the
   link ~10 min later. (A persisted toggle escapes this only because it applies
   at boot, before the timer ever arms.) znet itself cancels a live timer only
   when its battery policy is re-evaluated via the patched AC path, which
   nothing here triggers, so cancel it directly.

   0x41a7f278 is znet's own cancel routine: timeKillEvent(retry_timer_id) +
   event signal + clears retry_timer_armed/id, under znet's lock. It takes the
   conn-ctx singleton in r0 and no-ops if nothing is armed. */
#define ZNET_CONN_CTX_VA        0x41af29d0u
#define ZNET_RETRY_ID_OFF       0xa90u
#define ZNET_RETRY_ARMED_OFF    0xa94u
#define ZNET_TIMER_CANCEL_VA    0x41a7f278u
#define ZNET_TIMER_CANCEL_FIRST 0xe92d4010u   /* push {r4, lr} - prologue guard vs a firmware shift */

typedef DWORD (*ZnetTimerCancelFn)(DWORD conn_ctx);

static void wa_cancel_armed_timer(void) {
    DWORD armed, id, prologue;
    __try {
        armed = *(volatile DWORD*)(ZNET_CONN_CTX_VA + ZNET_RETRY_ARMED_OFF);
        id    = *(volatile DWORD*)(ZNET_CONN_CTX_VA + ZNET_RETRY_ID_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;   /* znet conn-ctx not mapped yet - next tick retries */
    }
    if (!armed) return;
    __try { prologue = *(volatile DWORD*)ZNET_TIMER_CANCEL_VA; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (prologue != ZNET_TIMER_CANCEL_FIRST) {
        WAlogf(L"cancel-timer: wrapper prologue 0x%08x unexpected - skip", prologue);
        return;
    }
    __try {
        ((ZnetTimerCancelFn)ZNET_TIMER_CANCEL_VA)(ZNET_CONN_CTX_VA);
        WAlogf(L"cancel-timer: cancelled armed retry timer (id=0x%08x)", id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WAlogf(L"cancel-timer: wrapper call faulted");
    }
}

/* ── authority worker ─────────────────────────────────────────────────── */

DWORD WINAPI WifiAwakeAuthorityThread(LPVOID param) {
    HANDLE evt;
    int last_applied   = -1;   /* patch state: -1 unknown, 0 stock, 1 applied */
    int last_published = -1;   /* published status: -1 unknown, else 0/1 */
    (void)param;

    evt = CreateEventW(NULL, FALSE, FALSE, WIFI_AWAKE_EVENT);
    WAlogf(L"== authority start (evt=%p) ==", evt);

    for (;;) {
        int wanted;

        WaitForSingleObject(evt, 4000);   /* wake on a demand change or every 4s */

        wanted = wa_demanded();

        /* Publish effective demand as the keepalive STATUS so the wifiIcon tint
           reflects "Wi-Fi is being kept awake" by any demand source, independent
           of whether the patch write below succeeds. */
        if (wanted != last_published) {
            ModStateSetState(WIFI_AWAKE_STATUS_KEY, wanted, 0);
            ModStateEventPublish();   /* wake gemstone + servicesd icons to re-tint */
            last_published = wanted;
            WAlogf(L"keepalive status published=%d", wanted);
        }

        /* Apply (or restore) the patch set on a demand transition. The patches
           are durable once written, so this stays edge-triggered. On failure
           (kerncore not ready / module not mapped) leave last_applied unchanged
           so the next wake retries. */
        if (wanted != last_applied && wa_keepalive_set(wanted) == 0) {
            last_applied = wanted;
            WAlogf(L"demand=%d reconciled", wanted);
        }

        /* Pull, every tick while keepalive is applied: cancel any battery retry
           timer that armed before/around the demand (mid-session enable, or a
           re-association). The arm-site NOP cannot reach an already-armed
           timer; left alone it fires ~10 min later and drops the link. Cheap -
           a single read unless one is actually armed. */
        if (wanted && last_applied == 1) wa_cancel_armed_timer();
    }
    return 0;  /* not reached; satisfies SEH-conservative flow analysis (C4716) */
}

/* ── on-demand activation ─────────────────────────────────────────────── */

static int wa_is_servicesd(void) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* base;
    if (len == 0 || len >= MAX_PATH) return 0;
    base = path + len;
    while (base > path && *(base - 1) != L'\\') base--;
    return _wcsicmp(base, L"servicesd.exe") == 0;
}

void WifiAwake_EnsureActive(void) {
    /* The authority thread + the patch set live in servicesd. This only arms
       the thread, and only when a mod requires/provides wifi_awake. */
    if (!wa_is_servicesd()) return;
    if (InterlockedExchange(&g_authority_started, 1) == 0) {
        HANDLE h = CreateThread(NULL, 0, WifiAwakeAuthorityThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        WAlogf(L"== EnsureActive: authority spawned ==");
    }
}

void WifiAwake_Notify(void) {
    wa_signal_authority();
}
