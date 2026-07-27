#include "mods_state_event.h"
#include "mod_notify.h"       /* ModNotifyRegister / ModNotifyCreateQueue / drain */
#include "mods_icon_host.h"   /* ModsIconOnStateChanged */
#include "mods_log.h"
#include "boot_state.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "ce_log.h"

/* This host's consumer end: the queue a producer pings, joined to the firmware
   main loop's own wait set so a change wakes the UI thread by construction. The
   registry and the fan-out are shared (mod_notify.c); only the plumbing that
   drains a queue on THIS process's UI thread lives here. */
typedef DWORD (WINAPI *MsgWaitFn)(DWORD count, const HANDLE* handles,
                                  DWORD ms, DWORD mask, DWORD flags);

#define MWMO_WAITALL_ 0x00000001u

static MsgWaitFn g_orig_wait = 0;   /* real MsgWaitForMultipleObjectsEx */
static HANDLE    g_read_q    = 0;   /* this process's consumer queue */

CE_LOGGER(elog, L"\\flash2\\automation\\mods\\state-event.log")


/* Drain every pending notification (non-blocking, the native UI-thread pattern)
   and re-render this process's icons. Runs on the UI thread, the thread the
   firmware main loop calls MsgWait on. The record is a bare ping; the icons
   read the authoritative values from the shared block. */
static void (*g_drain_hook)(void) = NULL;

void ModStateEventSetDrainHook(void (*fn)(void)) { g_drain_hook = fn; }

static void drain_render(void) {
    ModNotifyDrainQueue(g_read_q);
    ModsIconOnStateChanged();
    if (g_drain_hook) g_drain_hook();
}

/* The redirected MsgWaitForMultipleObjectsEx: append our queue handle to the
   firmware loop's own handle array, wait, and translate the result so the loop's
   existing dispatch is unaffected. WAIT semantics: a message wake returns
   WAIT_OBJECT_0 + count, so passing count+1 puts the message at +count+1 and our
   handle at +count, both remapped below. */
static DWORD WINAPI MsgWait_proxy(DWORD count, const HANDLE* handles,
                                  DWORD ms, DWORD mask, DWORD flags) {
    HANDLE local[64];
    DWORD  i, r;
    if (g_orig_wait == 0) return WAIT_FAILED;
    ModsHudMenuTick();   /* UI thread: dismiss a HUD menu whose HUD has closed (no-op off the HUD host) */
    BootStateTick();     /* UI thread: commit the boot once the shell's loop is live (no-op off the shell) */
    if (g_read_q == 0 || count == 0 || count >= 63 || (flags & MWMO_WAITALL_))
        return g_orig_wait(count, handles, ms, mask, flags);

    for (i = 0; i < count; i++) local[i] = handles[i];
    local[count] = g_read_q;

    r = g_orig_wait(count + 1, local, ms, mask, flags);

    if (r == WAIT_OBJECT_0 + count) {           /* our queue signalled */
        drain_render();
        return WAIT_TIMEOUT;                      /* loop re-pumps; nothing of its own */
    }
    if (r == WAIT_OBJECT_0 + count + 1)          /* the message pseudo-handle, shifted +1 */
        return WAIT_OBJECT_0 + count;            /* what the loop expects for "message" */
    return r;                                    /* loop's own handles / timeout / failure */
}

void ModStateEventInstallConsumer(DWORD msgwaitIatSlot, const wchar_t* queueName) {
    DWORD original = 0;

    g_read_q = ModNotifyCreateQueue(queueName, 1);
    if (g_read_q == 0) {
        elog("consumer: CreateMsgQueue(read,%S) failed err=%lu", queueName, GetLastError());
        return;
    }

    ModNotifyRegister(MOD_NOTIFY_UI_QUEUE, queueName);

    __try {
        original = *(volatile DWORD*)msgwaitIatSlot;
        g_orig_wait = (MsgWaitFn)original;   /* set before redirect: a call landing
                                                on the proxy mid-install must have it */
        *(volatile DWORD*)msgwaitIatSlot = (DWORD)&MsgWait_proxy;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        elog("consumer: MsgWait IAT patch faulted @0x%08x", msgwaitIatSlot);
        return;
    }
    elog("consumer installed (pid=%lu): q=%S iat=0x%08x orig=0x%p",
         GetCurrentProcessId(), queueName, msgwaitIatSlot, (void*)original);
}
