#ifndef MODS_DETOUR_H
#define MODS_DETOUR_H

#include <windows.h>

/* Observe-only inline detour for an ARM (A32) function in a firmware module
   mapped into THIS process. The handler runs at the target's entry with
   r0-r3 as the original arguments; the original function then runs unchanged
   and returns to its real caller.

   Mechanism: the target's first two instructions are relocated into a small
   RWX trampoline (VirtualAlloc PAGE_EXECUTE_READWRITE; CE6 user pages are
   executable, see mods_phase2.c); the entry is repointed to that trampoline,
   which saves the caller-saved registers, calls the handler, restores them,
   replays the two relocated instructions, and branches back to target+8.

   Constraint: the target's first two instructions (8 bytes) MUST be position-
   independent: no PC-relative loads/branches. Verify per target before use.

   Requires kerncore ready (the PT-flip gadget patches the RO .text entry).
   Returns 0 on success, -1 otherwise. Install-once; not undoable (no caller
   needs removal yet). */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ModDetourHandler)(DWORD r0, DWORD r1, DWORD r2, DWORD r3);

int ModDetourInstallObserve(DWORD target_va, ModDetourHandler handler);

#ifdef __cplusplus
}
#endif

#endif /* MODS_DETOUR_H */
