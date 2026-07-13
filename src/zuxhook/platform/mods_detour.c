#include "mods_detour.h"
#include "kerncore.h"

/* Trampoline word layout (9 words = 36 bytes), all A32:

     [0] e92d500f  stmfd sp!, {r0-r3, r12, lr}     save caller-saved regs
     [1] e59fc014  ldr   r12, [pc, #20]  -> [8]    load handler address
     [2] e12fff3c  blx   r12                       handler(r0,r1,r2,r3)
     [3] e8bd500f  ldmfd sp!, {r0-r3, r12, lr}     restore (sp back to entry)
     [4] <orig0>   relocated target instruction 0
     [5] <orig1>   relocated target instruction 1
     [6] e51ff004  ldr   pc, [pc, #-4]   -> [7]    branch back
     [7] target+8  resume address (after the two relocated instructions)
     [8] handler   handler function address

   r0-r3 are still the original arguments at [2] (only copies were pushed at
   [0]), so the handler observes them directly. stmfd/ldmfd are balanced, so sp
   equals the function's entry sp when the relocated `mov ip, sp` runs. */

int ModDetourInstallObserve(DWORD target_va, ModDetourHandler handler) {
    DWORD proc, orig0, orig1, entry_patch[2];
    DWORD* tramp;

    if (handler == 0) return -1;
    if (!kerncore_is_ready() || !kerncore_ensure_helpers()) return -1;
    proc = kerncore_find_proc_struct(GetCurrentProcessId());
    if (proc == 0) return -1;

    __try {
        orig0 = *(volatile DWORD*)target_va;
        orig1 = *(volatile DWORD*)(target_va + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    /* Already redirected (our entry gadget present), so don't double-install. */
    if (orig0 == 0xe51ff004u) return -1;

    tramp = (DWORD*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_EXECUTE_READWRITE);
    if (tramp == 0) return -1;

    tramp[0] = 0xe92d500fu;
    tramp[1] = 0xe59fc014u;
    tramp[2] = 0xe12fff3cu;
    tramp[3] = 0xe8bd500fu;
    tramp[4] = orig0;
    tramp[5] = orig1;
    tramp[6] = 0xe51ff004u;
    tramp[7] = target_va + 8;
    tramp[8] = (DWORD)handler;
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);

    entry_patch[0] = 0xe51ff004u;        /* ldr pc, [pc, #-4]  */
    entry_patch[1] = (DWORD)tramp;       /* -> trampoline      */
    if (kerncore_patch_code(proc, target_va, entry_patch, 8) != 0) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), (void*)target_va, 8);
    return 0;
}
