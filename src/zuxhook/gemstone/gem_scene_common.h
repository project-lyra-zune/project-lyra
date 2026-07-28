#ifndef GEM_SCENE_COMMON_H
#define GEM_SCENE_COMMON_H

#include <windows.h>

/* Shared scaffolding for the gemstone mod-UI scenes (gem_mod_hub /
   gem_mod_manager / gem_mod_detail / gem_mods_list_content_scene): the two
   xuidll/gemstone entry points they all call, the msg-0xe data-source
   sub-struct, and the message codes common to more than one scene. */

/* xuidll XuiElementGetDescendantById - resolve a named descendant element. */
typedef HRESULT (*XuiElementGetDescendantByIdFn)(void* parent,
                                                  const wchar_t* id,
                                                  void** out,
                                                  int flags);
#define XUI_GET_DESC_BY_ID  ((XuiElementGetDescendantByIdFn)0x0006afec)

typedef HRESULT (*SetLabelTextFn)(void* elem, const wchar_t* text);
#define SET_LABEL_TEXT  ((SetLabelTextFn)0x00038434)
typedef int (*SetShowFn)(void* elem, int show);
#define SET_SHOW  ((SetShowFn)0x00058860)

/* Guarded: a skin need not carry every element, and one it omits resolves to 0. */
static void render_label(void* elem, const wchar_t* text) {
    if (!elem || !text) return;
    __try { SET_LABEL_TEXT(elem, text); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void show_elem(DWORD e, int on) {
    if (e) { __try { SET_SHOW((void*)e, on ? 1 : 0); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
}

/* Base XuiScene message handler (gemstone+0x653f4), the fall-through a scene
   subclass invokes for messages it does not handle itself. */
typedef HRESULT (*MessageHandlerFn)(void* this_, void* msg);
#define XUISCENE_ON_MESSAGE  ((MessageHandlerFn)0x000653f4)

/* Sub-struct carried by msg 0xe (MSG_DATA_SOURCE); matches GemSettingScene +
   GemMarketplaceScene. */
struct DataSourceSubStruct {
    DWORD sub_code;      /* +0x00 */
    DWORD target_elem;   /* +0x04 - tapped element handle */
    DWORD size_hint;     /* +0x08 */
    DWORD output_area;   /* +0x0c */
};

#define MSG_DATA_SOURCE  0xe
#define MSG_INIT_BIND    0x13
#define SUB_DS_SET_SEL   0x01     /* per-element tap dispatch */
#define SUB_DS_GET_ITEM  0x3e8
#define SUB_DS_COUNT     0x3eb

#endif
