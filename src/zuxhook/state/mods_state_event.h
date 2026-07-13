#ifndef MODS_STATE_EVENT_H
#define MODS_STATE_EVENT_H

#include <windows.h>

/* Cross-process mod-state change notification - Layer 2, built on the same
   primitive native uses for transport play-state.

   The current value lives in the shared ModStateBlock; THIS module carries the
   *change event* cross-process. The set of consumers is not hardcoded: each
   process registers its own notification endpoint in a shared registry
   (zune-mod-notify-v1) at install, and the producer fans the change out to every
   registered endpoint. Two endpoint kinds:

     UI host (gemstone, servicesd/zhud) - a Windows CE point-to-point MsgQueue,
       drained on that process's XUI UI thread because the UI thread is the
       queue's own waiter. Each UI host calls MsgWaitForMultipleObjectsEx exactly
       once (its main loop); we redirect that single import slot to a wrapper that
       appends our queue handle to the loop's own handle array, so a producer
       write wakes the UI thread by construction; the wrapper drains (non-blocking
       ReadMsgQueue), re-renders the icons on the UI thread, and returns an index
       the firmware loop reads identically. No stack-frame surgery, no worker->UI
       marshalling.

     Non-UI daemon (castd) - its own named auto-reset event. One event PER daemon
       (not one shared event for all), so an auto-reset wake reaches its single
       intended waiter; a second daemon cannot split the signal stream. */

#ifdef __cplusplus
extern "C" {
#endif

/* Notification-endpoint kinds in the shared consumer registry. Values are part
   of the cross-process ABI (castd mirrors them); names may differ per side. */
enum {
    MOD_NOTIFY_FREE         = 0,
    MOD_NOTIFY_UI_QUEUE     = 1,   /* CreateMsgQueue + WriteMsgQueue ping */
    MOD_NOTIFY_DAEMON_EVENT = 2    /* CreateEventW + SetEvent, one per daemon */
};

/* Register this process's notification endpoint in the shared registry. Dedup
   is by name (re-register updates in place), so stable role-based names
   (zune-mod-state-q-gem, ...-q-svc, zune-mod-state-evt-castd) let a restarted
   process reclaim its slot without growth. */
void ModNotifyRegister(DWORD kind, const wchar_t* name);

/* COREDLL ord#871 (MsgWaitForMultipleObjectsEx) import slot, per host. Single
   caller in each module (the UI main loop), so redirecting the slot is exactly a
   one-call-site detour. */
#define STATE_EVENT_MSGWAIT_IAT_GEMSTONE  0x00096244u
#define STATE_EVENT_MSGWAIT_IAT_SERVICESD 0x419de150u   /* zhud_serv.dll import */

/* Consumer: create this process's notification queue, register it as a UI-queue
   endpoint, and redirect its UI main loop's MsgWait so the queue joins the
   loop's wait set. */
void ModStateEventInstallConsumer(DWORD msgwaitIatSlot, const wchar_t* queueName);

/* Producer: notify every registered consumer that a slot changed. Call AFTER
   the ModStateBlock write. */
void ModStateEventPublish(void);

#ifdef __cplusplus
}
#endif

#endif /* MODS_STATE_EVENT_H */
