/* mods_context_list_scene.cpp - general modkit sub-list scene class (servicesd/zhud).

   The record-array list raised as an overlay from a long-pressed row. Same machinery
   as ModQuickSettingsScene (mods_list_model: the scene owns a row snapshot, the
   list's data-source queries read from it, ListInvalidate re-renders in place), but
   the row source is whatever the opener bound via ModContextListBind; this scene
   carries no domain knowledge. Selecting a row calls the source's on_tap and closes
   the overlay; tapping off closes it.

   Registered against parent HudActiveBaseScene. Instance layout mirrors
   HudNetworkListScene (list_element at +0x08, cancel at +0x0c; instance_size 0x17c)
   so the base's writes don't trample our state. One context list open at a time. */

extern "C" {
#include "mods_context_list_scene.h"
#include "mods_list_model.h"        /* ModListModel / ModListSource / ModListRefresh */
#include "mods_icon_host.h"         /* ModsHudContextRequestDismiss */
}

#include <windows.h>

struct ModContextListSceneInstance {
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
   zhud VA from HudNetworkListScene's get-text path. */
typedef HRESULT (*RowAssignTextFn)(DWORD out_8, DWORD out_c, const wchar_t* text);
#define ROW_ASSIGN_TEXT        ((RowAssignTextFn)0x419d7bd8u)
#define ROW_ASSIGN_IMAGE_PATH  ROW_ASSIGN_TEXT

/* Selected-row index reader (r0=list). zhud VA from HudNetworkListScene's 0xe/sub1. */
typedef int (*ListGetSelIdxFn)(void* list_element, void* out_secondary);
#define LIST_GET_SELECTED_IDX  ((ListGetSelIdxFn)0x419d6050u)

/* ── Message constants ───────────────────────────────────────────────────── */
#define MSG_INIT_BIND     0x13
#define MSG_DATA_SOURCE   0xe
#define SUB_DS_SET_SEL    0x01    /* row tap / set-selection */
#define SUB_DS_COUNT      0x3eb
#define SUB_DS_GET_TEXT   0x3e8   /* output[1] = DataAssociation: 0=main, 1=sub */
#define SUB_DS_GET_IMAGE  0x3e9   /* output[1] = DataAssociation: 0=art */

struct DataSourceSubStruct {
    DWORD sub_code;
    DWORD target_elem;
    DWORD size_hint;
    DWORD output_area;
};

/* ── Bound source + row snapshot (one open at a time) ───────────────────────── */
static const ModListSource* g_ctx_source = NULL;
static ModListModel         g_ctx_model;
static DWORD                g_ctx_list = 0;   /* open list element, for live refresh */

void ModContextListBind(const ModListSource* src) {
    g_ctx_source = src;
    if (src) ModListModelPopulate(&g_ctx_model, src);
    else     g_ctx_model.count = 0;
}

void ModContextListLiveRefresh(void) {
    if (!g_ctx_list || !g_ctx_source) return;
    ModListModelPopulate(&g_ctx_model, g_ctx_source);
    ModListRefresh((void*)g_ctx_list);
}

int ModContextListRowCount(void) {
    return g_ctx_model.count;
}

static void resolve_engine(void) {
    HMODULE x;
    if (g_get_desc) return;
    x = GetModuleHandleW(L"xuidll.dll");
    if (x) g_get_desc = (XuiGetDescByIdFn)GetProcAddress(x, L"XuiElementGetDescendantById");
}

extern "C" __declspec(dllexport)
HRESULT ModContextListScene_OnInit(ModContextListSceneInstance* self) {
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
    g_ctx_list = (DWORD)list;

    /* Snapshot from the bound source before the engine starts querying. */
    if (g_ctx_source) ModListModelPopulate(&g_ctx_model, g_ctx_source);
    return 0;
}

extern "C" __declspec(dllexport)
HRESULT ModContextListScene_OnMessage(ModContextListSceneInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    DataSourceSubStruct* sub = NULL;
    DWORD sub_code = 0, target = 0;

    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    if (msg_id == MSG_INIT_BIND) {
        if (g_ctx_source) ModListModelPopulate(&g_ctx_model, g_ctx_source);
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
        /* Tap-off (cancel button) → dismiss; row tap → select then dismiss. */
        if (self->cancel_element && target == self->cancel_element) {
            ModsHudContextRequestDismiss();
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (target == self->list_element) {
            int row = -1;
            __try { row = LIST_GET_SELECTED_IDX((void*)self->list_element, NULL); }
            __except (EXCEPTION_EXECUTE_HANDLER) { row = -1; }
            if (row >= 0 && row < g_ctx_model.count && g_ctx_source && g_ctx_source->on_tap) {
                __try { g_ctx_source->on_tap(g_ctx_source->ctx, row); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            ModsHudContextRequestDismiss();   /* a selection closes the picker */
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    /* count / get-text / get-image target the list element. */
    if (target != self->list_element) return 0;

    if (sub_code == SUB_DS_COUNT) {
        __try {
            ((DWORD*)sub->output_area)[1] = (DWORD)g_ctx_model.count;
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
        if (!out || idx < 0 || idx >= g_ctx_model.count) return 0;

        if (assoc == 0)      text = g_ctx_model.rows[idx].main;
        else if (assoc == 1) text = g_ctx_model.rows[idx].sub;
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
        if (!out || idx < 0 || idx >= g_ctx_model.count || assoc != 0) return 0;

        icon = g_ctx_model.rows[idx].icon[0] ? g_ctx_model.rows[idx].icon : NULL;
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
