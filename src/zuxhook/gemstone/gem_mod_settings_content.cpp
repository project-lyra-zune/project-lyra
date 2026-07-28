/* gem_mod_settings_content.cpp - list content for ModSettingsContent.xur.

   Sibling of GemModsListContentScene, but shaped like a native settings list
   rather than a music list: the scene's XuiList carries the stock SettingsList
   visual, whose row button has no named children, so a row is ONE label. The
   native get-item switch (gemstone 0x5c6f4) does the same thing for every row
   type it handles: compose one string and bind it. A value therefore lives in
   the row's own text ("safe mode: on"), never in a second column.

   Instance layout follows GemModsListContentScene's (0x50):

     +0x00   vtable_ptr
     +0x04   scene_handle
     +0x08   breadcrumb_elem
     +0x0c..+0x24  reserved (base may write here)
     +0x28   list_element
     +0x2c   noItems_element
     +0x30   view_subtype           (0=system, 1=mods; msg=0x13 args)
     +0x34   row_count
     +0x38..+0x4c reserved */

extern "C" {
#include "boot_state.h"
#include "mod_scanner.h"
}

#include "device_reboot.h"

#include "mods_state_block.h"   /* ModStateGetState / ModStateSetState - cross-process */
#include "mods_state_event.h"   /* ModStateEventPublish - wake the other consumers */

#include <windows.h>
#include "gem_scene_common.h"

typedef HRESULT (*RawRowLabelFn)(DWORD out_8, DWORD out_c, const wchar_t* text);
#define RAW_ROW_LABEL  ((RawRowLabelFn)0x00083914)

typedef int (*ListInvalidateFn)(void* list_element, int arg2, int arg3);
#define LIST_INVALIDATE  ((ListInvalidateFn)0x00058890)


typedef int (*ListGetSelectedIdxFn)(void* list_element, int* out_secondary);
#define GET_SELECTED_IDX  ((ListGetSelectedIdxFn)0x0003195c)

/* Two-button HUD confirm, the same call gem_mod_detail uses for destructive
   actions; the tap posts scene msg 0x8000007 with result 3. */
typedef int (*HudConfirmShowFn)(void* host, const wchar_t* message, int cfg, int flag,
                                int reserved, DWORD* out_handle);
#define HUD_CONFIRM_SHOW   ((HudConfirmShowFn)0x00072db8)
#define HUD_CONFIRM_CFG    8
#define HUD_CONFIRM_CFG_OK 3
#define HUD_CONFIRM_FLAG   1
#define MSG_HUD_RESULT     0x8000007
#define HUD_RESULT_CONFIRM 3

#define VIEW_SYSTEM  0
#define VIEW_MODS    1

#define SETTINGS_MAX_ROWS  24
#define SETTINGS_ROW_CHARS 64

struct GemModSettingsContentInstance {
    DWORD vtable;
    DWORD scene_handle;
    DWORD breadcrumb_elem;       /* +0x08 */
    DWORD reserved_0c[7];        /* +0x0c..+0x24 */
    DWORD list_element;          /* +0x28 */
    DWORD noItems_element;       /* +0x2c */
    DWORD view_subtype;          /* +0x30 */
    int   row_count;             /* +0x34 */
    DWORD reserved_tail[6];      /* +0x38..+0x4c */
};

/* The safe-mode row is always row 0 of the System tab. Leaving safe mode is an
   explicit act here, not an offer made after a failed boot: the user navigated
   into Settings to do it. */
#define SYSTEM_ROW_SAFE_MODE  0

static DWORD g_confirm_handle = 0;
static volatile LONG g_confirm_pending = 0;

static DWORD WINAPI clear_safe_mode_exec(LPVOID param) {
    (void)param;
    BootStateClear();
    RebootDevice();   /* does not return */
    return 0;
}

/* Composed row text. One list is live at a time (the manager swaps content
   scenes per tab), so a single static array backs whichever is showing. */
static wchar_t g_rows[SETTINGS_MAX_ROWS][SETTINGS_ROW_CHARS];

/* Which declaration each row came from. Rows are a filtered view of the
   declarations, so a row index is not a declaration index and a tap must not
   assume it is. */
static int g_row_decl[SETTINGS_MAX_ROWS];

static void row_append(wchar_t* dst, int* p, const wchar_t* s) {
    int i;
    for (i = 0; s[i] && *p < SETTINGS_ROW_CHARS - 1; i++) dst[(*p)++] = s[i];
    dst[*p] = 0;
}

static void row_append_a(wchar_t* dst, int* p, const char* s) {
    int i;
    for (i = 0; s[i] && *p < SETTINGS_ROW_CHARS - 1; i++)
        dst[(*p)++] = (wchar_t)(unsigned char)s[i];
    dst[*p] = 0;
}

static void build_system_rows(GemModSettingsContentInstance* self) {
    BootState bs;
    int n = 0, p = 0;

    BootStateRead(&bs);
    p = 0;
    row_append(g_rows[n], &p, L"safe mode: ");
    row_append_a(g_rows[n], &p,
                 bs.level == BOOT_LEVEL_NORMAL ? "off" : BootLevelName(bs.level));
    n++;


    self->row_count = n;
}

/* The declarations come from the manifests via the scanner, because
   register_setting lowers to servicesd and its registry does not exist in this
   process; the value comes from the shared state block, which does. */
static void build_mod_rows(GemModSettingsContentInstance* self) {
    int count = 0, i, n = 0;

    __try { count = ModScanSettingCount(); } __except (EXCEPTION_EXECUTE_HANDLER) { count = 0; }
    for (i = 0; i < count && n < SETTINGS_MAX_ROWS; i++) {
        const ModSettingDecl* d = NULL;
        int state = 0, p = 0;
        __try { d = ModScanSettingAt(i); } __except (EXCEPTION_EXECUTE_HANDLER) { d = NULL; }
        if (!d || !d->label) continue;
        __try { state = ModStateGetState(d->key); } __except (EXCEPTION_EXECUTE_HANDLER) { state = -1; }
        /* The declarations come from every mod dir on disk, including ones not
           enabled, held back, or faulted. A slot exists only where
           register_setting ran, so it is the one honest test of whether the
           setting is live: without it a row would show "off" for a control that
           is not there. */
        if (state < 0) continue;
        row_append(g_rows[n], &p, d->label);
        row_append(g_rows[n], &p, L": ");
        row_append(g_rows[n], &p, state > 0 ? L"on" : L"off");
        g_row_decl[n] = i;
        n++;
    }
    self->row_count = n;
}

static void rebuild_rows(GemModSettingsContentInstance* self) {
    self->row_count = 0;
    if (self->view_subtype == VIEW_MODS) build_mod_rows(self);
    else                                 build_system_rows(self);
}

static void flip_visibility(GemModSettingsContentInstance* self) {
    __try { LIST_INVALIDATE((void*)self->list_element, 0, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { SET_SHOW((void*)self->list_element, self->row_count > 0 ? 1 : 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (self->noItems_element) {
        __try { SET_SHOW((void*)self->noItems_element, self->row_count <= 0 ? 1 : 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

extern "C" __declspec(dllexport)
HRESULT GemModSettingsContent_OnInit(GemModSettingsContentInstance* self) {
    void* breadcrumb = NULL;
    void* list = NULL;
    void* noItems = NULL;
    int i;

    if (!self) return -1;
    self->breadcrumb_elem = 0;
    for (i = 0; i < 7; i++) self->reserved_0c[i] = 0;
    self->list_element    = 0;
    self->noItems_element = 0;
    self->view_subtype    = 0;
    self->row_count       = 0;

    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &breadcrumb, 0);
    self->breadcrumb_elem = (DWORD)breadcrumb;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"list", &list, 0);
    self->list_element = (DWORD)list;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"noItems", &noItems, 0);
    self->noItems_element = (DWORD)noItems;
    return 0;
}

extern "C" __declspec(dllexport)
HRESULT GemModSettingsContent_OnMessage(GemModSettingsContentInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    DWORD sub_code = 0, target = 0;
    DataSourceSubStruct* sub = NULL;
    HRESULT hr = 0;

    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    if (msg_id == MSG_DATA_SOURCE) {
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (msg_id == MSG_INIT_BIND) {
        DWORD parent_arg = 0;
        __try {
            DWORD** outer = (DWORD**)m[4];
            if (outer) {
                DWORD* args_ptr = *outer;
                if (args_ptr) parent_arg = *args_ptr;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        self->view_subtype = parent_arg;
        rebuild_rows(self);
        flip_visibility(self);
        __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    if (msg_id == MSG_HUD_RESULT) {
        DWORD* payload = NULL;
        DWORD h = 0, result = 0;
        __try { payload = (DWORD*)m[4]; if (payload) { h = payload[0]; result = payload[1]; } }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (payload && h && h == g_confirm_handle) {
            g_confirm_handle = 0;
            if (result == HUD_RESULT_CONFIRM) {
                HANDLE t = CreateThread(NULL, 0, clear_safe_mode_exec, NULL, 0, NULL);
                if (t) CloseHandle(t);
            } else {
                InterlockedExchange(&g_confirm_pending, 0);
            }
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
    }

    /* Row tap: only the System tab's safe-mode row acts, and only when demoted. */
    if (msg_id == MSG_DATA_SOURCE && sub && sub_code == SUB_DS_SET_SEL
        && target == self->list_element && self->view_subtype == VIEW_SYSTEM) {
        int idx = -1;
        __try { idx = GET_SELECTED_IDX((void*)self->list_element, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { idx = -1; }
        if (idx == SYSTEM_ROW_SAFE_MODE
            && InterlockedCompareExchange(&g_confirm_pending, 1, 0) == 0) {
            BootState bs;
            int demoted, hr = -1;
            BootStateRead(&bs);
            demoted = (bs.level != BOOT_LEVEL_NORMAL);
            g_confirm_handle = 0;
            /* Always answer the tap. A row that silently does nothing when the
               device is healthy is indistinguishable from a broken one. */
            __try {
                hr = demoted
                   ? HUD_CONFIRM_SHOW((void*)self->scene_handle,
                         L"Leave safe mode? Your mods will be applied again on "
                         L"restart, including any that stopped it starting.",
                         HUD_CONFIRM_CFG, HUD_CONFIRM_FLAG, 0, &g_confirm_handle)
                   : HUD_CONFIRM_SHOW((void*)self->scene_handle,
                         L"Safe mode is off, so every mod you have enabled is "
                         L"being applied. If a mod stops your Zune starting "
                         L"twice in a row, Lyra turns safe mode on by itself: it "
                         L"loads only its own components and leaves your mods "
                         L"off, so you can still reach this menu and disable the "
                         L"one at fault. While it is on, tapping here turns it "
                         L"back off and restarts.",
                         HUD_CONFIRM_CFG_OK, HUD_CONFIRM_FLAG, 0, &g_confirm_handle);
            } __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
            if (hr < 0 || !demoted) {
                InterlockedExchange(&g_confirm_pending, 0);
                g_confirm_handle = 0;   /* informational: nothing to act on */
            }
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
    }

    /* Mods tab: a tap flips the setting. The value lives in the shared state
       block, so the write is visible to whichever process acts on it. */
    if (msg_id == MSG_DATA_SOURCE && sub && sub_code == SUB_DS_SET_SEL
        && target == self->list_element && self->view_subtype == VIEW_MODS) {
        int idx = -1;
        __try { idx = GET_SELECTED_IDX((void*)self->list_element, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { idx = -1; }
        if (idx >= 0 && idx < self->row_count) {
            const ModSettingDecl* d = NULL;
            __try { d = ModScanSettingAt(g_row_decl[idx]); }
            __except (EXCEPTION_EXECUTE_HANDLER) { d = NULL; }
            if (d) {
                int cur = 0;
                __try {
                    cur = ModStateGetState(d->key);
                    ModStateSetState(d->key, cur ? 0 : 1, 0);
                    ModStateEventPublish();
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                rebuild_rows(self);
                flip_visibility(self);
            }
        }
        __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    if (msg_id == MSG_DATA_SOURCE && sub && target == self->list_element) {
        if (sub_code == SUB_DS_COUNT) {
            __try {
                *((DWORD*)(sub->output_area + 4)) = (DWORD)self->row_count;
                m[2] = 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (sub_code == SUB_DS_GET_ITEM) {
            DWORD* output = NULL;
            int idx = -1, col = 0;
            DWORD out_8 = 0, out_c = 0;
            __try {
                output = (DWORD*)sub->output_area;
                if (output) {
                    idx   = (int)output[0];
                    col   = (int)output[1];
                    out_8 = output[2];
                    out_c = output[3];
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (!output || idx < 0 || idx >= self->row_count) return 0;
            /* SettingsList renders one line, so only column 0 is answered. */
            if (col != 0) return 0;
            __try {
                if ((int)RAW_ROW_LABEL(out_8, out_c, g_rows[idx]) >= 0) m[2] = 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
    }

    __try { hr = XUISCENE_ON_MESSAGE(self, msg); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}
