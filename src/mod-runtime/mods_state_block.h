#ifndef MODS_STATE_BLOCK_H
#define MODS_STATE_BLOCK_H

#include <windows.h>

#include "mod_state_abi.h"   /* ModStateBlock / ModFeatureSlot / section names */

/* ModStateBlock - the cross-process shared buffer of current mod-feature state,
   pulled at each consumer's tick (shared buffer + pull, not an event chain).
   servicesd (HUD icons, quick-toggle host, settings writers), gemstone
   (NowPlaying icons), and the non-UI daemons (castd) all map the same named
   section.

   Every slot's key carries its WRITER ROLE as a namespace prefix, the
   single-writer contract made structural instead of conventional:

     "setting/<mod>/<id>"  INTENT - written only by a control surface (the
                           quick-toggle / settings page); persisted per the
                           setting's `persist` flag.
     "status/<mod>/<id>"   EFFECT - written only by the feature's actor (a
                           subsystem authority in servicesd, or a daemon);
                           never persisted, runtime-derived.

   A slot's value is one small `state` integer (0..N-1): an icon shows
   frame[state], a row shows label[state]; a bool is just N=2. `owner_pid` tags
   daemon-written status so a dead daemon's status reaps back to state 0
   (control slots and subsystem-owned status use owner_pid 0).

   Steady-state `state` reads are a single aligned byte (atomic on ARM); the
   lock serialises slot assignment and the infrequent writes. */

#ifdef __cplusplus
extern "C" {
#endif

/* Map (creating + zero-initialising on first use) the shared block for this
   process. Cached for the process lifetime. NULL only if the section cannot be
   created or mapped. */
ModStateBlock* ModStateMap(void);

/* Slot index for `key`. If `assign` and no slot exists, claims the first free
   slot (state 0, owner 0). -1 if absent (and !assign) or the table is full. */
int ModStateSlotIndex(const char* key, int assign);

/* Current state (0..N-1). -1 if the key has no slot. Lock-free aligned read. */
int ModStateGetState(const char* key);

/* Set a slot's state (assigning on first use). The caller MUST own the slot per
   its role: a control surface for `setting/`, the feature's actor for
   `status/`. `owner_pid` is recorded for status reaping: pass 0 for
   control/subsystem slots, GetCurrentProcessId() for daemon-written status.

   Returns 1 only if the value moved. Publish on that: re-asserting the same
   state every iteration is normal here, and each fan-out wakes every UI host. */
int ModStateSetState(const char* key, int state, DWORD owner_pid);

/* Initialise a slot the first time it is seen this boot (idempotent across
   processes and re-applies: an existing slot keeps its live state). */
void ModStateSeed(const char* key, int state, DWORD owner_pid);

/* A slot's MOD_SLOT_FLAG_* bits: per-setting attributes a control surface owns,
   independent of the value. -1 if the key has no slot. The state and flag
   writers never touch each other's field, so either may be set alone. */
int  ModStateGetFlags(const char* key);
void ModStateSetFlags(const char* key, int flags);

/* Reset every `status/` slot whose owner_pid names a dead process back to state
   0 and clear its owner. Returns the number reset (the caller republishes the
   UI notify when >0). No-op until kerncore liveness is available. */
int ModStateReapDeadOwners(void);

/* Append a one-shot dump of the block to this process's mod-state log. `role`
   tags the line with the host process. */
void ModStateLogSnapshot(const wchar_t* role);

#ifdef __cplusplus
}
#endif

#endif /* MODS_STATE_BLOCK_H */
