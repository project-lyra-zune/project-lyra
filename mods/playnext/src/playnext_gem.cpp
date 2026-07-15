/* "Play Next" gemstone integration (load_module init PlayNextInstall).
 * Two detours on gemstone.exe, base 0x10000, static VA == live VA:
 *   0x669fc  row-add helper, replaced to inject the "Play Next" row into media menus
 *   0x67f94  context-menu command executor, hooked to handle PN_CMD
 *   0x66204  label resolver, called by the row-add replacement
 * See playnext_queue.c and notes/re-2026-07-14-queue-insertitem-stub/. */

#include <windows.h>
#include <stdio.h>
#include "playnext_queue.h"
#include "kerncore.h"

#define GEM_EXECUTOR       0x00067f94u
#define PN_CMD             0x40u

static const wchar_t PN_LABEL[] = L"Play Next";

static void L(const char* s) {
    HANDLE f = CreateFileW(L"\\flash2\\automation\\playnext.log", GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    { DWORD n; WriteFile(f, s, (DWORD)strlen(s), &n, NULL); WriteFile(f, "\r\n", 2, &n, NULL); }
    CloseHandle(f);
}

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

/* Modifying entry detour on the executor: call PlayNext_Handle; if it handled
   PN_CMD, return S_OK and skip the native body, else replay the prologue and
   continue. Hand-assembled A32; the word annotations are the encoding. */
static int install_executor_hook(void) {
    DWORD proc, orig0, orig1, entry[2];
    DWORD* t;
    if (!kerncore_is_ready() || !kerncore_ensure_helpers()) return -1;
    proc = kerncore_find_proc_struct(GetCurrentProcessId());
    if (!proc) return -1;
    __try {
        orig0 = *(volatile DWORD*)GEM_EXECUTOR;
        orig1 = *(volatile DWORD*)(GEM_EXECUTOR + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (orig0 == 0xe51ff004u) return -1;               /* already hooked */

    t = (DWORD*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return -1;
    t[0]  = 0xe92d400fu;                               /* push {r0-r3, lr}     */
    t[1]  = 0xe59fc028u;                               /* ldr r12,[pc,#0x28]->[13] */
    t[2]  = 0xe12fff3cu;                               /* blx r12              */
    t[3]  = 0xe1a0c000u;                               /* mov r12, r0          */
    t[4]  = 0xe8bd400fu;                               /* pop {r0-r3, lr}      */
    t[5]  = 0xe35c0000u;                               /* cmp r12, #0          */
    t[6]  = 0x0a000001u;                               /* beq -> [9]           */
    t[7]  = 0xe3a00000u;                               /* mov r0, #0           */
    t[8]  = 0xe12fff1eu;                               /* bx lr                */
    t[9]  = orig0;
    t[10] = orig1;
    t[11] = 0xe51ff004u;                               /* ldr pc,[pc,#-4]->[12]*/
    t[12] = GEM_EXECUTOR + 8;
    t[13] = (DWORD)&PlayNext_Handle;
    FlushInstructionCache(GetCurrentProcess(), t, 64);

    entry[0] = 0xe51ff004u;                            /* ldr pc,[pc,#-4]      */
    entry[1] = (DWORD)t;
    if (kerncore_patch_code(proc, GEM_EXECUTOR, entry, 8) != 0) {
        VirtualFree(t, 0, MEM_RELEASE);
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), (void*)GEM_EXECUTOR, 8);
    return 0;
}

#define GEM_ROW_ADD        0x000669fcu
#define GEM_RESOLVE_LABEL  0x00066204u   /* resolve_label(command_id) -> wchar_t* */

typedef DWORD (*LabelFn)(DWORD cmd);
static int g_media = 0;                  /* this build's menu is a media menu */
static int g_injected = 0;               /* our row already added this build   */

/* Replacement for the native row-add helper 0x669fc. Reproduces the native store,
   then on media menus (marked by the add-to-now-playing cmd 0x19/0x1a) injects our
   row once at the top. The builder takes each next index from *count, so inject
   exactly once (guarded). 5th arg (count) is the caller's stack slot. */
extern "C" int PlayNext_RowAdd(DWORD idx, DWORD max, DWORD* items, DWORD cmd, DWORD* count) {
    int i;
    if (idx >= max) return (int)0x8007007au;
    items[idx * 2 + 0] = ((LabelFn)GEM_RESOLVE_LABEL)(cmd);
    items[idx * 2 + 1] = cmd;
    if (count) *count = idx + 1;

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

/* Redirect a function entry straight to a replacement (no trampoline). */
static int install_replace(DWORD target, void* func) {
    DWORD proc, orig0, entry[2];
    if (!kerncore_is_ready() || !kerncore_ensure_helpers()) return -1;
    proc = kerncore_find_proc_struct(GetCurrentProcessId());
    if (!proc) return -1;
    __try { orig0 = *(volatile DWORD*)target; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (orig0 == 0xe51ff004u) return -1;               /* already redirected */
    entry[0] = 0xe51ff004u;                            /* ldr pc,[pc,#-4]    */
    entry[1] = (DWORD)func;                            /* &replacement (Thumb bit set) */
    if (kerncore_patch_code(proc, target, entry, 8) != 0) return -1;
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 8);
    return 0;
}

extern "C" __declspec(dllexport) int PlayNextInstall(void) {
    int rc, rc2;
    L("PlayNextInstall: loaded into gemstone");
    rc = install_executor_hook();
    { char b[48]; _snprintf(b, sizeof(b), "executor hook rc=%d", rc); b[47]=0; L(b); }
    rc2 = install_replace(GEM_ROW_ADD, (void*)&PlayNext_RowAdd);
    { char b[48]; _snprintf(b, sizeof(b), "rowadd replace rc=%d", rc2); b[47]=0; L(b); }
    return rc;
}

extern "C" BOOL WINAPI DllMain(HANDLE h, DWORD r, LPVOID l) { (void)h; (void)r; (void)l; return TRUE; }
