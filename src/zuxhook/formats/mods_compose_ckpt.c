#include <string.h>

#include "mods_compose_state.h"

int ModsComposeCheckpoint(ComposeState* st, ModsArena* arena,
                          ComposeCheckpoint* cp) {
    int i;
    cp->valid = 0;
    if (st->count > COMPOSE_CKPT_MAX_GEMS) return -1;
    cp->gem_count = st->count;
    for (i = 0; i < st->count; i++) {
        GemComp* g = st->gems[i];
        int n = g->xuiz.count;
        cp->entry_count[i] = n;
        cp->modified[i]    = g->modified;
        cp->entries[i]     = NULL;
        if (n > 0) {
            cp->entries[i] = (ModsXuizEntry*)ModsArenaAlloc(
                arena, (size_t)n * sizeof(ModsXuizEntry));
            if (!cp->entries[i]) return -1;
            memcpy(cp->entries[i], g->xuiz.entries,
                   (size_t)n * sizeof(ModsXuizEntry));
        }
    }
    cp->valid = 1;
    return 0;
}

void ModsComposeRollback(ComposeState* st, const ComposeCheckpoint* cp) {
    int i;
    if (!cp->valid) return;
    /* Gems this mod caused to load carry none of its edits once dropped. */
    if (st->count > cp->gem_count) st->count = cp->gem_count;
    for (i = 0; i < cp->gem_count && i < st->count; i++) {
        GemComp* g = st->gems[i];
        if (cp->entries[i] && cp->entry_count[i] > 0)
            memcpy(g->xuiz.entries, cp->entries[i],
                   (size_t)cp->entry_count[i] * sizeof(ModsXuizEntry));
        g->xuiz.count = cp->entry_count[i];
        g->modified   = cp->modified[i];
    }
}
