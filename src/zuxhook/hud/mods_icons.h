#ifndef MODS_ICONS_H
#define MODS_ICONS_H

#include <windows.h>
#include "mods_state_block.h"   /* MOD_STATE_ID_LEN - the state-key width */

/* Registry of mod status icons declared via the add_status_icon manifest action.
   The icon-host injects each registered icon into the iconGrid (gemstone
   NowPlaying + servicesd HUD). Process-local. Each entry carries two distinct
   strings:

     - token: the bare setting id (ASCII, /-free). The fragment's element id is
       "modicon_<token>" by convention, so the token stays a clean DOM locator
       and the authored .xur is never namespaced.
     - key:   the namespaced state key owner/id. The injected element's
       controller reads its live state from ModStateBlock under this key (via the
       inject-time front map), so a toggle and an icon that name the same id under
       one owner converge on the same slot.

   Registration order fixes each icon's grid column. */
#define MOD_ICON_MAX         8
#define MOD_ICON_TOKEN_LEN   16   /* bare setting id; builds modicon_<token> */
#define MOD_ICON_SCENE_LEN   48
#define MOD_ICON_STATES_MAX  8    /* per-state tint colours (status state count) */
/* the key field uses MOD_STATE_ID_LEN (the ModStateBlock slot-key width) */

/* Registry of element tints declared via the tint_element manifest action. A tint
   recolors an EXISTING authored scene element (and its subtree) while a state slot
   is active (e.g. the native WifiIcon orange while "Keep WiFi on" is demanded).
   The recolor is a render-time colour multiply (mods_ui_tint): it preserves the
   element's detail (the wifi strength bars stay visible), needs no assets, and
   works on any element. The icon-host resolves each element by id per scene-create
   and drives ModUiTintSet/Clear on scene-create and state change. */
#define MOD_ICON_TINT_MAX       4
#define MOD_ICON_TINT_ELEM_LEN  32    /* authored element id, e.g. "wifiIcon" */

#ifdef __cplusplus
extern "C" {
#endif

/* Register one icon. token is the bare setting id (builds modicon_<token>); key
   is the namespaced state key the controller reads via ModStateGetState; scene
   is the gem entry of the icon fragment. Each state maps to a frame element
   (frame_of[state], -1 = hidden) and a multiplicative tint (tint_of[state],
   0xFFFFFFFF = none). Several states can share one frame with different tints
   (recolour one image) or each have its own frame (distinct images). Dedup by
   key. */
void        ModIconsRegister(const char* token, const char* key, const char* scene,
                             const signed char* frame_of, const DWORD* tint_of, int nstates);
int         ModIconsCount(void);
const char* ModIconGetToken(int i);   /* bare id (DOM token), NULL if out of range */
const char* ModIconGetKey(int i);     /* owner/id state key, NULL if out of range */
const char* ModIconGetScene(int i);   /* gem entry name, NULL if out of range */
/* Per-state appearance for the icon whose state key == `key`. ModIconStateFrame
   returns the frame element index to show (-1 = hide all); ModIconStateTint the
   tint to apply (0xFFFFFFFF = none). state out of range => hidden/none. */
int         ModIconStateFrame(const char* key, int state);
DWORD       ModIconStateTint(const char* key, int state);

/* Register one tint. element is the authored scene element id ("wifiIcon"); key is
   the namespaced owner/id state key gating it (tint on iff ModStateGetActive(key));
   argb is the multiplicative tint colour (0xAARRGGBB). Dedup by key. */
void        ModIconTintRegister(const char* element, const char* key, DWORD argb);
int         ModIconTintCount(void);
const char* ModIconTintElement(int i);  /* authored element id, NULL if out of range */
const char* ModIconTintKey(int i);      /* owner/id state key, NULL if out of range */
DWORD       ModIconTintColor(int i);    /* 0xAARRGGBB, 0 if out of range */

#ifdef __cplusplus
}
#endif

#endif /* MODS_ICONS_H */
