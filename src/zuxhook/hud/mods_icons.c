#include "mods_icons.h"

#include <string.h>

typedef struct {
    char        token[MOD_ICON_TOKEN_LEN + 1];   /* bare id -> modicon_<token> */
    char        key[MOD_STATE_ID_LEN + 1];        /* owner/id state key */
    char        scene[MOD_ICON_SCENE_LEN + 1];
    int         nstates;
    signed char frame_of[MOD_ICON_STATES_MAX];    /* frame elem index per state; -1 = hidden */
    DWORD       tint_of[MOD_ICON_STATES_MAX];      /* per-state tint; 0xFFFFFFFF = none */
} IconEntry;

static IconEntry g_icons[MOD_ICON_MAX];
static int       g_count = 0;

static void cpy(char* out, int cap, const char* src) {
    int i;
    for (i = 0; i + 1 < cap && src && src[i]; i++) out[i] = src[i];
    out[i] = 0;
}

static void copy_state_maps(IconEntry* e, const signed char* frame_of,
                            const DWORD* tint_of, int nstates) {
    int s;
    if (nstates < 0) nstates = 0;
    if (nstates > MOD_ICON_STATES_MAX) nstates = MOD_ICON_STATES_MAX;
    e->nstates = nstates;
    for (s = 0; s < MOD_ICON_STATES_MAX; s++) {
        e->frame_of[s] = (s < nstates && frame_of) ? frame_of[s] : -1;
        e->tint_of[s]  = (s < nstates && tint_of)  ? tint_of[s]  : 0xFFFFFFFFu;
    }
}

void ModIconsRegister(const char* token, const char* key, const char* scene,
                      const signed char* frame_of, const DWORD* tint_of, int nstates) {
    int i;
    if (!token || !key || !scene) return;
    for (i = 0; i < g_count; i++) {
        if (strncmp(g_icons[i].key, key, MOD_STATE_ID_LEN) == 0) {
            cpy(g_icons[i].token, (int)sizeof(g_icons[i].token), token);
            cpy(g_icons[i].scene, (int)sizeof(g_icons[i].scene), scene);
            copy_state_maps(&g_icons[i], frame_of, tint_of, nstates);
            return;
        }
    }
    if (g_count >= MOD_ICON_MAX) return;
    cpy(g_icons[g_count].token, (int)sizeof(g_icons[g_count].token), token);
    cpy(g_icons[g_count].key,   (int)sizeof(g_icons[g_count].key),   key);
    cpy(g_icons[g_count].scene, (int)sizeof(g_icons[g_count].scene), scene);
    copy_state_maps(&g_icons[g_count], frame_of, tint_of, nstates);
    g_count++;
}

int ModIconsCount(void) { return g_count; }
const char* ModIconGetToken(int i) { return (i >= 0 && i < g_count) ? g_icons[i].token : 0; }
const char* ModIconGetKey(int i)   { return (i >= 0 && i < g_count) ? g_icons[i].key   : 0; }
const char* ModIconGetScene(int i) { return (i >= 0 && i < g_count) ? g_icons[i].scene : 0; }

static const IconEntry* icon_by_key(const char* key) {
    int i;
    if (!key) return 0;
    for (i = 0; i < g_count; i++)
        if (strncmp(g_icons[i].key, key, MOD_STATE_ID_LEN) == 0) return &g_icons[i];
    return 0;
}

int ModIconStateFrame(const char* key, int state) {
    const IconEntry* e = icon_by_key(key);
    if (!e || state < 0 || state >= e->nstates) return -1;
    return (int)e->frame_of[state];
}

DWORD ModIconStateTint(const char* key, int state) {
    const IconEntry* e = icon_by_key(key);
    if (!e || state < 0 || state >= e->nstates) return 0xFFFFFFFFu;
    return e->tint_of[state];
}

typedef struct {
    char  element[MOD_ICON_TINT_ELEM_LEN + 1];
    char  key[MOD_STATE_ID_LEN + 1];
    DWORD argb;
} TintEntry;

static TintEntry g_tints[MOD_ICON_TINT_MAX];
static int       g_tint_count = 0;

void ModIconTintRegister(const char* element, const char* key, DWORD argb) {
    int i;
    if (!element || !key) return;
    for (i = 0; i < g_tint_count; i++) {
        if (strncmp(g_tints[i].key, key, MOD_STATE_ID_LEN) == 0) {
            cpy(g_tints[i].element, (int)sizeof(g_tints[i].element), element);
            g_tints[i].argb = argb;
            return;
        }
    }
    if (g_tint_count >= MOD_ICON_TINT_MAX) return;
    cpy(g_tints[g_tint_count].element, (int)sizeof(g_tints[g_tint_count].element), element);
    cpy(g_tints[g_tint_count].key,     (int)sizeof(g_tints[g_tint_count].key),     key);
    g_tints[g_tint_count].argb = argb;
    g_tint_count++;
}

int ModIconTintCount(void) { return g_tint_count; }
const char* ModIconTintElement(int i) { return (i >= 0 && i < g_tint_count) ? g_tints[i].element : 0; }
const char* ModIconTintKey(int i)     { return (i >= 0 && i < g_tint_count) ? g_tints[i].key     : 0; }
DWORD       ModIconTintColor(int i)   { return (i >= 0 && i < g_tint_count) ? g_tints[i].argb    : 0; }
