#ifndef MODS_HOOK_H
#define MODS_HOOK_H

#include <windows.h>

/* Chainable replacement hook for an ARM (A32) function in a firmware module
   mapped into THIS process.

   Unlike ModDetourInstallObserve, the replacement REPLACES the target: it takes
   the target's arguments and decides what happens. It continues by calling the
   pointer it was handed, which is the next hook in the chain and, at the end,
   the original function.

   Several mods may hook the same target. The most recently installed runs first
   and receives the one installed before it, so each mod's hook is reached
   whatever the load order. That is the whole reason this lives in the platform:
   a mod that patches an entry itself owns the address, and the second mod to
   want it silently loses.

   Constraint, inherited from the trampoline that runs the original: the target's
   first two instructions (8 bytes) MUST be position-independent. Verify per
   target before use.

   Requires kerncore ready. Returns 0 on success, -1 otherwise. Install-once per
   (target, replacement); not undoable. */

#ifdef __cplusplus
extern "C" {
#endif

int ModHookInstall(DWORD target_va, void* replacement, void** out_next);

#ifdef __cplusplus
}
#endif

#endif /* MODS_HOOK_H */
