/* gem_mod_hub.cpp - C++ handlers for the mod-hub scene class.

   The mod-hub is the landing scene the user arrives at when tapping the
   "mods" tile in the start menu. Three large rows along the bottom:
   manage / browse / settings. Tapping each navigates to a child scene.

   Layout modeled after GemMarketplaceScene (gemstone v4.5 firmware,
   factory 0x2ba50, vtable 0x14ffc, OnMessage 0x4ec8c, DS-set-sel handler
   0x4e7f4). XuiButton presses route through the
   *same* msg 0xe / sub_code 1 path the data-source protocol uses for
   XuiList row taps. The button's element handle arrives in
   `sub_payload[+0x4]` (`target_elem`); the handler matches it against
   scene-instance-stored button bindings and tail-calls `0x1c284`
   (id_nav_stub) with the corresponding scene_id.

   Instance layout. Button bindings live past +0x28 so XuiScene base
   writes to +0x0c..+0x18 (sel-cache, breadcrumb dispatch chain) don't
   trample them. Scene_id constants are baked in by the ctor at plant
   time via the build script's `extra_init` overrides; they are
   compile-time-known per manifest action order. */

#include <windows.h>
#include "gem_scene_common.h"

struct GemModHubInstance {
    DWORD vtable;            /* +0x00 - set by ctor */
    DWORD scene_handle;      /* +0x04 - set by factory from r0 */
    DWORD breadcrumb_elem;   /* +0x08 - XuiScene base reads this */
    DWORD reserved_0c;       /* +0x0c - XuiScene base writes */
    DWORD reserved_10;       /* +0x10 */
    DWORD reserved_14;       /* +0x14 */
    DWORD nav_source_elem;   /* +0x18 - tapped button handle; engine reads
                                 this via msg 0x18000022 to seed the
                                 destination scene's breadcrumbTransText/
                                 Image morph. Without this set, the engine
                                 has no source element to animate from and
                                 the destination breadcrumb never appears. */
    DWORD reserved_1c;
    DWORD reserved_20;
    DWORD reserved_24;
    DWORD manage_btn;        /* +0x28 - bound at OnInit */
    DWORD browse_btn;        /* +0x2c */
    DWORD settings_btn;      /* +0x30 */
    DWORD reserved_34;
    DWORD reserved_38;
    DWORD reserved_3c;
};
/* instance_size = 0x40 (64 bytes), 4-aligned, fits imm8 */

/* Name-based scene navigation. xuidll!XuiSceneCreate loads a .xur by
   URI; gemstone+0x1e5d8 (scene_navigate_wrapper) installs the new
   scene as the active one and transitions to it. */
typedef HRESULT (*XuiSceneCreateFn)(const wchar_t* base, const wchar_t* path,
                                      void* init_data, void** out_handle);
#define XUI_SCENE_CREATE  ((XuiSceneCreateFn)0x418358d0)

typedef int (*SceneNavigateFn)(void* current_ctx, void* scene_handle);
#define SCENE_NAVIGATE  ((SceneNavigateFn)0x0001e5d8)

#define CURRENT_CTX_GLOBAL  ((void**)0x00097300)

static int nav_to_scene_by_name(const wchar_t* name) {
    void* h = NULL;
    HRESULT hr = XUI_SCENE_CREATE(L"gem://", name, NULL, &h);
    if (FAILED(hr) || h == NULL) return -1;
    return SCENE_NAVIGATE(*CURRENT_CTX_GLOBAL, h);
}

#define MSG_NAV_SOURCE_QUERY 0x18000022   /* engine asks "what element are we
                                              navigating from?" Return
                                              instance+0x18. */

extern "C" __declspec(dllexport)
HRESULT GemModHub_OnInit(GemModHubInstance* self) {
    if (!self) return -1;
    self->breadcrumb_elem = 0;
    self->manage_btn = 0;
    self->browse_btn = 0;
    self->settings_btn = 0;

    void* h = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &h, 0);
    self->breadcrumb_elem = (DWORD)h;

    h = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"manageButton", &h, 0);
    self->manage_btn = (DWORD)h;

    h = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"browseButton", &h, 0);
    self->browse_btn = (DWORD)h;

    h = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"settingsButton", &h, 0);
    self->settings_btn = (DWORD)h;
    return 0;
}

extern "C" __declspec(dllexport)
HRESULT GemModHub_OnMessage(GemModHubInstance* self, void* msg) {
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

    /* Engine query during scene transition: "what element are we
       navigating from?" Marketplace's handler writes instance+0x18 to
       the sub_payload's first word and returns 0. Matches exact bytes
       at GemMarketplaceScene OnMessage RVA 0x4ed68: msg payload at
       msg[4], write nav_source_elem to *m[4]. */
    if (msg_id == MSG_NAV_SOURCE_QUERY) {
        __try {
            DWORD* sub_payload = (DWORD*)m[4];
            if (sub_payload) {
                sub_payload[0] = self->nav_source_elem;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    if (msg_id == MSG_DATA_SOURCE && sub && sub_code == SUB_DS_SET_SEL) {
        const wchar_t* scene_name = NULL;
        if (target && target == self->manage_btn) {
            scene_name = L"ManageMods.xur";
        } else if (target && target == self->browse_btn) {
            scene_name = L"BrowseMods.xur";
        }
        /* settings button is not yet implemented; leave scene_name NULL so
           the tap falls through to the base. */

        if (scene_name) {
            /* Store the tapped button's element handle so the engine can
               read it via msg 0x18000022 to seed the destination scene's
               breadcrumbTransText/Image morph. Matches the exact
               sequence in GemMarketplaceScene's DS-set-sel handler
               (RE 0x4e848): set instance+0x18 = element handle, then
               navigate. */
            self->nav_source_elem = target;
            __try {
                nav_to_scene_by_name(scene_name);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        /* Unbound target or scene not implemented (browse/settings):
           fall through to base, a harmless no-op. */
    }

    HRESULT hr = 0;
    __try {
        hr = XUISCENE_ON_MESSAGE(self, msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = 0;
    }
    return hr;
}
