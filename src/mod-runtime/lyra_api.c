/* The mod-facing API. Signatures here are published: adding a verb raises
   lyra.mod_runtime's `cur`, changing or removing one raises its `min_compat`.
   Nothing but primitives and NUL-terminated strings may cross. */

#include <windows.h>

#include <string.h>

#include "mods_state_block.h"
#include "mod_notify.h"
#include "mods_list_channel.h"
#include "kerncore.h"
#include "mods_hook.h"
#include "mods_ui_post.h"

#define LYRA_API __declspec(dllexport)

/* ── state ───────────────────────────────────────────────────────────────── */

LYRA_API int lyra_state_get(const char* key) {
    return ModStateGetState(key);
}

/* The pid is what lets the platform reset this slot if the daemon dies. */
LYRA_API void lyra_state_set_status(const char* key, int state) {
    if (!key || strncmp(key, "status/", 7) != 0) return;
    if (ModStateSetState(key, state, GetCurrentProcessId()))
        ModStateEventPublish();
}

/* owner 0: an intent slot belongs to no process, so the reaper must leave it. */
LYRA_API void lyra_state_set_setting(const char* key, int state) {
    if (!key || strncmp(key, "setting/", 8) != 0) return;
    if (ModStateSetState(key, state, 0))
        ModStateEventPublish();
}

LYRA_API void lyra_state_notify(void) {
    ModStateEventPublish();
}

/* One event per name, not per process: an auto-reset wake must reach its single
   intended waiter, and mods can share a host. */
#define LYRA_EVT_CACHE_MAX 8
static struct { wchar_t name[MOD_NOTIFY_NAME_LEN]; HANDLE h; } g_evt[LYRA_EVT_CACHE_MAX];
static int g_evt_n = 0;
static CRITICAL_SECTION g_evt_cs;

/* Called from DllMain: attach precedes any export, which a lazy init cannot. */
LYRA_API void LyraRuntimeProcessAttach(void) {
    InitializeCriticalSection(&g_evt_cs);
    ModListChannelProcessAttach();
    ModUiPostProcessAttach();
}

LYRA_API HANDLE lyra_state_change_event(const wchar_t* daemon_event_name) {
    HANDLE h = NULL;
    int i;
    if (!daemon_event_name) return NULL;
    EnterCriticalSection(&g_evt_cs);
    for (i = 0; i < g_evt_n; i++)
        if (wcscmp(g_evt[i].name, daemon_event_name) == 0) { h = g_evt[i].h; break; }
    if (!h && g_evt_n < LYRA_EVT_CACHE_MAX) {
        h = CreateEventW(NULL, FALSE, FALSE, daemon_event_name);
        if (h) {
            for (i = 0; i < MOD_NOTIFY_NAME_LEN - 1 && daemon_event_name[i]; i++)
                g_evt[g_evt_n].name[i] = daemon_event_name[i];
            g_evt[g_evt_n].name[i] = 0;
            g_evt[g_evt_n].h = h;
            g_evt_n++;
            ModNotifyRegister(MOD_NOTIFY_DAEMON_EVENT, daemon_event_name);
        }
    }
    LeaveCriticalSection(&g_evt_cs);
    return h;
}

/* ── picker channel ──────────────────────────────────────────────────────────
   Every verb takes the setting key: mods sharing a host process (load_module
   into gemstone) must not collide on a bound channel. */

static void copy_w(wchar_t* dst, int cap, const wchar_t* src) {
    int i = 0;
    if (src) for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void copy_a(char* dst, int cap, const char* src) {
    int i = 0;
    if (src) for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

LYRA_API HANDLE lyra_channel_scan_event(const char* toggle_key) {
    return ModListChannelScanEvent(toggle_key);
}

LYRA_API int lyra_channel_stage_row(const char* toggle_key, int idx, const wchar_t* name,
                                    const wchar_t* sub, const char* value) {
    return ModListChannelStage(toggle_key, idx, name, sub, value);
}

LYRA_API void lyra_channel_commit(const char* toggle_key, int count) {
    ModListChannelCommit(toggle_key, count);
    ModStateEventPublish();    /* an open picker re-queries */
}

LYRA_API void lyra_channel_open(const char* toggle_key) {
    ModListChannelRequestOpen(toggle_key);
    ModStateEventPublish();    /* the HUD tick picks the request up */
}

LYRA_API int lyra_channel_row_count(const char* toggle_key) {
    ModListChannelBlock* b = ModListChannelMap(toggle_key);
    int n;
    if (!b) return 0;
    n = (int)b->count;
    return (n > MODLISTCH_MAX_ROWS) ? MODLISTCH_MAX_ROWS : n;
}

LYRA_API int lyra_channel_get_row(const char* toggle_key, int idx,
                                  wchar_t* name_out, int name_cap,
                                  wchar_t* sub_out, int sub_cap,
                                  char* value_out, int value_cap) {
    ModListChannelBlock* b = ModListChannelMap(toggle_key);
    if (!b || idx < 0 || idx >= lyra_channel_row_count(toggle_key)) return 0;
    if (name_out)  copy_w(name_out, name_cap, b->row[idx].name);
    if (sub_out)   copy_w(sub_out, sub_cap, b->row[idx].sub);
    if (value_out) copy_a(value_out, value_cap, b->row[idx].value);
    return 1;
}

LYRA_API int lyra_channel_get_selection(const char* toggle_key, char* out, int out_cap) {
    ModListChannelBlock* b = ModListChannelMap(toggle_key);
    if (!b || !out || out_cap <= 0) return 0;
    copy_a(out, out_cap < MODLISTCH_VAL_LEN + 1 ? out_cap : MODLISTCH_VAL_LEN + 1, b->sel_value);
    return out[0] ? 1 : 0;
}

LYRA_API void lyra_channel_set_selection(const char* toggle_key, const char* value) {
    ModListChannelSelect(toggle_key, value);
    ModStateEventPublish();
}

LYRA_API void lyra_channel_set_sublabel(const char* toggle_key, const wchar_t* text) {
    ModListChannelBlock* b = ModListChannelMap(toggle_key);
    int i;
    if (!b) return;
    /* Callers re-assert the same label on a hot path; only write and wake the UI
       when it actually changes. */
    for (i = 0; i < MODLISTCH_SUBLABEL_LEN - 1 && text && text[i]; i++)
        if (b->sublabel[i] != text[i]) break;
    if ((text ? text[i] : 0) == b->sublabel[i]) return;
    copy_w(b->sublabel, MODLISTCH_SUBLABEL_LEN, text);
    ModStateEventPublish();
}

/* ── kernel tools ────────────────────────────────────────────────────────── */

LYRA_API int lyra_kernel_ready(void) {
    return kerncore_is_ready();
}

LYRA_API int lyra_kernel_ensure_helpers(void) {
    return kerncore_ensure_helpers();
}

LYRA_API DWORD lyra_kreadu32(DWORD va) {
    return kerncore_kreadu32(va);
}

LYRA_API void lyra_kread(DWORD va, void* buf, DWORD len) {
    kerncore_kread(va, buf, len);
}

LYRA_API void lyra_kmemcpy(DWORD va, const void* buf, DWORD len) {
    kerncore_kmemcpy(va, buf, (size_t)len);
}

LYRA_API DWORD lyra_kcall(DWORD fn, DWORD a0, DWORD a1, DWORD a2,
                          DWORD a3, DWORD a4, DWORD a5) {
    return kerncore_kcall(fn, a0, a1, a2, a3, a4, a5);
}

LYRA_API DWORD lyra_find_proc_struct(DWORD pid) {
    return kerncore_find_proc_struct(pid);
}

LYRA_API DWORD lyra_kscratch(void) {
    return KERNCORE_KSCRATCH;
}

/* helper_v4 is bx-to-shellcode with a TTBR swap: enter `target_proc`'s address
   space and branch to code_va. */
LYRA_API DWORD lyra_kexec_in_proc(DWORD target_proc, DWORD code_va) {
    return kerncore_kcall(KERNCORE_HELPER_V4, target_proc, code_va, 0, 0, 0, 0);
}

LYRA_API int lyra_patch_code(DWORD target_proc, DWORD target_va,
                             const void* bytes, int len) {
    return kerncore_patch_code(target_proc, target_va, bytes, len);
}

LYRA_API int lyra_hook_install(DWORD target_va, void* replacement, void** out_next) {
    return ModHookInstall(target_va, replacement, out_next);
}

LYRA_API int lyra_ui_post(void (*fn)(void*), void* ctx) {
    return ModUiPost((ModUiFn)fn, ctx);
}
