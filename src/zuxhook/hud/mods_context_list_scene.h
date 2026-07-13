#ifndef MODS_CONTEXT_LIST_SCENE_H
#define MODS_CONTEXT_LIST_SCENE_H

#include "mods_list_model.h"

/* General modkit sub-list scene (HUD/zhud), the record-array list raised as an
   overlay from a long-pressed row (e.g. a quick-settings row whose key has a
   registered context provider). It renders rows from a bound ModListSource and
   returns the tapped row to that source's on_tap; it knows nothing about what the
   rows represent (devices, outputs, …). One context list open at a time.

   The opener calls ModContextListBind(src) before raising gem://ContextList.xur;
   OnInit/OnMessage read the bound source. Selecting a row, or tapping off, closes
   the overlay (via the icon-host's deferred dismiss). */

#ifdef __cplusplus
extern "C" {
#endif

void ModContextListBind(const ModListSource* src);

/* Re-snapshot the bound source + re-render the open list in place (for a live
   provider whose rows change while the picker is open). No-op if none open. */
void ModContextListLiveRefresh(void);

/* Current row count of the bound source's snapshot (0 if none). The overlay host
   reads it after a live refresh to re-size the overlay when the list grows. */
int ModContextListRowCount(void);

#ifdef __cplusplus
}
#endif

#endif /* MODS_CONTEXT_LIST_SCENE_H */
