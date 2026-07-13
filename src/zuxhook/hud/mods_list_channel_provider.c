#include "mods_list_channel_provider.h"
#include "mods_list_channel.h"
#include "mods_list_model.h"     /* ModListRow / ModListSource */
#include "mods_icon_host.h"      /* ModsHudContextRegister */
#include "mods_state_event.h"    /* ModStateEventPublish: wake the daemon */
#include "mods_log.h"

#include <windows.h>
#include <string.h>

#define CHP_LOG  L"\\flash2\\automation\\mods\\list-channel.log"

/* One provider per registered picker key. The ModListSource `ctx` points back at
   the owning slot so the callbacks recover their key. Slots are file-static and
   never freed (registration is a boot-time, once-per-mod act), so the pointers
   handed to the context registry stay valid for the process lifetime. */
#define CHP_MAX  8
typedef struct {
    char         key[MOD_STATE_ID_LEN + 1];
    ModListSource src;
    int          used;
} ChpSlot;
static ChpSlot g_slots[CHP_MAX];

static void chp_log(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mods_vflashlog(CHP_LOG, fmt, ap);
    va_end(ap);
}

static void copy_w(wchar_t* dst, int cap, const wchar_t* src) {
    int i = 0;
    if (src) for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* Rows come straight from the channel the daemon publishes. When the channel is
   empty (no scan landed yet), show a single non-selectable "Searching" row so the
   picker never renders blank while discovery runs. user = row index, or the
   PLACEHOLDER sentinel for the searching row (on_tap ignores it). */
#define CHP_PLACEHOLDER  0xFFFFFFFFu

static int chp_count(void* ctx) {
    ChpSlot* s = (ChpSlot*)ctx;
    ModListChannelBlock* b = ModListChannelMap(s->key);
    int n;
    if (!b) return 1;
    n = (int)b->count;
    if (n < 0) n = 0;
    if (n > MODLISTCH_MAX_ROWS) n = MODLISTCH_MAX_ROWS;
    return n > 0 ? n : 1;
}

static int val_eq(const char* a, const char* b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static void chp_fill(void* ctx, int row, ModListRow* out) {
    ChpSlot* s = (ChpSlot*)ctx;
    ModListChannelBlock* b = ModListChannelMap(s->key);
    int n = b ? (int)b->count : 0;
    if (n > MODLISTCH_MAX_ROWS) n = MODLISTCH_MAX_ROWS;
    if (!b || n <= 0) {
        copy_w(out->main, MODLIST_TEXT_LEN, L"Searching...");
        out->user = CHP_PLACEHOLDER;
        return;
    }
    if (row < 0 || row >= n) return;   /* row already zeroed by populate */
    copy_w(out->main, MODLIST_TEXT_LEN, b->row[row].name);
    if (b->row[row].sub[0])
        copy_w(out->sub, MODLIST_TEXT_LEN, b->row[row].sub);
    else if (b->sel_value[0] && val_eq(b->row[row].value, b->sel_value))
        copy_w(out->sub, MODLIST_TEXT_LEN, L"Selected");
    out->user = (DWORD)row;
}

static void chp_on_open(void* ctx) {
    ChpSlot* s = (ChpSlot*)ctx;
    ModListChannelSignalScan(s->key);
    chp_log(L"OPEN %S -> scan requested", s->key);
}

static void chp_on_tap(void* ctx, int row) {
    ChpSlot* s = (ChpSlot*)ctx;
    ModListChannelBlock* b = ModListChannelMap(s->key);
    int n = b ? (int)b->count : 0;
    if (n > MODLISTCH_MAX_ROWS) n = MODLISTCH_MAX_ROWS;
    if (!b || n <= 0 || row < 0 || row >= n) return;   /* placeholder / stale tap */
    ModListChannelSelect(s->key, b->row[row].value);
    ModStateEventPublish();   /* wake the daemon so it adopts the new selection */
    chp_log(L"SELECT %S row=%d value=%S", s->key, row, b->row[row].value);
}

void ModListChannelProviderRegister(const char* setting_key) {
    int i, klen;
    ChpSlot* s = NULL;
    if (!setting_key) return;
    for (i = 0; i < CHP_MAX; i++) {
        if (g_slots[i].used && val_eq(g_slots[i].key, setting_key)) return;   /* already registered */
        if (!g_slots[i].used && !s) s = &g_slots[i];
    }
    if (!s) return;
    for (klen = 0; klen < MOD_STATE_ID_LEN && setting_key[klen]; klen++) s->key[klen] = setting_key[klen];
    s->key[klen] = 0;
    s->src.count   = chp_count;
    s->src.fill    = chp_fill;
    s->src.on_tap  = chp_on_tap;
    s->src.ctx     = s;
    s->src.on_open = chp_on_open;
    s->used        = 1;
    ModsHudContextRegister(s->key, &s->src);
    ModListChannelMap(s->key);   /* create the section now so the daemon can attach */
    chp_log(L"registered picker for %S", s->key);
}
