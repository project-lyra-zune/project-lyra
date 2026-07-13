/* mods_quick_settings_scene.cpp - HUD quick-settings list scene class.

   The rich-row replacement for the flat ZuneContextMenu quick-toggle, built on
   the native record-array list pattern (mods_list_model): the scene owns a row
   snapshot, the list's data-source queries read from it, and on a change the
   snapshot is re-populated and the list is told to re-render IN PLACE via
   ListInvalidate_dataChanged (msg 0x7de), exactly how HudNetworkListScene
   updates its rows. The row SOURCE is pluggable (ModListSource), so the same
   scene machinery will back async device lists (Chromecast / nearby devices)
   later; discovery events just re-populate + ModListRefresh.

   Registered against parent HudActiveBaseScene; OnInit binds the `list`
   element; OnMessage answers the msg=0xe data-source sub-codes (count / get-text
   assoc 0+1 / get-image / row tap).

   Instance layout mirrors HudNetworkListScene so the base's writes don't trample
   our state: list_element at +0x08; instance_size 0x17c (set in the reloc).
   One quick-settings scene is open at a time (dismiss-before-open), so the model
   + bound list are file-static. */

extern "C" {
#include "mods_toggles.h"
#include "mods_state_block.h"   /* ModStateGetState / ModStateSetState */
#include "mods_state_event.h"   /* ModStateEventPublish */
#include "mods_settings.h"      /* ModSettingsSave */
#include "mods_wifi_awake.h"    /* WifiAwake_Notify */
#include "mods_list_model.h"    /* ModListModel / ModListSource / ModListRefresh */
#include "mods_curation.h"      /* ModCurationVisibleCount / ModCurationVisibleIndex */
#include "mods_icon_host.h"     /* ModsHudMenuRequestDismiss */
#include "mods_list_channel.h"  /* ModListChannelSubLabel (daemon-composed row sub-label) */
#include "mods_log.h"
}

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

struct ModQuickSettingsSceneInstance {
    DWORD vtable;         /* +0x00 - set by the class-blob ctor */
    DWORD scene_handle;   /* +0x04 - base-written; GetDescendantById root */
    DWORD list_element;   /* +0x08 - bound by OnInit (mirrors HudNetworkListScene) */
    DWORD cancel_element; /* +0x0c - tap-off button (mirrors HudNetworkListScene btnCancel@+0x0c) */
    /* +0x10..+0x178 owned by the HudActiveBaseScene base. instance_size 0x17c. */
};

/* ── Engine primitives (servicesd / zhud_serv, image base 0x419b0000) ──────── */
typedef HRESULT (*XuiGetDescByIdFn)(void* parent, const wchar_t* id, void** out, int flags);
static XuiGetDescByIdFn g_get_desc = NULL;

/* Assign a wide string into a row's text output slot (output[2], output[3], text).
   zhud VA from HudNetworkListScene's get-text path (0x419c0d04). */
typedef HRESULT (*RowAssignTextFn)(DWORD out_8, DWORD out_c, const wchar_t* text);
#define ROW_ASSIGN_TEXT  ((RowAssignTextFn)0x419d7bd8u)

/* Native HudNetworkListScene's image helper maps numeric WiFi icon ids to stock
   image-path strings, then uses the same string assigner. Custom mod row art can
   therefore supply a fully-qualified file:// path directly. */
#define ROW_ASSIGN_IMAGE_PATH  ROW_ASSIGN_TEXT

/* Selected-row index reader. zhud VA from HudNetworkListScene's 0xe/sub1 list arm
   (bl 0x419d6050 with r0=list; result used as row*0x13c into the record array). */
typedef int (*ListGetSelIdxFn)(void* list_element, void* out_secondary);
#define LIST_GET_SELECTED_IDX  ((ListGetSelIdxFn)0x419d6050u)

/* ── Message constants ───────────────────────────────────────────────────── */
#define MSG_INIT_BIND     0x13
#define MSG_DATA_SOURCE   0xe
#define SUB_DS_SET_SEL    0x01    /* row tap / set-selection */
#define SUB_DS_COUNT      0x3eb
#define SUB_DS_GET_TEXT   0x3e8   /* output[1] = DataAssociation: 0=main, 1=sub */
#define SUB_DS_GET_IMAGE  0x3e9   /* output[1] = DataAssociation: 0=art */
#define SUB_DS_CONTEXT    0x12    /* long-press / context request; carries touch point, not a target elem */

struct DataSourceSubStruct {
    DWORD sub_code;
    DWORD target_elem;
    DWORD size_hint;
    DWORD output_area;
};

/* ── The row model + the quick-toggle source ───────────────────────────────── */
static ModListModel g_model;

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

/* Append-per-line log, opened only from the rare sub=0x12 (hold) arm. The hot
   data-source arms (count/text/image) stay I/O-free; per-message file I/O on the
   pump thread is what caused the mods-tab first-present stall. */
#define QUICK_SETTINGS_LOG  L"\\flash2\\automation\\mods\\quicksettings.log"

static void qs_log(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mods_vflashlog(QUICK_SETTINGS_LOG, fmt, ap);
    va_end(ap);
}

/* Rows are the VISIBLE quick-toggle set (curated ∩ eligible), same source the
   flat menu used; `row` is a position in that set, mapped to a registry index. */
static int toggle_count(void* ctx) { (void)ctx; return ModCurationVisibleCount(); }

static void toggle_fill(void* ctx, int row, ModListRow* out) {
    int            idx = ModCurationVisibleIndex(row);
    const wchar_t* label;
    const char*    key;
    const wchar_t* sub;
    int            state_idx;
    (void)ctx;
    if (idx < 0) return;   /* row already zeroed by populate */
    label = ModToggleGetLabel(idx);
    key   = ModToggleGetKey(idx);
    copy_w(out->main, MODLIST_TEXT_LEN, label ? label : L"?");
    copy_a(out->key, MODLIST_KEY_LEN, key);
    /* Sub-label = the registry's per-state text indexed by the live state. When
       the setting links a status (the effect), index by that status slot's state
       (Connecting/Casting/Error...); otherwise by the setting's own value. */
    {
        const char* status_key = ModToggleGetStatusKey(idx);
        const char* read_key = status_key ? status_key : key;
        state_idx = read_key ? ModStateGetState(read_key) : 0;
    }
    if (state_idx < 0) state_idx = 0;
    sub = ModToggleGetStateLabel(idx, state_idx);
    /* A context-picker daemon (e.g. castd) can publish the exact row sub-label
       into its list channel (device name, SCANNING…/CONNECTING… loading text)
       which overrides the static state label when present. */
    {
        const wchar_t* ch_sub = key ? ModListChannelSubLabel(key) : NULL;
        if (ch_sub) sub = ch_sub;
    }
    copy_w(out->sub, MODLIST_TEXT_LEN, sub ? sub : L"");
    copy_w(out->icon, MODLIST_ICON_LEN, ModToggleGetQuickIcon(idx));
    out->user = (DWORD)idx;
}

static void toggle_on_tap(void* ctx, int row) {
    int         idx = ModCurationVisibleIndex(row);
    const char* key;
    (void)ctx;
    if (idx < 0) return;
    key = ModToggleGetKey(idx);
    if (key && ModToggleGetKind(idx) == MOD_TOGGLE_BINARY) {
        int cur = ModStateGetState(key);
        if (cur < 0) cur = 0;
        ModStateSetState(key, cur ? 0 : 1, 0);   /* control slot, owner 0 */
        ModStateEventPublish();   /* notify every consumer process */
        ModSettingsSave();         /* persist across reboot */
        WifiAwake_Notify();        /* re-evaluate keepalive now */
    }
}

static const ModListSource g_toggle_source = {
    toggle_count, toggle_fill, toggle_on_tap, NULL
};

static void resolve_engine(void) {
    HMODULE x;
    if (g_get_desc) return;
    x = GetModuleHandleW(L"xuidll.dll");
    if (x) g_get_desc = (XuiGetDescByIdFn)GetProcAddress(x, L"XuiElementGetDescendantById");
}

/* The open menu's list element (one menu open at a time). Used to invalidate the
   list when an external state change (e.g. castd's status) lands, mirroring how
   the native lists re-query on a notification. Valid only while the menu is open
   (the icon-host gates the live refresh on g_open_menu). */
static DWORD g_qs_list = 0;

/* Cross-message hold latch. A long-press is delivered to this scene as TWO
   OnMessage calls: the hold (sub=0x12) first, then on finger-up the row select
   (sub=0x01). Native HudNetworkListScene never receives that release-select on a
   hold (its gesture FSM consumes it); ours does, so without correlation the
   release runs the row's tap and toggles after the picker has opened. Setting
   m[2]=1 on the sub=0x12 message cannot suppress it; the release is a separate,
   later message. The latch arms when a hold opens a context picker and swallows
   the single immediately-following select for that same row.
   File-static like g_model/g_qs_list: one menu is open at a time. */
static int g_hold_consumed     = 0;
static int g_hold_consumed_row = -1;

/* External live refresh; mirrors how the native lists re-query their data source
   on a state-change notification: re-snapshot the rows and invalidate the list so
   the engine re-queries, picking up status sub-label changes (Connecting ->
   Casting -> ...). Called from the state-change drain (icon-host), gated on
   g_open_menu so g_qs_list is valid. Runs on the UI thread. */
extern "C" void ModsQuickSettingsLiveRefresh(void) {
    if (!g_qs_list) return;
    ModListModelPopulate(&g_model, &g_toggle_source);
    ModListRefresh((void*)g_qs_list);
}

extern "C" __declspec(dllexport)
HRESULT ModQuickSettingsScene_OnInit(ModQuickSettingsSceneInstance* self) {
    void* list = NULL;
    void* cancel = NULL;
    if (!self) return -1;
    self->list_element = 0;
    self->cancel_element = 0;
    resolve_engine();
    if (g_get_desc) {
        __try { g_get_desc((void*)self->scene_handle, L"list", &list, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { list = NULL; }
        __try { g_get_desc((void*)self->scene_handle, L"btnCancel", &cancel, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { cancel = NULL; }
    }
    self->list_element   = (DWORD)list;
    self->cancel_element = (DWORD)cancel;
    g_qs_list = (DWORD)list;   /* for external live-refresh while the menu is open */
    g_hold_consumed     = 0;   /* fresh open: no pending hold-release to swallow */
    g_hold_consumed_row = -1;

    /* Snapshot the rows from the source before the engine starts querying. */
    ModListModelPopulate(&g_model, &g_toggle_source);
    return 0;
}

extern "C" __declspec(dllexport)
HRESULT ModQuickSettingsScene_OnMessage(ModQuickSettingsSceneInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    DataSourceSubStruct* sub = NULL;
    DWORD sub_code = 0, target = 0;

    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    if (msg_id == MSG_INIT_BIND) {
        ModListModelPopulate(&g_model, &g_toggle_source);
        if (self && self->list_element) ModListRefresh((void*)self->list_element);
        return 0;
    }

    if (msg_id != MSG_DATA_SOURCE) return 0;

    __try {
        sub = (DataSourceSubStruct*)m[4];
        if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    if (!sub || !self) return 0;

    if (sub_code == SUB_DS_SET_SEL) {
        /* Tap-off (cancel button) → intentional dismiss; row tap → toggle. */
        if (self->cancel_element && target == self->cancel_element) {
            ModsHudMenuRequestDismiss();   /* tick removes our AddChild'd overlay */
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (target == self->list_element) {
            int row = -1;
            __try { row = LIST_GET_SELECTED_IDX((void*)self->list_element, NULL); }
            __except (EXCEPTION_EXECUTE_HANDLER) { row = -1; }
            if (g_hold_consumed) {
                int matched = (row == g_hold_consumed_row);
                qs_log(L"SET_SEL row=%d hold_row=%d -> %s",
                       row, g_hold_consumed_row, matched ? L"SWALLOW" : L"toggle(row mismatch)");
                g_hold_consumed     = 0;
                g_hold_consumed_row = -1;
                if (matched) {
                    __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    return 0;
                }
            }
            if (row >= 0 && row < g_model.count) {
                __try { g_toggle_source.on_tap(g_toggle_source.ctx, row); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                ModListModelPopulate(&g_model, &g_toggle_source);
                ModListRefresh((void*)self->list_element);
            }
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    /* Long-press / context request. Identify the held row (sel tracks it, device-
       validated), map it to its setting key, and if that key has a registered
       context provider, open the picker over this menu. The slot meanings differ
       from the data-source arms (payload carries the touch point, not a target
       element), so this runs before the target-elem gate. */
    if (sub_code == SUB_DS_CONTEXT) {
        int         sel = -1, idx = -1, handled = 0;
        const char* key = NULL;
        __try { sel = LIST_GET_SELECTED_IDX((void*)self->list_element, NULL); }
        __except (EXCEPTION_EXECUTE_HANDLER) { sel = -1; }
        if (sel >= 0 && sel < g_model.count) {
            idx = ModCurationVisibleIndex(sel);
            key = (idx >= 0) ? ModToggleGetKey(idx) : NULL;
            if (key) handled = ModsHudContextOpenForKey(key);
        }
        qs_log(L"HOLD sub=0x12 sel=%d idx=%d key=%S handled=%d",
               sel, idx, key ? key : "(null)", handled);
        if (handled) {
            /* Arm the latch so the release-select for this row is swallowed (the
               sub=0x12 m[2]=1 below cannot; the release is a separate message). */
            g_hold_consumed     = 1;
            g_hold_consumed_row = sel;
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    /* count / get-text target the list element. */
    if (target != self->list_element) return 0;

    if (sub_code == SUB_DS_COUNT) {
        __try {
            ((DWORD*)sub->output_area)[1] = (DWORD)g_model.count;
            m[2] = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    if (sub_code == SUB_DS_GET_TEXT) {
        DWORD* out = NULL;
        int idx = -1, assoc = -1;
        DWORD o8 = 0, oc = 0;
        const wchar_t* text = NULL;
        __try {
            out = (DWORD*)sub->output_area;
            if (out) { idx = (int)out[0]; assoc = (int)out[1]; o8 = out[2]; oc = out[3]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { out = NULL; }
        if (!out || idx < 0 || idx >= g_model.count) return 0;

        /* Read the pre-snapshot row: assoc 0 = main label, assoc 1 = sub label. */
        if (assoc == 0)      text = g_model.rows[idx].main;
        else if (assoc == 1) text = g_model.rows[idx].sub;
        if (text) {
            HRESULT hr = -1;
            __try { hr = ROW_ASSIGN_TEXT(o8, oc, text); }
            __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
            if (hr >= 0) __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    if (sub_code == SUB_DS_GET_IMAGE) {
        DWORD* out = NULL;
        int idx = -1, assoc = -1;
        DWORD o8 = 0, oc = 0;
        const wchar_t* icon = NULL;
        __try {
            out = (DWORD*)sub->output_area;
            if (out) { idx = (int)out[0]; assoc = (int)out[1]; o8 = out[2]; oc = out[3]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { out = NULL; }
        if (!out || idx < 0 || idx >= g_model.count || assoc != 0) return 0;

        icon = g_model.rows[idx].icon[0] ? g_model.rows[idx].icon : NULL;
        if (icon) {
            HRESULT hr = -1;
            __try { hr = ROW_ASSIGN_IMAGE_PATH(o8, oc, icon); }
            __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
            if (hr >= 0) __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    return 0;
}
