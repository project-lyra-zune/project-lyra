#ifndef MODS_SCENE_SUPPRESS_H
#define MODS_SCENE_SUPPRESS_H

#include <windows.h>

/* Generic XUI scene-suppression registry.

   A `suppress_scene` manifest action registers a (scene-URI, gating setting-key)
   pair here at apply time; the XuiSceneCreateEx proxy (mods_icon_host.c) queries
   it on every scene create and fails the create for any registered scene whose
   setting is currently active. The host's scene navigator then takes its existing
   create-failure path and silently skips the scene; the underlying state (e.g.
   the device's connected/sync state) is untouched, only the scene is dropped.

   Process-local: each host (gemstone / servicesd) populates its own registry from
   the actions routed to it, and its own proxy reads it. The gating value itself is
   the cross-process ModStateBlock slot named by `setting_key` ("owner/id"). */

#ifdef __cplusplus
extern "C" {
#endif

/* Register a scene URI to suppress while `setting_key` (an "owner/id"
   ModStateBlock key) is active. Idempotent on (uri, setting_key). Returns 0 on
   success, -1 if the registry is full. */
int ModSceneSuppressAdd(const wchar_t* uri, const char* setting_key);

/* 1 if `uri` is registered and its gating setting is currently active; else 0. */
int ModSceneSuppressActive(const wchar_t* uri);

#ifdef __cplusplus
}
#endif

#endif /* MODS_SCENE_SUPPRESS_H */
