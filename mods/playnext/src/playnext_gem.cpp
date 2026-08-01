/* "Play Next" gemstone integration (load_module init PlayNextInstall).
 * Two chained hooks on gemstone.exe, base 0x10000, static VA == live VA:
 *   0x669fc  row-add helper, to inject the "Play Next" row into media menus
 *   0x67f94  context-menu command executor, to handle PN_CMD
 * Both go through lyra_hook_install, so another mod hooking the same functions
 * keeps working whatever the load order.
 * See playnext_queue.c and notes/re-2026-07-14-queue-insertitem-stub/. */

#include <windows.h>
#include <stdarg.h>
#include "playnext_queue.h"
#include "lyra.h"
#include "ce_log.h"

#define GEM_EXECUTOR       0x00067f94u
#define PN_CMD             0x40u

static const wchar_t PN_LABEL[] = L"Play Next";

CE_LOGGER(L, L"\\flash2\\automation\\playnext.log")

typedef DWORD (*ExecFn)(DWORD cmd, DWORD ctx, DWORD item);
static DWORD g_add_cmd = 0;   /* the native "add to now playing" cmd for the open menu */

/* Live play-order position. The queue object's own 0xa4 field does not track
   natural track advance, so read the ZDK accessor. -1 on failure. */
static int live_active_index(void) {
    typedef int (*GasiFn)(int*);
    HMODULE z = LoadLibraryW(L"zdksystem.dll");
    GasiFn f = z ? (GasiFn)GetProcAddress(z, L"ZDKMedia_Queue_GetActiveSongIndex") : 0;
    int gi = -1;
    if (f) { if (f(&gi) < 0) gi = -1; }
    return gi;
}

/* Reuse the native add-to-now-playing (0x19/0x1a) to append, then reorder the
   appended tail to after the current track. The hook only catches PN_CMD, so
   calling the executor with the add-cmd passes through to the native body. */
extern "C" int PlayNext_Handle(DWORD command_id, DWORD ctx, DWORD item) {
    DWORD addcmd;
    int old, nw, n, tries, cur;
    if (command_id != PN_CMD) return 0;

    addcmd = g_add_cmd ? g_add_cmd : 0x19u;
    cur = live_active_index();                       /* live position (append won't move it) */
    if (cur < 0) cur = 0;
    old = queue_count();
    ((ExecFn)GEM_EXECUTOR)(addcmd, ctx, item);       /* native append (passes through) */
    nw = old;
    for (tries = 0; tries < 40; tries++) {           /* wait for servicesd to apply it */
        nw = queue_count();
        if (nw > old) break;
        Sleep(25);
    }
    n = (old >= 0 && nw > old) ? (nw - old) : 0;
    if (n > 0) queue_move_tail_next(n, cur);
    return 1;
}

/* Executor hook: handle PN_CMD, otherwise continue down the chain so another
   mod's hook, and finally the native body, still run. */
static ExecFn g_next_exec = 0;

extern "C" DWORD PlayNext_Exec(DWORD command_id, DWORD ctx, DWORD item) {
    if (command_id == PN_CMD) { PlayNext_Handle(command_id, ctx, item); return 0; }
    return g_next_exec ? g_next_exec(command_id, ctx, item) : 0;
}

#define GEM_ROW_ADD        0x000669fcu
typedef int   (*RowAddFn)(DWORD idx, DWORD max, DWORD* items, DWORD cmd, DWORD* count);

static RowAddFn g_next_rowadd = 0;       /* rest of the row-add chain          */
static int g_media = 0;                  /* this build's menu is a media menu  */
static int g_injected = 0;               /* our row already added this build   */

/* Row-add hook. The chain does the native store (and any other mod's rows);
   this adds ours once per build, on media menus, marked by the add-to-now-playing
   cmd 0x19/0x1a. The builder takes each next index from *count, so inject exactly
   once (guarded). 5th arg (count) is the caller's stack slot. */
extern "C" int PlayNext_RowAdd(DWORD idx, DWORD max, DWORD* items, DWORD cmd, DWORD* count) {
    int i, rc;
    if (idx >= max) return (int)0x8007007au;
    rc = g_next_rowadd ? g_next_rowadd(idx, max, items, cmd, count) : 0;
    if (rc < 0) return rc;
    idx = count ? *count - 1 : idx;      /* the chain may have moved the cursor */

    if (idx == 0) { g_media = 0; g_injected = 0; }
    if (cmd == 0x19 || cmd == 0x1a) { g_media = 1; g_add_cmd = cmd; }
    if (g_media && !g_injected && count && (idx + 2) <= (int)max) {
        for (i = (int)idx; i >= 0; i--) {          /* shift [0..idx] down one slot */
            items[(i + 1) * 2 + 0] = items[i * 2 + 0];
            items[(i + 1) * 2 + 1] = items[i * 2 + 1];
        }
        items[0] = (DWORD)PN_LABEL;                /* our row at the top           */
        items[1] = PN_CMD;
        *count = idx + 2;
        g_injected = 1;
    }
    return 0;
}

extern "C" __declspec(dllexport) int PlayNextInstall(void) {
    int rc, rc2;
    L("PlayNextInstall: loaded into gemstone");
    rc  = lyra_hook_install(GEM_EXECUTOR, (void*)&PlayNext_Exec, (void**)&g_next_exec);
    rc2 = lyra_hook_install(GEM_ROW_ADD, (void*)&PlayNext_RowAdd, (void**)&g_next_rowadd);
    L("executor hook rc=%d rowadd hook rc=%d", rc, rc2);
    return (rc == 0 && rc2 == 0) ? 0 : -1;
}

extern "C" BOOL WINAPI DllMain(HANDLE h, DWORD r, LPVOID l) { (void)h; (void)r; (void)l; return TRUE; }
