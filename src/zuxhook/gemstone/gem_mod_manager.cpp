/* gem_mod_manager.cpp - outer shell class for ManageMods.xur.

   Canonical shell+content structure (matches MarketplaceGames +
   MarketplaceGamesListContent + MarketplaceGamesDetailsContent):

     ManageMods.xur          → this class (shell: chrome + twist)
     ManageModsContent.xur   → GemModsListContentScene (list, swapped per tab)
     ManageModDetail.xur     → GemModDetail (drilldown via id_nav_stub)

   Content load is driven by the inherited GemLibraryBaseScene helper:
   the helper self-dispatches msg=0x1800001c (its 0x3ed94) to our outer
   scene, and our OnMessage handler creates ManageModsContent.xur by
   name and returns the handle for the helper to adopt.

   Instance layout - GemModHub's device-validated shape (breadcrumb at
   +0x08 + XuiScene base tail-call for back-nav). Class-private state
   lives past +0x28 so XuiScene base's writes to +0x0c..+0x18 don't
   trample it:

     +0x00   vtable_ptr
     +0x04   scene_handle
     +0x08   breadcrumb_elem        (XuiScene base reads - bound by OnInit)
     +0x0c..+0x18  reserved (base may write here)
     +0x1c   nav_source_elem        (returned by msg=0x18000022)
     +0x20..+0x24  reserved
     +0x28   twist_elem             (bound by OnInit)
     +0x2c   selected_tab
     +0x30   twist_ds[0..3]
     +0x40   reserved (unused)
     +0x44   reserved (unused)
     +0x48   installed_label_id     (ctor extra_init)
     +0x4c   archived_label_id      (ctor extra_init)
     +0x50   updates_label_id       (ctor extra_init)
     +0x54   reserved */

extern "C" {
#include "mod_scanner.h"
}

#include <windows.h>
#include "gem_scene_common.h"
#include "repo_client.h"

struct GemModManagerInstance {
    DWORD vtable;
    DWORD scene_handle;
    DWORD breadcrumb_elem;       /* +0x08 */
    DWORD reserved_0c[4];        /* +0x0c..+0x18 */
    DWORD nav_source_elem;       /* +0x1c */
    DWORD reserved_20[2];        /* +0x20..+0x24 */
    DWORD twist_elem;            /* +0x28 */
    DWORD selected_tab;          /* +0x2c */
    DWORD twist_ds[4];           /* +0x30..+0x3c */
    DWORD reserved_40;           /* +0x40 - unused */
    DWORD reserved_44;           /* +0x44 - unused */
    DWORD installed_label_id;    /* +0x48 - extra_init */
    DWORD archived_label_id;     /* +0x4c - extra_init */
    DWORD updates_label_id;      /* +0x50 - extra_init */
    DWORD reserved_54;
};

/* ── Engine primitives ─────────────────────────────────────────────────── */

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

/* Name-based scene create. xuidll!XuiSceneCreate takes a base URI
   prefix + scene name path; init_data is passed through to the new
   scene (delivered as msg=0x13 args). */
typedef HRESULT (*XuiSceneCreateFn)(const wchar_t* base, const wchar_t* path,
                                      void* init_data, void** out_handle);
#define XUI_SCENE_CREATE  ((XuiSceneCreateFn)0x418358d0)

/* ── Message constants ─────────────────────────────────────────────────── */

#define MSG_CONTENT_LOAD    0x1800001c   /* sent by the inherited helper's
                                             0x3ed94 to XuiGetOuter(scene)
                                             → routes to OUR OnMessage.
                                             Payload[0]=idx; we fill
                                             payload[+4]=new scene handle. */
#define MSG_NAV_SOURCE      0x18000022

/* Content swap is now handled by the canonical GemLibraryBaseScene
   helper class; our class inherits from it via parent_name in the
   manifest. The helper's 0x3ef40 OnMessage handles twist commits, its
   0x3ed94 builds msg=0x1800001c → calls 0x1c250 → 0x3eb54 adopts the
   new content. No manual content-load code needed here. */

/* ── Class entry points ────────────────────────────────────────────────── */

extern "C" __declspec(dllexport)
HRESULT GemModManager_OnInit(GemModManagerInstance* self) {
    if (!self) return -1;

    /* Re-project the mod row set from disk on every Manage entry, before
       the content-list child scenes are created (via MSG_CONTENT_LOAD) and
       read ModScanGet. reposd mutates enabled.json + the mod dirs live from
       the Browse UI, so this is what keeps Manage in sync without a reboot. */
    __try { ModScanRebuild(); } __except (EXCEPTION_EXECUTE_HANDLER) {}

    self->breadcrumb_elem        = 0;
    for (int i = 0; i < 4; i++) self->reserved_0c[i] = 0;
    self->nav_source_elem        = 0;
    self->reserved_20[0]         = 0;
    self->reserved_20[1]         = 0;
    self->twist_elem             = 0;
    self->selected_tab           = 0;
    self->twist_ds[0]            = 0;
    self->twist_ds[1]            = 0;
    self->twist_ds[2]            = 0;
    self->twist_ds[3]            = 0;
    self->reserved_40            = 0;
    self->reserved_44            = 0;

    void* breadcrumb = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &breadcrumb, 0);
    self->breadcrumb_elem = (DWORD)breadcrumb;

    void* twist = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"twist", &twist, 0);
    self->twist_elem = (DWORD)twist;
    self->nav_source_elem = (DWORD)twist;

    /* Manage is a reposd client for the updates tab: request the feed on every entry
       (WiFi/DNS may be late at boot). The updates list starts from the current feed
       snapshot and the async DONE re-filters it (GemModsListHandleRepoDone). */
    RepoClientRequestFeed();
    return 0;
}


extern "C" __declspec(dllexport)
HRESULT GemModManager_OnMessage(GemModManagerInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    DWORD sub_code = 0, target = 0;
    DataSourceSubStruct* sub = NULL;
    if (msg_id == MSG_DATA_SOURCE) {
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    /* msg=0x13 - populate twist data source (so our tab labels render).
       The canonical content load is now driven by the inherited
       GemLibraryBaseScene helper; we just populate the twist DS and
       fall through. The helper's msg=0x13 (via class chain) handles
       content load. */
    if (msg_id == MSG_INIT_BIND) {
        __try {
            TWIST_DS_INIT(self->twist_ds,
                          self->twist_ds[0], self->twist_ds[1]);
            TWIST_DS_ADD_ROW(self->twist_ds, -1,
                              self->installed_label_id, 0, 0);
            TWIST_DS_ADD_ROW(self->twist_ds, -1,
                              self->updates_label_id, 0, 0);
            TWIST_DS_ADD_ROW(self->twist_ds, -1,
                              self->archived_label_id, 0, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        /* Don't mark handled - let helper run msg=0x13 too. */
    }

    /* Twist data source queries. */
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
            if (!output) return 0;
            int hr = -1;
            __try {
                hr = TWIST_DS_GET_ITEM(self->twist_ds, idx, out_8, out_c);
            } __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
            if (hr >= 0) {
                __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            return 0;
        }
        /* Tab commits arrive as msg=0x1800001c (handled below), not as
           SUB_DS_SET_SEL on twist: the tap bubbles up with
           target=row_element, and the engine synthesizes the tab-commit
           signal from it. */
    }

    /* msg=0x1800001c - the canonical helper (inherited GemLibraryBaseScene)
       sends this to XuiGetOuter(scene_handle), which routes back to OUR
       OnMessage. Payload[0]=tab idx, payload[+4]=out_handle slot. We
       create the content scene by name and the helper adopts the
       returned handle on return. The helper handles adoption via
       AddChild + transition; we don't navigate ourselves. */
    if (msg_id == MSG_CONTENT_LOAD) {
        DWORD* payload = NULL;
        __try { payload = (DWORD*)m[4]; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (payload) {
            int idx = (int)payload[0];
            if (idx < 0) idx = 0;
            if (idx > 2) idx = 2;
            DWORD sub_idx = (DWORD)idx;
            void* hScene = NULL;
            HRESULT hr = (HRESULT)-1;
            __try {
                hr = XUI_SCENE_CREATE(L"gem://", L"ManageModsContent.xur",
                                        &sub_idx, &hScene);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            (void)hr;
            payload[1] = (DWORD)hScene;
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    /* msg=0x18000022 - nav-source query. Call ListGetSelectedIdx(twist,
       sub_payload), which writes the selected twist row's element handle
       into sub_payload[0]. The destination scene walks up from there to
       find an XuiLabel/XuiText for the breadcrumb morph. */
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

    /* Fall through to XuiScene base for breadcrumb visuals + back-nav
       (msg=0xe/sub=1 with target==breadcrumb_elem at [self+8]). */
    HRESULT hr = 0;
    __try { hr = XUISCENE_ON_MESSAGE(self, msg); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}
