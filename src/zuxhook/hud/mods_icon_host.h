#ifndef MODS_ICON_HOST_H
#define MODS_ICON_HOST_H

#include "mods_list_model.h"   /* ModListSource for the context-list provider registry */

/* Runtime status-icon injection host.

   Adds the mod status icon to an existing scene's `iconGrid` at runtime; the
   on-disk scene XURs are left untouched.

   Trigger: hook xuidll!XuiSceneCreateEx (the universal scene-creation funnel,
   body 0x41835604) via gemstone IAT slot 0x000963f8. The exported name-based
   XuiSceneCreate (0x418358d0) reaches 0x41835604 by a direct internal branch (not
   the IAT), so our own scene creates won't re-enter the hook.

   On each created scene that carries an iconGrid, the proxy builds the icon with
   XuiSceneCreate("gem://ModIcon.xur") (the engine runs the full build pipeline,
   incl. the per-property apply a bare XuiCreateObject would skip), extracts the
   element, RemoveFromParent + AddChild into the grid, and expands + re-pins the
   grid at construction time. The element's controller (ModStatusIcon) drives the
   once-per-display column heal.

   Pass the host's XuiSceneCreateEx IAT slot: gemstone gemstone.exe 0x000963f8
   (NowPlaying); servicesd zhud_serv.dll 0x419de2c0 (playback HUD). The HUD
   iconGrid is layout-identical to NowPlaying, so the proxy/fragment/grid-heal
   are reused as-is. */
#ifdef __cplusplus
extern "C" {
#endif

/* XuiSceneCreateEx IAT slots, per host process. */
#define ICON_HOST_IAT_GEMSTONE   0x000963f8u
#define ICON_HOST_IAT_SERVICESD  0x419de2c0u   /* zhud_serv.dll import */

void ModsIconHostInstall(unsigned int iatSlot);

/* Re-render this process's mod status icons from the shared block. Called on the
   UI thread by the state-change drain in mods_state_event.c. */
void ModsIconOnStateChanged(void);

/* Clear the open-menu tracker once the navigated quick-toggle scene has been
   popped + destroyed (so the button can re-open it). Called each iteration of the
   servicesd UI-thread main loop (mods_state_event.c's MsgWait hook). The menu is
   NOT tied to HUD visibility; it persists until the user dismisses it. */
void ModsHudMenuTick(void);

/* Request dismissal of the open quick-toggle overlay (from the scene's tap-off
   handler). The actual RemoveFromParent + Destroy runs on the next ModsHudMenuTick,
   deferred, so the scene isn't torn down while its own message handler runs. */
void ModsHudMenuRequestDismiss(void);

/* Re-query the open quick-settings menu's rows (defined in the menu scene). Wired
   to the state-change drain so a row's status sub-label updates live, the way the
   native lists re-query on a notification. No-op if no menu is open. */
void ModsQuickSettingsLiveRefresh(void);

/* ── Context sub-list (long-press picker) ──────────────────────────────────
   A row whose key has a registered provider opens a ModContextListScene overlay
   on long-press, sourced by that provider. The general engine owns raising/
   dismissing the overlay; a mod owns the provider (rows + on-select). */

/* Register a context-list provider for a quick-settings row key. Called at boot. */
void ModsHudContextRegister(const char* key, const ModListSource* src);

/* If `key` has a registered provider, raise the context list over the HUD content
   host sourced by it. Returns 1 if handled (a provider matched + opened), else 0. */
int  ModsHudContextOpenForKey(const char* key);

/* Request dismissal of the open context overlay (from the context scene's tap/cancel
   handler). The slide-out + teardown runs on the next tick, not inline. */
void ModsHudContextRequestDismiss(void);

/* Injected HUD menu-button tap handler, called from the 0x419c8b50 dispatcher
   detour (mods_phase2.c enable_debug_button_launch). scene = the dispatcher's
   scene instance pointer, tapped = the engine's hit-tested element handle. When
   tapped is our injected "•••" button it launches the quick-toggle menu and
   returns 0 (handled; caller rejoins native flow at the post-dispatch
   continuation); otherwise returns nonzero to resume native dispatch. */
int ModsHudMenuButtonTap(unsigned int scene, unsigned int tapped);

#ifdef __cplusplus
}
#endif

#endif /* MODS_ICON_HOST_H */
