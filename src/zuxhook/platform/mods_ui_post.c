#include "mods_ui_post.h"

/* Small ring: posts are discrete acts (an answer arrived, a file landed), not a
   stream. A full queue means the UI loop is not running, and dropping is better
   than growing without bound behind a stalled loop. */
#define UI_POST_MAX 8

typedef struct { ModUiFn fn; void* ctx; } UiItem;

static UiItem           g_q[UI_POST_MAX];
static int              g_head = 0, g_tail = 0;
static CRITICAL_SECTION g_cs;
static int              g_ready = 0;

void ModUiPostProcessAttach(void) {
    InitializeCriticalSection(&g_cs);
    g_head = g_tail = 0;
    g_ready = 1;
}

int ModUiPost(ModUiFn fn, void* ctx) {
    int next, rc = -1;
    if (!fn || !g_ready) return -1;
    EnterCriticalSection(&g_cs);
    next = (g_tail + 1) % UI_POST_MAX;
    if (next != g_head) {
        g_q[g_tail].fn = fn;
        g_q[g_tail].ctx = ctx;
        g_tail = next;
        rc = 0;
    }
    LeaveCriticalSection(&g_cs);
    return rc;
}

void ModUiDrain(void) {
    if (!g_ready) return;
    for (;;) {
        UiItem it;
        EnterCriticalSection(&g_cs);
        if (g_head == g_tail) { LeaveCriticalSection(&g_cs); return; }
        it = g_q[g_head];
        g_head = (g_head + 1) % UI_POST_MAX;
        LeaveCriticalSection(&g_cs);
        __try { it.fn(it.ctx); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
