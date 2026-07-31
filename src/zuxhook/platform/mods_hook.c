#include "mods_hook.h"
#include "kerncore.h"

/* One entry per hooked target in this process. Entries are never freed: a hook
   is a boot-time act and the chain pointers handed to mods must stay valid for
   the process lifetime. */
#define HOOK_MAX 16

typedef struct {
    DWORD  target;
    void*  head;      /* newest replacement; what the entry currently points at */
    void*  original;  /* trampoline that runs the target's real body */
} HookEntry;

static HookEntry g_hooks[HOOK_MAX];
static int       g_hook_n = 0;

/* Trampoline word layout (4 words), all A32:

     [0] <orig0>   relocated target instruction 0
     [1] <orig1>   relocated target instruction 1
     [2] e51ff004  ldr pc, [pc, #-4]  -> [3]
     [3] target+8  resume address

   Entering it with the original arguments in r0-r3 runs the target unchanged. */
static void* build_original_tramp(DWORD target_va, DWORD orig0, DWORD orig1) {
    DWORD* t = (DWORD*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    t[0] = orig0;
    t[1] = orig1;
    t[2] = 0xe51ff004u;
    t[3] = target_va + 8;
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    return t;
}

static HookEntry* find_entry(DWORD target_va) {
    int i;
    for (i = 0; i < g_hook_n; i++)
        if (g_hooks[i].target == target_va) return &g_hooks[i];
    return 0;
}

int ModHookInstall(DWORD target_va, void* replacement, void** out_next) {
    DWORD proc, orig0, orig1, entry_patch[2];
    HookEntry* e;

    if (!replacement || !out_next) return -1;
    *out_next = 0;
    if (!kerncore_is_ready() || !kerncore_ensure_helpers()) return -1;
    proc = kerncore_find_proc_struct(GetCurrentProcessId());
    if (proc == 0) return -1;

    e = find_entry(target_va);
    if (!e) {
        if (g_hook_n >= HOOK_MAX) return -1;
        __try {
            orig0 = *(volatile DWORD*)target_va;
            orig1 = *(volatile DWORD*)(target_va + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        /* Someone patched this entry without going through here, so the bytes we
           would relocate are a branch to their code, not the real body. */
        if (orig0 == 0xe51ff004u) return -1;

        e = &g_hooks[g_hook_n];
        e->target   = target_va;
        e->original = build_original_tramp(target_va, orig0, orig1);
        if (!e->original) return -1;
        e->head = e->original;
        g_hook_n++;
    }

    /* Hand the caller its continuation before the entry points at it: the first
       call can arrive on the very next instruction. */
    *out_next = e->head;

    entry_patch[0] = 0xe51ff004u;                /* ldr pc, [pc, #-4] */
    entry_patch[1] = (DWORD)replacement;
    if (kerncore_patch_code(proc, target_va, entry_patch, 8) != 0) {
        *out_next = 0;
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), (void*)target_va, 8);

    e->head = replacement;
    return 0;
}
