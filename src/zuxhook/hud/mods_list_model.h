#ifndef MODS_LIST_MODEL_H
#define MODS_LIST_MODEL_H

#include <windows.h>

#include "mods_state_block.h"   /* MOD_STATE_ID_LEN */

/* Reusable record-array list model for HUD list scenes (servicesd/zhud).

   This mirrors the native HudNetworkListScene pattern: the scene owns a
   per-row record array (the network list keeps it at scene+0x20), the list's
   data-source queries (count / get-text / get-image) read from it, and on a
   change the array is re-populated and the list is told to re-query so it
   re-renders IN PLACE, so rows update/appear/disappear without a rebuild.

   The row source is pluggable so the same scene machinery backs different
   lists: the quick-toggle registry today, async device discovery (Chromecast,
   nearby devices) later. A discovery event is just another caller of
   "re-populate + ModListRefresh". */

#define MODLIST_MAX_ROWS  16
#define MODLIST_TEXT_LEN  48
#define MODLIST_ICON_LEN  160
#define MODLIST_KEY_LEN   (MOD_STATE_ID_LEN + 1)

/* One rendered row - a snapshot the scene's get-text/get-image read from. */
typedef struct {
    wchar_t        main[MODLIST_TEXT_LEN];   /* assoc 0 - primary label */
    wchar_t        sub[MODLIST_TEXT_LEN];    /* assoc 1 - status sub-label */
    wchar_t        icon[MODLIST_ICON_LEN];   /* image path/ref; empty = none */
    char           key[MODLIST_KEY_LEN];     /* identity for the source's action */
    DWORD          user;                     /* source-defined (e.g. registry index) */
} ModListRow;

/* Pluggable row source. `ctx` is passed back to each callback so a source can
   carry state. fill() writes into a zeroed ModListRow. on_tap() performs the
   row's action (flip a toggle, connect to a device, …). on_open() (optional, may
   be NULL) fires once when a context picker backed by this source opens, letting
   a dynamic source kick off a refresh (e.g. a device scan) before the first
   query. */
typedef struct {
    int  (*count)(void* ctx);
    void (*fill)(void* ctx, int idx, ModListRow* out);
    void (*on_tap)(void* ctx, int idx);
    void* ctx;
    void (*on_open)(void* ctx);
} ModListSource;

typedef struct {
    ModListRow rows[MODLIST_MAX_ROWS];
    int        count;
} ModListModel;

#ifdef __cplusplus
extern "C" {
#endif

/* Re-snapshot the model from the source (each row zeroed before fill). */
void ModListModelPopulate(ModListModel* m, const ModListSource* src);

/* In-place re-render: ListInvalidate_dataChanged (msg 0x7de), the
   device-validated re-query used by the native network list and our shipped
   gemstone list scene. zhud VA 0x419bd0f4 == gemstone 0x58890; both build
   msg 0x7de with payload {0,1}. Call after re-populating the model. */
void ModListRefresh(void* list_element);

#ifdef __cplusplus
}
#endif

#endif /* MODS_LIST_MODEL_H */
