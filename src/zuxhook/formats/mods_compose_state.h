#ifndef MODS_COMPOSE_STATE_H
#define MODS_COMPOSE_STATE_H

#include "mods_arena.h"
#include "mods_xuiz.h"

/* The composition's shape and its per-mod checkpoint, split out from
   mods_compose.h so both build without windows.h: mods_compose.h pulls in
   mods_manifest.h for ModAction, and the rollback logic needs none of that.
   The split is what lets the host tests cover it, and the rollback only ever
   runs when a mod fails, so it is otherwise never exercised. */

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

/* Gem mutations substitute arena-allocated buffers into ModsXuiz entries and
   nothing reaches \Windows\ until the flush, so discarding a half-applied mod
   is a matter of putting the entry tables back. Reloading the gem instead would
   revert every mod applied before this one. */
#define COMPOSE_CKPT_MAX_GEMS 16

typedef struct {
    int             gem_count;
    int             entry_count[COMPOSE_CKPT_MAX_GEMS];
    int             modified[COMPOSE_CKPT_MAX_GEMS];
    ModsXuizEntry*  entries[COMPOSE_CKPT_MAX_GEMS];
    int             valid;
} ComposeCheckpoint;

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot before applying a mod. Returns 0 on success; on failure `cp->valid`
   is 0 and the caller simply cannot roll that mod back. */
int  ModsComposeCheckpoint(ComposeState* st, ModsArena* arena,
                           ComposeCheckpoint* cp);

/* Restore the composition to the checkpoint, dropping any gem the mod loaded. */
void ModsComposeRollback(ComposeState* st, const ComposeCheckpoint* cp);

#ifdef __cplusplus
}
#endif

#endif
