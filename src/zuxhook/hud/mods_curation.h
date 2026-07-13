#ifndef MODS_CURATION_H
#define MODS_CURATION_H

#include "mods_state_block.h"   /* MOD_STATE_ID_LEN */

/* The user's curated, ordered list of quick-toggle setting keys shown in the
   HUD quick menu. The VALUE side lives in
   ModStateBlock and mod-settings.json's "values"; this is the "quick_toggles"
   side, persisted in the same file. Seeded on first boot from settings declared
   quick_toggle:"default"; the settings page (later) edits it. Process-local;
   the HUD menu host (servicesd) reads it. */
#define MOD_CURATION_MAX  16

#ifdef __cplusplus
extern "C" {
#endif

void        ModCurationClear(void);
void        ModCurationAdd(const char* key);    /* append if not already present */
int         ModCurationCount(void);
const char* ModCurationGetKey(int i);            /* owner/id key, NULL if out of range */
int         ModCurationContains(const char* key);

/* The VISIBLE quick-toggle set: curated keys that are still quick-eligible in
   the registry, in curation order. Single source of truth for both the HUD menu
   builder and the quick-settings list scene (so a settings-page-only toggle
   never leaks into the quick menu). */
int         ModCurationVisibleCount(void);
int         ModCurationVisibleIndex(int row);   /* registry index of the row-th visible toggle; -1 if OOR */

#ifdef __cplusplus
}
#endif

#endif /* MODS_CURATION_H */
