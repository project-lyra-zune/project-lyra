#include "mods_curation.h"
#include "mods_toggles.h"   /* ModToggleIndexForKey / ModToggleGetQuickToggle */

#include <string.h>

typedef struct { char key[MOD_STATE_ID_LEN + 1]; } CurEntry;

static CurEntry g_cur[MOD_CURATION_MAX];
static int      g_count = 0;

void ModCurationClear(void) { g_count = 0; }

int ModCurationContains(const char* key) {
    int i;
    if (!key) return 0;
    for (i = 0; i < g_count; i++)
        if (strncmp(g_cur[i].key, key, MOD_STATE_ID_LEN) == 0) return 1;
    return 0;
}

void ModCurationAdd(const char* key) {
    int n;
    if (!key || g_count >= MOD_CURATION_MAX || ModCurationContains(key)) return;
    for (n = 0; n + 1 < (int)sizeof(g_cur[g_count].key) && key[n]; n++)
        g_cur[g_count].key[n] = key[n];
    g_cur[g_count].key[n] = 0;
    g_count++;
}

int ModCurationCount(void) { return g_count; }

const char* ModCurationGetKey(int i) {
    return (i >= 0 && i < g_count) ? g_cur[i].key : NULL;
}

int ModCurationVisibleCount(void) {
    int i, n = 0;
    for (i = 0; i < g_count; i++) {
        int ri = ModToggleIndexForKey(g_cur[i].key);
        if (ri < 0 || ModToggleGetQuickToggle(ri) == MOD_QT_NONE) continue;
        n++;
    }
    return n;
}

int ModCurationVisibleIndex(int row) {
    int i, n = 0;
    if (row < 0) return -1;
    for (i = 0; i < g_count; i++) {
        int ri = ModToggleIndexForKey(g_cur[i].key);
        if (ri < 0 || ModToggleGetQuickToggle(ri) == MOD_QT_NONE) continue;
        if (n == row) return ri;
        n++;
    }
    return -1;
}
