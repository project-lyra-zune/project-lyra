#ifndef MODS_UI_POST_H
#define MODS_UI_POST_H

#include <windows.h>

/* Run a callback on the host's UI thread.

   Anything that touches XUI must run there, but a mod's own work often does not:
   a daemon thread, a discovery worker, or a hook that deferred its work until an
   answer arrived. Without this the mod's only options are to touch XUI off-thread
   or to busy-wait inside a hook, and both are wrong.

   Queued callbacks run from the UI-loop hook (mods_state_event.c's MsgWait
   proxy), so they run in whichever process the caller is in and nowhere else: the
   queue holds function pointers, which are meaningless across processes, so it is
   deliberately process-local rather than a shared block.

   ModUiPost returns 0 if queued, -1 if the queue is full. Callbacks run once, in
   post order, each guarded so one that faults cannot take the UI loop down. */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ModUiFn)(void* ctx);

int  ModUiPost(ModUiFn fn, void* ctx);

/* UI thread only; called by the loop hook. */
void ModUiDrain(void);

/* Called once at process attach, before any post can arrive. */
void ModUiPostProcessAttach(void);

#ifdef __cplusplus
}
#endif

#endif /* MODS_UI_POST_H */
