#ifndef MODS_XUR_BUILDER_H
#define MODS_XUR_BUILDER_H

#include <windows.h>
#include "mods_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOD_ICON_MAX_FRAMES 8

/* Narrow XUR v5 writer for generated mod status icons. It intentionally owns
   only the object/property subset needed by the icon host:
     fragment: XuiCanvas -> XuiScene -> XuiControl -> XuiControl(frame{k})...
     visual:   XuiCanvas -> XuiVisual -> XuiImage
   This is device-side generation of the same canonical format xuihelper-zune
   writes, not a patch over precompiled bytes. */

/* Build the icon fragment with one `frame{k}` child per state. `visual_ids`
   is indexed by state (0..nstates-1); a NULL entry means that state shows no
   icon (hidden, e.g. an "off" state), so no frame element is emitted for it.
   The element id is "modicon_<slot>"; each frame references its state's visual
   by name. At least one non-NULL state is required. */
int ModsBuildStatusIconFragmentXur(ModsArena* arena,
                                   const char* slot,
                                   const char* const* visual_ids,
                                   int nstates,
                                   const unsigned char** out,
                                   int* out_len);

int ModsBuildStatusIconVisualXur(ModsArena* arena,
                                 const char* visual_id,
                                 const wchar_t* image_url,
                                 const unsigned char** out,
                                 int* out_len);

#ifdef __cplusplus
}
#endif

#endif
