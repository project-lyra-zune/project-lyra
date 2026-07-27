#ifndef MOD_NOTIFY_H
#define MOD_NOTIFY_H

#include <windows.h>

#include "mod_state_abi.h"   /* MOD_NOTIFY_* kinds + the registry layout */

/* The consumer registry and the change fan-out, shared by every platform binary.
   The current value of a slot lives in ModStateBlock; this carries the *change
   event* to whoever asked for one.

   Each consumer process registers its own endpoint, so the set is not
   hardcoded. Two kinds: a UI host (gemstone, servicesd) registers a CE
   point-to-point MsgQueue drained on its own UI thread, and a non-UI daemon
   registers a named auto-reset event of its own. One event PER daemon: an
   auto-reset wake must reach its single intended waiter, and a shared event
   would let one daemon swallow another's signal.

   The UI-thread plumbing that consumes a queue is the host's own business and
   lives with the host (mods_state_event.c in zuxhook). */

#ifdef __cplusplus
extern "C" {
#endif

/* Register this process's notification endpoint. Dedup is by name, so a stable
   role-based name lets a restarted process reclaim its slot without growth. */
void ModNotifyRegister(DWORD kind, const wchar_t* name);

/* Notify every registered consumer that a slot changed. Call AFTER the
   ModStateBlock write. */
void ModStateEventPublish(void);

/* Create a CE MsgQueue by name. `read_access` picks the end: a consumer's read
   end, or a producer's write end. NULL if coredll's MsgQueue family is
   unavailable or creation fails. */
HANDLE ModNotifyCreateQueue(const wchar_t* name, int read_access);

/* Drain every pending record from a read queue without blocking. The records
   are bare pings: a consumer re-reads the authoritative values from the block. */
void ModNotifyDrainQueue(HANDLE q);

#ifdef __cplusplus
}
#endif

#endif /* MOD_NOTIFY_H */
