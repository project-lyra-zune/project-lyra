/* gem_mod_settings.cpp - outer shell class for ModSettings.xur.

   Same shell+content structure as GemModManager, reached from the hub's
   settings button. The twist divides settings by who owns them, not by
   subject: System (platform behaviour, safe mode, the quick-toggle menu) and
   Mods (each mod's registered settings). That axis is deliberately not the
   feed's category vocabulary, so nothing has to stay in sync with it.

   Instance layout follows GemModManager's, which is GemModHub's
   device-validated shape (breadcrumb at +0x08, class-private state past +0x28
   so XuiScene base's writes to +0x0c..+0x18 don't trample it):

     +0x00   vtable_ptr
     +0x04   scene_handle
     +0x08   breadcrumb_elem
     +0x0c..+0x18  reserved (base may write here)
     +0x1c   nav_source_elem
     +0x20..+0x24  reserved
     +0x28   twist_elem
     +0x2c   selected_tab
     +0x30   twist_ds[0..3]
     +0x40   reserved
     +0x44   reserved
     +0x48   system_label_id        (ctor extra_init)
     +0x4c   mods_label_id          (ctor extra_init)
     +0x50   reserved
     +0x54   reserved */

#include <windows.h>
#include "gem_scene_common.h"

struct GemModSettingsInstance {
    DWORD vtable;
    DWORD scene_handle;
    DWORD breadcrumb_elem;       /* +0x08 */
    DWORD reserved_0c[4];        /* +0x0c..+0x18 */
    DWORD nav_source_elem;       /* +0x1c */
    DWORD reserved_20[2];        /* +0x20..+0x24 */
    DWORD twist_elem;            /* +0x28 */
    DWORD selected_tab;          /* +0x2c */
    DWORD twist_ds[4];           /* +0x30..+0x3c */
    DWORD reserved_40;           /* +0x40 */
    DWORD reserved_44;           /* +0x44 */
    DWORD system_label_id;       /* +0x48 - extra_init */
    DWORD mods_label_id;         /* +0x4c - extra_init */
    DWORD reserved_50;
    DWORD reserved_54;
};

typedef int (*ListGetSelectedIdxFn)(void* elem, void* sub_payload_or_zero);
#define GET_SELECTED_IDX  ((ListGetSelectedIdxFn)0x0003195c)

typedef int (*DataSourceCountFn)(DWORD* ds);
#define TWIST_DS_COUNT  ((DataSourceCountFn)0x0002a0ac)

typedef int (*DataSourceGetItemFn)(DWORD* ds, int idx, DWORD out_8, DWORD out_c);
#define TWIST_DS_GET_ITEM  ((DataSourceGetItemFn)0x0002a01c)

typedef int (*DataSourceInitFn)(DWORD* ds, DWORD start, DWORD end);
#define TWIST_DS_INIT  ((DataSourceInitFn)0x00024ef0)

typedef int (*DataSourceAddRowFn)(DWORD* ds, int parent_idx,
                                  DWORD string_id, DWORD value, DWORD extra);
#define TWIST_DS_ADD_ROW  ((DataSourceAddRowFn)0x0002a12c)

#define MSG_NAV_SOURCE    0x18000022
#define MSG_CONTENT_LOAD  0x1800001c

typedef HRESULT (*XuiSceneCreateFn)(const wchar_t* base, const wchar_t* path,
                                    void* init_data, void** out_handle);
#define XUI_SCENE_CREATE  ((XuiSceneCreateFn)0x418358d0)

#define SETTINGS_TAB_COUNT  2

extern "C" __declspec(dllexport)
HRESULT GemModSettings_OnInit(GemModSettingsInstance* self) {
    void* breadcrumb = NULL;
    void* twist = NULL;
    int i;

    if (!self) return -1;

    self->breadcrumb_elem = 0;
    for (i = 0; i < 4; i++) self->reserved_0c[i] = 0;
    self->nav_source_elem = 0;
    self->reserved_20[0]  = 0;
    self->reserved_20[1]  = 0;
    self->twist_elem      = 0;
    self->selected_tab    = 0;
    self->twist_ds[0]     = 0;
    self->twist_ds[1]     = 0;
    self->twist_ds[2]     = 0;
    self->twist_ds[3]     = 0;
    self->reserved_40     = 0;
    self->reserved_44     = 0;

    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &breadcrumb, 0);
    self->breadcrumb_elem = (DWORD)breadcrumb;

    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"twist", &twist, 0);
    self->twist_elem      = (DWORD)twist;
    self->nav_source_elem = (DWORD)twist;
    return 0;
}

extern "C" __declspec(dllexport)
HRESULT GemModSettings_OnMessage(GemModSettingsInstance* self, void* msg) {
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
        __try {
            TWIST_DS_INIT(self->twist_ds, self->twist_ds[0], self->twist_ds[1]);
            TWIST_DS_ADD_ROW(self->twist_ds, -1, self->system_label_id, 0, 0);
            TWIST_DS_ADD_ROW(self->twist_ds, -1, self->mods_label_id, 0, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        /* Not handled: the inherited helper runs msg=0x13 too. */
    }

    if (msg_id == MSG_DATA_SOURCE && sub && target == self->twist_elem) {
        if (sub_code == SUB_DS_COUNT) {
            __try {
                int count = TWIST_DS_COUNT(self->twist_ds);
                *((DWORD*)(sub->output_area + 4)) = (DWORD)count;
                m[2] = 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (sub_code == SUB_DS_GET_ITEM) {
            DWORD* output = NULL;
            int idx = -1;
            DWORD out_8 = 0, out_c = 0;
            __try {
                output = (DWORD*)sub->output_area;
                if (output) {
                    idx   = (int)output[0];
                    out_8 = output[2];
                    out_c = output[3];
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (output && idx >= 0) {
                __try {
                    TWIST_DS_GET_ITEM(self->twist_ds, idx, out_8, out_c);
                    m[2] = 1;
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            return 0;
        }
    }

    if (msg_id == MSG_CONTENT_LOAD) {
        DWORD* payload = NULL;
        __try { payload = (DWORD*)m[4]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (payload) {
            int idx = (int)payload[0];
            DWORD sub_idx;
            void* hScene = NULL;
            if (idx < 0) idx = 0;
            if (idx >= SETTINGS_TAB_COUNT) idx = SETTINGS_TAB_COUNT - 1;
            sub_idx = (DWORD)idx;
            __try {
                XUI_SCENE_CREATE(L"gem://", L"ModSettingsContent.xur", &sub_idx, &hScene);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            payload[1] = (DWORD)hScene;
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    if (msg_id == MSG_NAV_SOURCE) {
        __try {
            DWORD* payload = (DWORD*)m[4];
            if (payload) {
                GET_SELECTED_IDX((void*)self->twist_elem, payload);
                m[2] = 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    __try { hr = XUISCENE_ON_MESSAGE(self, msg); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}
