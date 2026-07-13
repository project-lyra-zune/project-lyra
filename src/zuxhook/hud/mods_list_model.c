#include "mods_list_model.h"

void ModListModelPopulate(ModListModel* m, const ModListSource* src) {
    int i, n;
    if (!m) return;
    if (!src || !src->count || !src->fill) { m->count = 0; return; }
    n = src->count(src->ctx);
    if (n < 0) n = 0;
    if (n > MODLIST_MAX_ROWS) n = MODLIST_MAX_ROWS;
    for (i = 0; i < n; i++) {
        ModListRow* r = &m->rows[i];
        r->main[0] = 0;
        r->sub[0]  = 0;
        r->icon[0] = 0;
        r->key[0]  = 0;
        r->user    = 0;
        src->fill(src->ctx, i, r);
    }
    m->count = n;
}

/* msg 0x7de - ListInvalidate_dataChanged. Forces the bound list to re-query
   its data source (count + get-text/get-image for visible rows) and re-render
   in place. Confirmed equal to gemstone 0x58890 (same builder shape, same msg
   id + payload {0,1}). */
typedef int (*ListDataChangedFn)(void* list_element, int a, int b);
#define LIST_DATA_CHANGED  ((ListDataChangedFn)0x419bd0f4u)

void ModListRefresh(void* list_element) {
    if (!list_element) return;
    __try { LIST_DATA_CHANGED(list_element, 0, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
