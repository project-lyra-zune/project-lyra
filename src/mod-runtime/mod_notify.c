#include "mod_notify.h"

#include <string.h>

#include "ce_log.h"

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

static CreateMsgQueueFn p_create = 0;
static WriteMsgQueueFn  p_write  = 0;
static ReadMsgQueueFn   p_read   = 0;


/* Each consumer process appends its endpoint to the shared registry at install;
   the producer reads it to fan a change out. */
static NotifyBlock* g_notify         = NULL;
static HANDLE       g_notify_section = NULL;
static HANDLE       g_notify_lock    = NULL;

/* ── log ─────────────────────────────────────────────────────────────────── */
CE_LOGGER(elog, L"\\flash2\\automation\\mods\\state-event.log")

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
        elog("notify: CreateFileMapping failed err=%lu", GetLastError());
        if (g_notify_lock) ReleaseMutex(g_notify_lock);
        return NULL;
    }
    created = (GetLastError() != ERROR_ALREADY_EXISTS);
    view = MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NotifyBlock));
    if (view == NULL) {
        elog("notify: MapViewOfFile failed err=%lu", GetLastError());
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
        elog("notify register: table full (%S)", name);
        return;
    }
    nb->c[free_idx].kind      = kind;
    nb->c[free_idx].owner_pid = GetCurrentProcessId();
    for (i = 0; i < MOD_NOTIFY_NAME_LEN; i++) nb->c[free_idx].name[i] = 0;
    for (i = 0; i < MOD_NOTIFY_NAME_LEN - 1 && name[i]; i++) nb->c[free_idx].name[i] = name[i];
    if (g_notify_lock) ReleaseMutex(g_notify_lock);
    elog("notify register pid=%lu slot=%d kind=%lu name=%S",
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

HANDLE ModNotifyCreateQueue(const wchar_t* name, int read_access) {
    MQ_OPTIONS o;
    resolve_coredll();
    if (!p_create || !name) return NULL;
    o.dwSize        = sizeof(o);
    o.dwFlags       = MQ_NOPRECOMMIT;
    o.dwMaxMessages = 16;
    o.cbMaxMessage  = sizeof(BYTE);
    o.bReadAccess   = read_access ? TRUE : FALSE;
    return p_create(name, &o);
}

void ModNotifyDrainQueue(HANDLE q) {
    BYTE  buf[8];
    DWORD nread, fl;
    if (!p_read || !q) return;
    while (p_read(q, buf, sizeof(buf), &nread, 0, &fl)) { /* drain */ }
}
