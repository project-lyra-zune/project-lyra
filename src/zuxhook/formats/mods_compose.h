#ifndef MODS_COMPOSE_H
#define MODS_COMPOSE_H

#include "mods_arena.h"
#include "mods_manifest.h"
#include "mods_xuiz.h"

/* In-memory composition of .gem files plus Layer 2/4 capability dispatch.

   A GemComp is lazy-loaded on the first capability that touches its
   gem. We read \Windows\<gem> (RAM-shadow, freshly ROM-reset at boot),
   decode the XUIZ container, and hold the decoded ModsXuiz alongside
   the source buffer (entries point into it).

   Mutations (append entry, replace entry data, etc.) substitute new
   arena-allocated buffers into the xuiz entries. At end-of-Phase-1,
   each modified gem is re-encoded and written back to \Windows\<gem>
   (write .new, delete the original, then MoveFileW the .new into place;
   WinCE 6 has no MoveFileExW REPLACE_EXISTING). */

typedef struct {
    char           basename[64];      /* e.g. "scenes_standard.gem" */
    int            modified;
    ModsXuiz       xuiz;
} GemComp;

typedef struct {
    GemComp**  gems;                  /* heap-of-pointers; each owned by arena */
    int        count;
    int        cap;
} ComposeState;

#ifdef __cplusplus
extern "C" {
#endif

/* Apply one action. Returns:
     0  - applied successfully (Phase 1 capability)
     1  - skipped (capability belongs to Phase 2; not an error)
    -1  - applied and failed (caller decides whether to abort or skip mod) */
int ModsComposeApplyAction(ComposeState* st, ModsArena* arena, ModAction* a);

/* Re-encode each modified gem and write to \Windows\<gem> atomically.
   Returns 0 on success; -1 if any write failed. Per-gem failures are
   logged. */
int ModsComposeFlushAll(ComposeState* st, ModsArena* arena);

#ifdef __cplusplus
}
#endif

#endif
