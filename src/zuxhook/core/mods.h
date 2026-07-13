#ifndef MODS_H
#define MODS_H

/* Top-level mod-runtime entrypoints.

   ModsApplyPhase1 runs from compositor.exe's ZUxHookInit. It reads
   \flash2\automation\mods\*\manifest.json + enabled.json, applies
   Layer 2/4 capabilities (gem_add_entry, xus_add_string, etc.), and
   writes composed gems atomically to \Windows\. Layer 5/6 capabilities
   (Phase 2) are silently skipped here; they're applied later by
   ModsApplyPhase2 from gemstone's own ZUxHookInit.

   Failures don't propagate: a bad mod produces log lines but leaves
   the rest of the device bootable. */

#include "mods_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

int ModsApplyPhase1(void);

/* Persist a mod's back-ref scope to
     \flash2\automation\mods\<mod_id>\backrefs.json
   so Phase 2 (in gemstone) can load values computed by Phase 1
   capabilities (xus_add_string assigns an index that
   inject_menu_entry needs). File: {"name":int,"name2":int,...} */
int ModsWriteBackRefs(Mod* m);

/* Load that file (if present) and pre-populate the mod's scope.
   Returns 0 on success or "file absent". */
int ModsLoadBackRefs(Mod* m, ModsArena* arena);

#ifdef __cplusplus
}
#endif

#endif
