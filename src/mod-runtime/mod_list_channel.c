#include <windows.h>
#include <string.h>

#include "mod_list_channel.h"
#include "mod_state.h"     /* mod_state_notify (wake the HUD) */

/* Section + scan-event names are derived from the toggle key so both sides
 * compute them identically. Must match mods_list_channel.c's derivation:
 * prefix + key with '/' -> '_'. */
#define CH_SECTION_PREFIX  L"zune-mod-listch-v2-"
#define CH_SCAN_PREFIX     L"zune-mod-listch-scan-"
#define CH_NAME_MAX        96

static ModListChannelBlock* g_block = NULL;
static HANDLE               g_scan_evt = NULL;
static char                 g_toggle_key[64] = { 0 };

void mod_channel_init(const char* toggle_key)
{
    int i;
    if (!toggle_key) return;
    for (i = 0; i < (int)sizeof(g_toggle_key); i++) g_toggle_key[i] = 0;
    for (i = 0; i < (int)sizeof(g_toggle_key) - 1 && toggle_key[i]; i++)
        g_toggle_key[i] = toggle_key[i];
}

static void derive_name(wchar_t* out, const wchar_t* prefix)
{
    int j = 0, i;
    for (i = 0; prefix[i] && j < CH_NAME_MAX - 1; i++) out[j++] = prefix[i];
    for (i = 0; g_toggle_key[i] && j < CH_NAME_MAX - 1; i++)
        out[j++] = (g_toggle_key[i] == '/') ? L'_' : (wchar_t)(unsigned char)g_toggle_key[i];
    out[j] = 0;
}

/* Map (creating if absent) the channel section. Additive: no seed authority, so
 * whoever maps first zero-inits version (mirrors mods_list_channel.c and the
 * notify registry in mod_state.c). CE6 has no OpenFileMapping; a named
 * CreateFileMappingW attaches to the existing section or creates it. */
static ModListChannelBlock* map_block(void)
{
    wchar_t name[CH_NAME_MAX];
    HANDLE  h;
    int     created;
    if (g_block) return g_block;
    if (!g_toggle_key[0]) return NULL;
    derive_name(name, CH_SECTION_PREFIX);
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                           sizeof(ModListChannelBlock), name);
    if (!h) return NULL;
    created = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_block = (ModListChannelBlock*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (g_block && created && g_block->version == 0) g_block->version = 1;
    return g_block;
}

HANDLE mod_channel_scan_event(void)
{
    wchar_t name[CH_NAME_MAX];
    if (g_scan_evt) return g_scan_evt;
    if (!g_toggle_key[0]) return NULL;
    derive_name(name, CH_SCAN_PREFIX);
    g_scan_evt = CreateEventW(NULL, FALSE, FALSE, name);   /* auto-reset, shared by name */
    return g_scan_evt;
}

void mod_channel_publish(const ModListChannelRow* rows, int n)
{
    ModListChannelBlock* b = map_block();
    int i;
    if (!b || !rows) return;
    if (n < 0) n = 0;
    if (n > MODLISTCH_MAX_ROWS) n = MODLISTCH_MAX_ROWS;
    for (i = 0; i < n; i++) b->row[i] = rows[i];
    b->count = (DWORD)n;   /* written after the rows so a concurrent reader is consistent */
    b->list_seq++;
    mod_state_notify();     /* wake the HUD so an open picker re-queries */
}

int mod_channel_get_rows(ModListChannelRow* out, int max)
{
    ModListChannelBlock* b = map_block();
    int i, n;
    if (!b || !out || max <= 0) return 0;
    n = (int)b->count;
    if (n > MODLISTCH_MAX_ROWS) n = MODLISTCH_MAX_ROWS;
    if (n > max) n = max;
    for (i = 0; i < n; i++) out[i] = b->row[i];
    return n;
}

int mod_channel_get_selection(char* out, int out_sz)
{
    ModListChannelBlock* b = map_block();
    int i;
    if (!b || !out || out_sz <= 0) return 0;
    for (i = 0; i < out_sz - 1 && i < MODLISTCH_VAL_LEN && b->sel_value[i]; i++) out[i] = b->sel_value[i];
    out[i] = 0;
    return out[0] ? 1 : 0;
}

void mod_channel_set_selection(const char* value)
{
    ModListChannelBlock* b = map_block();
    int i;
    if (!b || !value) return;
    for (i = 0; i < MODLISTCH_VAL_LEN - 1 && value[i]; i++)
        if (b->sel_value[i] != value[i]) break;
    if ((value[i] ? value[i] : 0) == b->sel_value[i]) return;   /* unchanged */
    for (i = 0; i < MODLISTCH_VAL_LEN - 1 && value[i]; i++) b->sel_value[i] = value[i];
    b->sel_value[i] = 0;
    mod_state_notify();
}

void mod_channel_set_sublabel(const wchar_t* text)
{
    ModListChannelBlock* b = map_block();
    int i;
    if (!b) return;
    /* Dedup: callers on a hot path re-assert the same label every iteration; only
     * rewrite + wake the UI when it actually changes. */
    for (i = 0; i < MODLISTCH_SUBLABEL_LEN - 1 && text && text[i]; i++)
        if (b->sublabel[i] != text[i]) break;
    if ((text ? text[i] : 0) == b->sublabel[i]) return;   /* unchanged */
    for (i = 0; i < MODLISTCH_SUBLABEL_LEN - 1 && text && text[i]; i++) b->sublabel[i] = text[i];
    b->sublabel[i] = 0;
    mod_state_notify();
}
