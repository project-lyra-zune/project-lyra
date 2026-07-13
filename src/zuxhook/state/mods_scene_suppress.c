#include "mods_scene_suppress.h"
#include "mods_state_block.h"

#define SCENE_SUPPRESS_MAX      16
#define SCENE_SUPPRESS_URI_LEN  80

typedef struct {
    wchar_t uri[SCENE_SUPPRESS_URI_LEN];
    char    key[MOD_STATE_ID_LEN + 1];     /* "owner/id", NUL-terminated */
} SuppressEntry;

static SuppressEntry g_entries[SCENE_SUPPRESS_MAX];
static int           g_count = 0;

static int uri_equals(const wchar_t* a, const wchar_t* b) {
    int i;
    for (i = 0; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static int key_equals(const char* a, const char* b) {
    int i;
    for (i = 0; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

int ModSceneSuppressAdd(const wchar_t* uri, const char* setting_key) {
    SuppressEntry* e;
    int i, j;
    if (!uri || !setting_key) return -1;
    for (i = 0; i < g_count; i++)
        if (uri_equals(g_entries[i].uri, uri) &&
            key_equals(g_entries[i].key, setting_key))
            return 0;                       /* already registered */
    if (g_count >= SCENE_SUPPRESS_MAX) return -1;
    e = &g_entries[g_count];
    for (j = 0; uri[j] && j < SCENE_SUPPRESS_URI_LEN - 1; j++) e->uri[j] = uri[j];
    e->uri[j] = 0;
    for (j = 0; setting_key[j] && j < MOD_STATE_ID_LEN; j++) e->key[j] = setting_key[j];
    e->key[j] = 0;
    g_count++;
    return 0;
}

int ModSceneSuppressActive(const wchar_t* uri) {
    int i;
    if (!uri) return 0;
    for (i = 0; i < g_count; i++)
        if (uri_equals(g_entries[i].uri, uri) &&
            ModStateGetState(g_entries[i].key) > 0)
            return 1;
    return 0;
}
