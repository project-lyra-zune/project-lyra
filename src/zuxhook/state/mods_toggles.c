#include "mods_toggles.h"

#include <string.h>

typedef struct {
    ModToggleKind  kind;
    ModQuickToggle qt;
    int     persist;                 /* 1 = value saved/restored across boots; 0 = transient */
    char    key[MOD_TOGGLE_KEY_LEN + 1];
    wchar_t label[MOD_TOGGLE_LABEL_LEN + 1];
    wchar_t icon[MOD_TOGGLE_ICON_LEN + 1];
    wchar_t states[MOD_TOGGLE_STATE_MAX][MOD_TOGGLE_STATE_LEN + 1];
    int     state_count;
    char    status_key[MOD_TOGGLE_KEY_LEN + 1];   /* row sub-label reads this status slot; empty = none */
} Entry;

static Entry g_entries[MOD_TOGGLE_MAX];
static int   g_count = 0;

static void copy_key(char out[MOD_TOGGLE_KEY_LEN + 1], const char* src) {
    int i;
    for (i = 0; i < MOD_TOGGLE_KEY_LEN && src[i]; i++) out[i] = src[i];
    out[i] = 0;
}

static void widen_label(wchar_t out[MOD_TOGGLE_LABEL_LEN + 1], const char* utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, MOD_TOGGLE_LABEL_LEN);
    if (n <= 0) {
        /* ASCII fallback if the UTF-8 conversion is unavailable. */
        int i;
        for (i = 0; i < MOD_TOGGLE_LABEL_LEN && utf8[i]; i++) out[i] = (wchar_t)(unsigned char)utf8[i];
        out[i] = 0;
        return;
    }
    out[MOD_TOGGLE_LABEL_LEN] = 0;
}

static void set_state_w(Entry* e, int idx, const wchar_t* w) {
    int i;
    if (!e || idx < 0 || idx >= MOD_TOGGLE_STATE_MAX || !w) return;
    for (i = 0; i < MOD_TOGGLE_STATE_LEN && w[i]; i++) e->states[idx][i] = w[i];
    e->states[idx][i] = 0;
    if (idx + 1 > e->state_count) e->state_count = idx + 1;
}

/* Reset to the kind's default state sub-labels. Binary = Disabled/Enabled
   (overridable via ModToggleSetStateLabel); other kinds start empty (declared). */
static void init_state_defaults(Entry* e) {
    e->state_count = 0;
    if (e->kind == MOD_TOGGLE_BINARY) {
        set_state_w(e, 0, L"Disabled");
        set_state_w(e, 1, L"Enabled");
    }
}

void ModTogglesRegister(ModToggleKind kind, const char* key,
                        const char* label_utf8, ModQuickToggle qt, int persist) {
    int i;
    if (!key || !label_utf8) return;
    for (i = 0; i < g_count; i++) {
        if (strncmp(g_entries[i].key, key, MOD_TOGGLE_KEY_LEN) == 0) {
            g_entries[i].kind    = kind;
            g_entries[i].qt      = qt;
            g_entries[i].persist = persist;
            widen_label(g_entries[i].label, label_utf8);
            g_entries[i].icon[0] = 0;
            g_entries[i].status_key[0] = 0;
            init_state_defaults(&g_entries[i]);
            return;
        }
    }
    if (g_count >= MOD_TOGGLE_MAX) return;
    g_entries[g_count].kind    = kind;
    g_entries[g_count].qt      = qt;
    g_entries[g_count].persist = persist;
    copy_key(g_entries[g_count].key, key);
    widen_label(g_entries[g_count].label, label_utf8);
    g_entries[g_count].icon[0] = 0;
    g_entries[g_count].status_key[0] = 0;
    init_state_defaults(&g_entries[g_count]);
    g_count++;
}

void ModToggleSetStateLabel(const char* key, int state_idx, const char* label_utf8) {
    int i, n;
    wchar_t tmp[MOD_TOGGLE_STATE_LEN + 1];
    if (!key || !label_utf8) return;
    for (i = 0; i < g_count; i++) {
        if (strncmp(g_entries[i].key, key, MOD_TOGGLE_KEY_LEN) != 0) continue;
        n = MultiByteToWideChar(CP_UTF8, 0, label_utf8, -1, tmp, MOD_TOGGLE_STATE_LEN);
        if (n <= 0) {
            int k;
            for (k = 0; k < MOD_TOGGLE_STATE_LEN && label_utf8[k]; k++)
                tmp[k] = (wchar_t)(unsigned char)label_utf8[k];
            tmp[k] = 0;
        } else {
            tmp[MOD_TOGGLE_STATE_LEN] = 0;
        }
        set_state_w(&g_entries[i], state_idx, tmp);
        return;
    }
}

void ModToggleSetQuickIcon(const char* key, const wchar_t* icon_path) {
    int i, k;
    if (!key || !icon_path) return;
    for (i = 0; i < g_count; i++) {
        if (strncmp(g_entries[i].key, key, MOD_TOGGLE_KEY_LEN) != 0) continue;
        for (k = 0; k < MOD_TOGGLE_ICON_LEN && icon_path[k]; k++)
            g_entries[i].icon[k] = icon_path[k];
        g_entries[i].icon[k] = 0;
        return;
    }
}

int ModTogglesCount(void) { return g_count; }

ModToggleKind ModToggleGetKind(int i) {
    return (i >= 0 && i < g_count) ? g_entries[i].kind : MOD_TOGGLE_BINARY;
}
const char* ModToggleGetKey(int i) {
    return (i >= 0 && i < g_count) ? g_entries[i].key : 0;
}
const wchar_t* ModToggleGetLabel(int i) {
    return (i >= 0 && i < g_count) ? g_entries[i].label : 0;
}
ModQuickToggle ModToggleGetQuickToggle(int i) {
    return (i >= 0 && i < g_count) ? g_entries[i].qt : MOD_QT_NONE;
}
int ModToggleGetPersist(int i) {
    return (i >= 0 && i < g_count) ? g_entries[i].persist : 1;
}
const wchar_t* ModToggleGetQuickIcon(int i) {
    if (i < 0 || i >= g_count || !g_entries[i].icon[0]) return 0;
    return g_entries[i].icon;
}
int ModToggleIndexForKey(const char* key) {
    int i;
    if (!key) return -1;
    for (i = 0; i < g_count; i++)
        if (strncmp(g_entries[i].key, key, MOD_TOGGLE_KEY_LEN) == 0) return i;
    return -1;
}
int ModToggleGetStateCount(int i) {
    return (i >= 0 && i < g_count) ? g_entries[i].state_count : 0;
}
const wchar_t* ModToggleGetStateLabel(int i, int state_idx) {
    if (i < 0 || i >= g_count) return 0;
    if (state_idx < 0 || state_idx >= g_entries[i].state_count) return 0;
    return g_entries[i].states[state_idx];
}

void ModToggleSetStatusKey(const char* key, const char* status_key) {
    int i;
    if (!key || !status_key) return;
    for (i = 0; i < g_count; i++) {
        if (strncmp(g_entries[i].key, key, MOD_TOGGLE_KEY_LEN) != 0) continue;
        copy_key(g_entries[i].status_key, status_key);
        return;
    }
}

const char* ModToggleGetStatusKey(int i) {
    if (i < 0 || i >= g_count || !g_entries[i].status_key[0]) return 0;
    return g_entries[i].status_key;
}
