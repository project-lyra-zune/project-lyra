#include "mods_list_channel.h"

#include <string.h>

/* Section + scan-event names are derived from the setting key so both sides
   compute them identically from one source of truth (the key) with no extra ids
   to keep in sync. '/' -> '_' keeps the name a legal object name. */
#define CH_SECTION_PREFIX  L"zune-mod-listch-v2-"
#define CH_SCAN_PREFIX     L"zune-mod-listch-scan-"
#define CH_NAME_MAX        96

/* One channel per registered picker key. A handful at most (one per mod that
   declares a context), so a small linear cache is right-sized. */
#define CH_CACHE_MAX  8
typedef struct {
    char                 key[MOD_STATE_ID_LEN + 1];
    ModListChannelBlock* block;
    HANDLE               scan_evt;
} ChannelSlot;
static ChannelSlot g_cache[CH_CACHE_MAX];
static int         g_cache_n = 0;

static int key_eq(const char* a, const char* b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static void derive_name(wchar_t* out, const wchar_t* prefix, const char* key) {
    int j = 0, i;
    for (i = 0; prefix[i] && j < CH_NAME_MAX - 1; i++) out[j++] = prefix[i];
    for (i = 0; key[i] && j < CH_NAME_MAX - 1; i++)
        out[j++] = (key[i] == '/') ? L'_' : (wchar_t)(unsigned char)key[i];
    out[j] = 0;
}

static ChannelSlot* find_slot(const char* key) {
    int i;
    for (i = 0; i < g_cache_n; i++)
        if (key_eq(g_cache[i].key, key)) return &g_cache[i];
    return NULL;
}

/* Allocate + map a channel slot for `key`. CE6 has no OpenFileMapping; a named
   CreateFileMappingW attaches to the existing section or creates it. The channel
   is additive (no seed authority): whoever maps first zero-inits `version`. */
static ChannelSlot* map_slot(const char* key) {
    ChannelSlot* s = find_slot(key);
    wchar_t name[CH_NAME_MAX];
    HANDLE  h;
    int     i;
    if (s) return s;
    if (g_cache_n >= CH_CACHE_MAX) return NULL;
    s = &g_cache[g_cache_n];
    for (i = 0; i < MOD_STATE_ID_LEN && key[i]; i++) s->key[i] = key[i];
    s->key[i] = 0;
    s->block = NULL;
    s->scan_evt = NULL;

    derive_name(name, CH_SECTION_PREFIX, key);
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                           sizeof(ModListChannelBlock), name);
    if (h) {
        int created = (GetLastError() != ERROR_ALREADY_EXISTS);
        s->block = (ModListChannelBlock*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (s->block && created && s->block->version == 0) s->block->version = 1;
    }
    derive_name(name, CH_SCAN_PREFIX, key);
    s->scan_evt = CreateEventW(NULL, FALSE, FALSE, name);   /* auto-reset, shared by name */

    g_cache_n++;
    return s;
}

ModListChannelBlock* ModListChannelMap(const char* setting_key) {
    ChannelSlot* s;
    if (!setting_key) return NULL;
    s = map_slot(setting_key);
    return s ? s->block : NULL;
}

void ModListChannelSignalScan(const char* setting_key) {
    ChannelSlot* s;
    if (!setting_key) return;
    s = map_slot(setting_key);
    if (s && s->scan_evt) SetEvent(s->scan_evt);
}

void ModListChannelSelect(const char* setting_key, const char* value) {
    ModListChannelBlock* b;
    int i;
    if (!setting_key || !value) return;
    b = ModListChannelMap(setting_key);
    if (!b) return;
    for (i = 0; i < MODLISTCH_VAL_LEN - 1 && value[i]; i++) b->sel_value[i] = value[i];
    b->sel_value[i] = 0;
    b->sel_seq++;
}

const wchar_t* ModListChannelSubLabel(const char* setting_key) {
    ModListChannelBlock* b;
    if (!setting_key) return NULL;
    b = ModListChannelMap(setting_key);
    if (!b || !b->sublabel[0]) return NULL;
    return b->sublabel;
}
