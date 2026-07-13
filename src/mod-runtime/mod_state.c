#include <windows.h>
#include <string.h>
#include "mod_state.h"

/* Mirror of the zuxhook ModStateBlock cross-process ABI v3
 * (payloads/replacement/zuxhook/mods_state_block.h). Keep in lockstep: the
 * section name carries the layout version, so a stale section from a prior
 * layout cannot be mapped. A daemon reads its toggle (setting/<mod>/<id>) and
 * writes its own status (status/<mod>/<id>). */
#define MOD_STATE_SECTION_NAME   L"zune-mod-state-v3"
#define MOD_STATE_MAX_SLOTS      32
#define MOD_STATE_ID_LEN         48     /* role-namespaced key, NUL-padded */

typedef struct {
    char  key[MOD_STATE_ID_LEN];
    BYTE  state;
    BYTE  _pad[3];
    DWORD owner_pid;
} ModFeatureSlot;                       /* 56 bytes */

typedef struct {
    DWORD          version;
    DWORD          count;
    ModFeatureSlot slots[MOD_STATE_MAX_SLOTS];
} ModStateBlock;

static ModStateBlock* g_block = NULL;

/* Mirror of the zuxhook notify registry ABI (zune-mod-notify-v1,
 * payloads/replacement/zuxhook/mods_state_event.{c,h}). Each consumer registers
 * its endpoint; a producer fans a change out to every entry. A daemon registers
 * its own wake event (so the producer wakes it) and, when it publishes a status
 * change, pings the registered UI queues so the icons re-render. */
#define MOD_NOTIFY_SECTION_NAME  L"zune-mod-notify-v1"
#define MOD_NOTIFY_LOCK_NAME     L"zune-mod-notify-lock-v1"
#define MOD_NOTIFY_VERSION       1u
#define MOD_NOTIFY_MAX           16
#define MOD_NOTIFY_NAME_LEN      48
#define MOD_NOTIFY_FREE          0
#define MOD_NOTIFY_UI_QUEUE      1
#define MOD_NOTIFY_DAEMON_EVENT  2

/* This daemon's unique wake-event name, set once via mod_state_daemon_init. */
static wchar_t g_daemon_evt_name[MOD_NOTIFY_NAME_LEN] = { 0 };

typedef struct {
    DWORD   kind;                       /* MOD_NOTIFY_* (FREE = empty) */
    DWORD   owner_pid;
    wchar_t name[MOD_NOTIFY_NAME_LEN];  /* NUL-terminated */
} NotifyConsumer;

typedef struct {
    DWORD          version;
    NotifyConsumer c[MOD_NOTIFY_MAX];
} NotifyBlock;

static NotifyBlock* g_notify      = NULL;
static HANDLE       g_notify_lock = NULL;

void mod_state_daemon_init(const wchar_t* daemon_event_name)
{
    int i;
    if (!daemon_event_name) return;
    for (i = 0; i < MOD_NOTIFY_NAME_LEN; i++) g_daemon_evt_name[i] = 0;
    for (i = 0; i < MOD_NOTIFY_NAME_LEN - 1 && daemon_event_name[i]; i++)
        g_daemon_evt_name[i] = daemon_event_name[i];
}

static ModStateBlock* map_block(void)
{
    HANDLE h;
    if (g_block) return g_block;
    /* CE6 has no OpenFileMapping: CreateFileMappingW by name attaches to the
     * existing section (servicesd is the creator). If WE end up the creator,
     * servicesd hasn't made it yet: drop our zeroed section so it doesn't mask
     * servicesd's first-create seed, and retry next poll. */
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                           sizeof(ModStateBlock), MOD_STATE_SECTION_NAME);
    if (!h) return NULL;
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return NULL;
    }
    g_block = (ModStateBlock*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    return g_block;
}

/* Slot index for `key`, or -1. Keys are NUL-padded but not NUL-terminated at
 * full length; match the bytes and require the slot's next byte (if any) NUL so
 * a prefix can't false-match a longer slot key. */
static int find_slot(ModStateBlock* b, const char* key)
{
    int i, klen = (int)strlen(key);
    if (klen > MOD_STATE_ID_LEN) klen = MOD_STATE_ID_LEN;
    for (i = 0; i < MOD_STATE_MAX_SLOTS; i++) {
        const char* sk = b->slots[i].key;
        if (memcmp(sk, key, (size_t)klen) == 0 &&
            (klen == MOD_STATE_ID_LEN || sk[klen] == '\0'))
            return i;
    }
    return -1;
}

int mod_state_get_state(const char* key)
{
    ModStateBlock* b = map_block();
    int idx;
    if (!b) return -1;
    idx = find_slot(b, key);
    return (idx < 0) ? -1 : (int)b->slots[idx].state;
}

/* CE6 MsgQueue API resolved from coredll at runtime: the SDK carries no import
 * lib for the MsgQueue family (mirrors zuxhook/mods_state_event.c). */
typedef struct {
    DWORD dwSize, dwFlags, dwMaxMessages, cbMaxMessage;
    BOOL  bReadAccess;
} MQ_OPTIONS;
typedef HANDLE (WINAPI *CreateMsgQueueFn)(const wchar_t* name, MQ_OPTIONS* opt);
typedef BOOL   (WINAPI *WriteMsgQueueFn)(HANDLE q, LPVOID buf, DWORD cb, DWORD timeout, DWORD flags);

/* Producer: write a 1-byte ping to the consumer's read queue (content unused;
 * the UI re-pulls the block).
 *
 * The write handle is opened once per queue name and kept open for the process
 * lifetime: a CE point-to-point MsgQueue read handle becomes persistently
 * signalled once its LAST writer closes, which spins the UI host's main loop (it
 * waits on that read handle). So never close the write end per ping. */
static void ping_ui_queue(const wchar_t* name)
{
    static CreateMsgQueueFn p_create = 0;
    static WriteMsgQueueFn  p_write  = 0;
    static struct { wchar_t name[MOD_NOTIFY_NAME_LEN]; HANDLE h; } cache[MOD_NOTIFY_MAX];
    static int cache_n = 0;
    MQ_OPTIONS o;
    HANDLE q = NULL;
    BYTE ping = 1;
    int i;
    if (!p_create) {
        HMODULE c = GetModuleHandleW(L"coredll.dll");
        if (!c) return;
        p_create = (CreateMsgQueueFn)GetProcAddress(c, L"CreateMsgQueue");
        p_write  = (WriteMsgQueueFn) GetProcAddress(c, L"WriteMsgQueue");
    }
    if (!p_create || !p_write) return;
    for (i = 0; i < cache_n; i++)
        if (wcscmp(cache[i].name, name) == 0) { q = cache[i].h; break; }
    if (!q && cache_n < MOD_NOTIFY_MAX) {
        memset(&o, 0, sizeof(o));
        o.dwSize        = sizeof(o);
        o.dwMaxMessages = 16;
        o.cbMaxMessage  = 1;
        o.bReadAccess   = FALSE;            /* write end */
        q = p_create(name, &o);
        if (q) {
            for (i = 0; i < MOD_NOTIFY_NAME_LEN; i++) cache[cache_n].name[i] = 0;
            for (i = 0; i < MOD_NOTIFY_NAME_LEN - 1 && name[i]; i++) cache[cache_n].name[i] = name[i];
            cache[cache_n].h = q;
            cache_n++;
        }
    }
    if (q) p_write(q, &ping, 1, 0, 0);
}

/* Map (creating if absent) the notify registry. Purely additive, so a daemon may
 * create it if it maps first (unlike the state block, where servicesd seeds). */
static NotifyBlock* notify_map(void)
{
    HANDLE h;
    void*  view;
    int    created;
    if (g_notify) return g_notify;
    if (!g_notify_lock)
        g_notify_lock = CreateMutexW(NULL, FALSE, MOD_NOTIFY_LOCK_NAME);
    if (g_notify_lock) WaitForSingleObject(g_notify_lock, INFINITE);
    if (g_notify) { if (g_notify_lock) ReleaseMutex(g_notify_lock); return g_notify; }
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                           sizeof(NotifyBlock), MOD_NOTIFY_SECTION_NAME);
    if (!h) { if (g_notify_lock) ReleaseMutex(g_notify_lock); return NULL; }
    created = (GetLastError() != ERROR_ALREADY_EXISTS);
    view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NotifyBlock));
    if (!view) { CloseHandle(h); if (g_notify_lock) ReleaseMutex(g_notify_lock); return NULL; }
    g_notify = (NotifyBlock*)view;
    if (created && g_notify->version == 0)
        g_notify->version = MOD_NOTIFY_VERSION;
    if (g_notify_lock) ReleaseMutex(g_notify_lock);
    return g_notify;
}

/* Register the daemon's endpoint (dedup by name: a restart reclaims its slot). */
static void notify_register(DWORD kind, const wchar_t* name)
{
    NotifyBlock* nb = notify_map();
    int i, free_idx = -1;
    if (!nb || !name) return;
    if (g_notify_lock) WaitForSingleObject(g_notify_lock, INFINITE);
    for (i = 0; i < MOD_NOTIFY_MAX; i++) {
        if (nb->c[i].kind == MOD_NOTIFY_FREE) { if (free_idx < 0) free_idx = i; continue; }
        if (wcscmp(nb->c[i].name, name) == 0) {
            nb->c[i].kind = kind;
            nb->c[i].owner_pid = GetCurrentProcessId();
            if (g_notify_lock) ReleaseMutex(g_notify_lock);
            return;
        }
    }
    if (free_idx < 0) { if (g_notify_lock) ReleaseMutex(g_notify_lock); return; }
    nb->c[free_idx].kind = kind;
    nb->c[free_idx].owner_pid = GetCurrentProcessId();
    for (i = 0; i < MOD_NOTIFY_NAME_LEN; i++) nb->c[free_idx].name[i] = 0;
    for (i = 0; i < MOD_NOTIFY_NAME_LEN - 1 && name[i]; i++) nb->c[free_idx].name[i] = name[i];
    if (g_notify_lock) ReleaseMutex(g_notify_lock);
}

/* Fan a change out to every registered consumer: UI queues get a ping, daemon
 * events get a SetEvent. Snapshot under the lock, then do the wake I/O unlocked. */
static void notify_publish(void)
{
    NotifyBlock*   nb = notify_map();
    NotifyConsumer snapshot[MOD_NOTIFY_MAX];
    int            n = 0, i;
    if (!nb) return;
    if (g_notify_lock) WaitForSingleObject(g_notify_lock, INFINITE);
    for (i = 0; i < MOD_NOTIFY_MAX; i++)
        if (nb->c[i].kind != MOD_NOTIFY_FREE) snapshot[n++] = nb->c[i];
    if (g_notify_lock) ReleaseMutex(g_notify_lock);
    for (i = 0; i < n; i++) {
        if (snapshot[i].kind == MOD_NOTIFY_UI_QUEUE) {
            ping_ui_queue(snapshot[i].name);
        } else if (snapshot[i].kind == MOD_NOTIFY_DAEMON_EVENT) {
            HANDLE e = CreateEventW(NULL, FALSE, FALSE, snapshot[i].name);
            if (e) { SetEvent(e); CloseHandle(e); }
        }
    }
}

void mod_state_notify(void)
{
    notify_publish();
}

void mod_state_set_status(const char* key, int state)
{
    ModStateBlock* b = map_block();
    int idx;
    BYTE want = (BYTE)(state < 0 ? 0 : state);
    if (!b) return;
    idx = find_slot(b, key);
    if (idx < 0) return;               /* not seeded yet (register_status, Phase 2) */
    if (b->slots[idx].state == want &&
        b->slots[idx].owner_pid == GetCurrentProcessId())
        return;                        /* no change */
    /* The daemon is the sole writer of its status slot and it is pre-seeded, so
     * no assignment lock is needed: the byte + owner writes are aligned. */
    b->slots[idx].state     = want;
    b->slots[idx].owner_pid = GetCurrentProcessId();
    notify_publish();                  /* wake the UI icons + any daemon consumer */
}

HANDLE mod_state_change_event(void)
{
    static HANDLE g_evt = NULL;
    if (!g_evt && g_daemon_evt_name[0]) {
        /* The daemon's own auto-reset event (one waiter). Register it as a daemon
         * endpoint so a producer's fan-out reaches it. */
        g_evt = CreateEventW(NULL, FALSE, FALSE, g_daemon_evt_name);
        if (g_evt) notify_register(MOD_NOTIFY_DAEMON_EVENT, g_daemon_evt_name);
    }
    return g_evt;
}
