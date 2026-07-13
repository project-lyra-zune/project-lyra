#include "mods_state_event.h"
#include "mods_icon_host.h"   /* ModsIconOnStateChanged */
#include "mods_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ── CE MsgQueue API ─────────────────────────────────────────────────────────
   Resolved from coredll at runtime so the module carries no import-lib
   dependency on the MsgQueue family. COREDLL exports (v4.5): CreateMsgQueue,
   WriteMsgQueue, ReadMsgQueue by name. */
typedef struct {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwMaxMessages;
    DWORD cbMaxMessage;
    BOOL  bReadAccess;
} MQ_OPTIONS;

#define MQ_NOPRECOMMIT 0x00000001u
#define MWMO_WAITALL_  0x00000001u

typedef HANDLE (WINAPI *CreateMsgQueueFn)(const wchar_t* name, MQ_OPTIONS* opt);
typedef BOOL   (WINAPI *WriteMsgQueueFn)(HANDLE q, LPVOID buf, DWORD cb, DWORD timeout, DWORD flags);
typedef BOOL   (WINAPI *ReadMsgQueueFn)(HANDLE q, LPVOID buf, DWORD cb, LPDWORD read, DWORD timeout, DWORD* flags);
typedef DWORD  (WINAPI *MsgWaitFn)(DWORD count, const HANDLE* handles, DWORD ms, DWORD mask, DWORD flags);

static CreateMsgQueueFn p_create = 0;
static WriteMsgQueueFn  p_write  = 0;
static ReadMsgQueueFn   p_read   = 0;

static MsgWaitFn g_orig_wait = 0;   /* real MsgWaitForMultipleObjectsEx */
static HANDLE    g_read_q    = 0;   /* this process's consumer queue */

/* ── shared consumer registry (zune-mod-notify-v1) ──────────────────────────
   Each consumer process appends its endpoint here at install; the producer
   reads it to fan a change out. Pagefile-backed named section, version-tagged
   like the state block so a stale layout can never be mapped at the new stride.
   castd mirrors this ABI byte-for-byte. */
#define MOD_NOTIFY_SECTION_NAME  L"zune-mod-notify-v1"
#define MOD_NOTIFY_LOCK_NAME     L"zune-mod-notify-lock-v1"
#define MOD_NOTIFY_VERSION       1u
#define MOD_NOTIFY_MAX           16
#define MOD_NOTIFY_NAME_LEN      48

typedef struct {
    DWORD   kind;                       /* MOD_NOTIFY_* (FREE = empty slot) */
    DWORD   owner_pid;                  /* registering process (diagnostic) */
    wchar_t name[MOD_NOTIFY_NAME_LEN];  /* queue / event name, NUL-terminated */
} NotifyConsumer;

typedef struct {
    DWORD          version;
    NotifyConsumer c[MOD_NOTIFY_MAX];
} NotifyBlock;

static NotifyBlock* g_notify         = NULL;
static HANDLE       g_notify_section = NULL;
static HANDLE       g_notify_lock    = NULL;

/* ── log ─────────────────────────────────────────────────────────────────── */
#define STATE_EVENT_LOG  L"\\flash2\\automation\\mods\\state-event.log"
static void elog(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mods_vflashlog(STATE_EVENT_LOG, fmt, ap);
    va_end(ap);
}

static void resolve_coredll(void) {
    HMODULE c;
    if (p_create) return;
    c = GetModuleHandleW(L"coredll.dll");
    if (!c) return;
    p_create = (CreateMsgQueueFn)GetProcAddress(c, L"CreateMsgQueue");
    p_write  = (WriteMsgQueueFn) GetProcAddress(c, L"WriteMsgQueue");
    p_read   = (ReadMsgQueueFn)  GetProcAddress(c, L"ReadMsgQueue");
}

/* ── consumer registry ──────────────────────────────────────────────────────
   The registry is purely additive (register-only; no seeding), so whichever
   process maps it first may create it, unlike the state block, where servicesd
   must seed slots before consumers read. */
static NotifyBlock* notify_map(void) {
    HANDLE sec;
    void*  view;
    int    created;

    if (g_notify) return g_notify;
    if (g_notify_lock == NULL)
        g_notify_lock = CreateMutexW(NULL, FALSE, MOD_NOTIFY_LOCK_NAME);
    if (g_notify_lock) WaitForSingleObject(g_notify_lock, INFINITE);
    if (g_notify) { if (g_notify_lock) ReleaseMutex(g_notify_lock); return g_notify; }

    sec = CreateFileMappingW((HANDLE)INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                             0, sizeof(NotifyBlock), MOD_NOTIFY_SECTION_NAME);
    if (sec == NULL) {
        elog(L"notify: CreateFileMapping failed err=%lu", GetLastError());
        if (g_notify_lock) ReleaseMutex(g_notify_lock);
        return NULL;
    }
    created = (GetLastError() != ERROR_ALREADY_EXISTS);
    view = MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NotifyBlock));
    if (view == NULL) {
        elog(L"notify: MapViewOfFile failed err=%lu", GetLastError());
        CloseHandle(sec);
        if (g_notify_lock) ReleaseMutex(g_notify_lock);
        return NULL;
    }
    g_notify_section = sec;
    g_notify         = (NotifyBlock*)view;
    if (created && g_notify->version == 0)
        g_notify->version = MOD_NOTIFY_VERSION;

    if (g_notify_lock) ReleaseMutex(g_notify_lock);
    return g_notify;
}

void ModNotifyRegister(DWORD kind, const wchar_t* name) {
    NotifyBlock* nb = notify_map();
    int i, free_idx = -1;
    if (!nb || !name) return;

    if (g_notify_lock) WaitForSingleObject(g_notify_lock, INFINITE);
    for (i = 0; i < MOD_NOTIFY_MAX; i++) {
        if (nb->c[i].kind == MOD_NOTIFY_FREE) { if (free_idx < 0) free_idx = i; continue; }
        if (wcscmp(nb->c[i].name, name) == 0) {   /* dedup: re-register updates in place */
            nb->c[i].kind      = kind;
            nb->c[i].owner_pid = GetCurrentProcessId();
            if (g_notify_lock) ReleaseMutex(g_notify_lock);
            return;
        }
    }
    if (free_idx < 0) {
        if (g_notify_lock) ReleaseMutex(g_notify_lock);
        elog(L"notify register: table full (%s)", name);
        return;
    }
    nb->c[free_idx].kind      = kind;
    nb->c[free_idx].owner_pid = GetCurrentProcessId();
    for (i = 0; i < MOD_NOTIFY_NAME_LEN; i++) nb->c[free_idx].name[i] = 0;
    for (i = 0; i < MOD_NOTIFY_NAME_LEN - 1 && name[i]; i++) nb->c[free_idx].name[i] = name[i];
    if (g_notify_lock) ReleaseMutex(g_notify_lock);
    elog(L"notify register pid=%lu slot=%d kind=%lu name=%s",
         GetCurrentProcessId(), free_idx, (unsigned long)kind, name);
}

/* Producer write-queue handle cache. A CE point-to-point MsgQueue read handle
   becomes persistently signalled once its LAST writer closes ("no writers"
   terminal state); the UI host waits on that read handle, so a stuck signal
   spins its main loop. The producer therefore opens each write end once and
   keeps it open for the process lifetime - never close per publish. */
static struct { wchar_t name[MOD_NOTIFY_NAME_LEN]; HANDLE h; } g_pubq[MOD_NOTIFY_MAX];
static int g_pubq_n = 0;

static HANDLE pub_write_queue(const wchar_t* name) {
    HANDLE     q;
    MQ_OPTIONS o;
    int        i;
    if (!p_create) return NULL;
    if (g_notify_lock) WaitForSingleObject(g_notify_lock, INFINITE);
    for (i = 0; i < g_pubq_n; i++) {
        if (wcscmp(g_pubq[i].name, name) == 0) {
            HANDLE cached = g_pubq[i].h;
            if (g_notify_lock) ReleaseMutex(g_notify_lock);
            return cached;
        }
    }
    if (g_pubq_n >= MOD_NOTIFY_MAX) { if (g_notify_lock) ReleaseMutex(g_notify_lock); return NULL; }
    o.dwSize        = sizeof(o);
    o.dwFlags       = MQ_NOPRECOMMIT;
    o.dwMaxMessages = 16;
    o.cbMaxMessage  = sizeof(BYTE);
    o.bReadAccess   = FALSE;
    q = p_create(name, &o);
    if (q) {
        for (i = 0; i < MOD_NOTIFY_NAME_LEN; i++) g_pubq[g_pubq_n].name[i] = 0;
        for (i = 0; i < MOD_NOTIFY_NAME_LEN - 1 && name[i]; i++) g_pubq[g_pubq_n].name[i] = name[i];
        g_pubq[g_pubq_n].h = q;
        g_pubq_n++;
    }
    if (g_notify_lock) ReleaseMutex(g_notify_lock);
    return q;
}

/* Drain every pending notification (non-blocking, the native UI-thread pattern)
   and re-render this process's icons. Runs on the UI thread, the thread the
   firmware main loop calls MsgWait on. The record is a bare ping; the icons
   read the authoritative values from the shared block. */
static void drain_render(void) {
    BYTE  buf[8];
    DWORD nread, fl;
    if (p_read) {
        while (p_read(g_read_q, buf, sizeof(buf), &nread, 0, &fl)) { /* drain */ }
    }
    ModsIconOnStateChanged();
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
    MQ_OPTIONS o;
    DWORD original = 0;

    resolve_coredll();
    if (p_create == 0 || p_read == 0) {
        elog(L"consumer: coredll MsgQueue unresolved (create=%p read=%p)", p_create, p_read);
        return;
    }

    o.dwSize        = sizeof(o);
    o.dwFlags       = MQ_NOPRECOMMIT;
    o.dwMaxMessages = 16;
    o.cbMaxMessage  = sizeof(BYTE);
    o.bReadAccess   = TRUE;
    g_read_q = p_create(queueName, &o);
    if (g_read_q == 0) {
        elog(L"consumer: CreateMsgQueue(read,%s) failed err=%lu", queueName, GetLastError());
        return;
    }

    ModNotifyRegister(MOD_NOTIFY_UI_QUEUE, queueName);

    __try {
        original = *(volatile DWORD*)msgwaitIatSlot;
        g_orig_wait = (MsgWaitFn)original;   /* set before redirect: a call landing
                                                on the proxy mid-install must have it */
        *(volatile DWORD*)msgwaitIatSlot = (DWORD)&MsgWait_proxy;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        elog(L"consumer: MsgWait IAT patch faulted @0x%08x", msgwaitIatSlot);
        return;
    }
    elog(L"consumer installed (pid=%lu): q=%s iat=0x%08x orig=0x%p",
         GetCurrentProcessId(), queueName, msgwaitIatSlot, (void*)original);
}

void ModStateEventPublish(void) {
    NotifyBlock*   nb = notify_map();
    NotifyConsumer snapshot[MOD_NOTIFY_MAX];
    int            n = 0, i;
    BYTE           ping = 1;

    resolve_coredll();
    if (!nb) return;

    /* Snapshot under the lock, then do the wake I/O (queue write / SetEvent)
       unlocked: a concurrent ModNotifyRegister in another process must not block
       behind a fan-out. */
    if (g_notify_lock) WaitForSingleObject(g_notify_lock, INFINITE);
    for (i = 0; i < MOD_NOTIFY_MAX; i++)
        if (nb->c[i].kind != MOD_NOTIFY_FREE) snapshot[n++] = nb->c[i];
    if (g_notify_lock) ReleaseMutex(g_notify_lock);

    for (i = 0; i < n; i++) {
        if (snapshot[i].kind == MOD_NOTIFY_UI_QUEUE) {
            HANDLE q = pub_write_queue(snapshot[i].name);   /* cached, never closed */
            if (q && p_write) p_write(q, &ping, sizeof(ping), 0, 0);
        } else if (snapshot[i].kind == MOD_NOTIFY_DAEMON_EVENT) {
            /* A named event has no last-writer sticky state, so open/set/close is
               safe; the daemon's own handle keeps the object alive. */
            HANDLE e = CreateEventW(NULL, FALSE, FALSE, snapshot[i].name);
            if (e) { SetEvent(e); CloseHandle(e); }
        }
    }
}
