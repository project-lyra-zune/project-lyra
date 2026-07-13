#include "mods_state_block.h"
#include "mods_log.h"
#include "kerncore.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Held open for the process lifetime: closing the section handle would
   invalidate the mapped view that consumers keep reading. zuxhook never
   unloads, so there is no teardown path to free these from. */
static ModStateBlock* g_block   = NULL;
static HANDLE         g_section = NULL;
static HANDLE         g_lock    = NULL;

/* ── logging (per-pid; two host processes write the block, so a single shared
      file would interleave/contend across processes) ──────────────────────── */

static void slog(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    va_list ap;

    _snwprintf(path, MAX_PATH - 1,
               L"\\flash2\\automation\\mods\\mod-state-%lu.log",
               GetCurrentProcessId());
    path[MAX_PATH - 1] = 0;

    va_start(ap, fmt);
    mods_vflashlog(path, fmt, ap);
    va_end(ap);
}

/* ── key helpers (fixed NUL-padded field vs C string) ─────────────────────── */

static void pack_key(char out[MOD_STATE_ID_LEN], const char* src) {
    int i;
    for (i = 0; i < MOD_STATE_ID_LEN; i++) out[i] = 0;
    for (i = 0; i < MOD_STATE_ID_LEN && src[i]; i++) out[i] = src[i];
}

static int key_eq(const char* slot_key, const char* src) {
    char want[MOD_STATE_ID_LEN];
    pack_key(want, src);
    return memcmp(slot_key, want, MOD_STATE_ID_LEN) == 0;
}

/* A slot's role is the namespace prefix of its key. Only `status/` slots carry
   a live owner pid and are subject to dead-owner reaping. */
static int key_role_is_status(const char* slot_key) {
    return strncmp(slot_key, "status/", 7) == 0;
}

/* ── lock ─────────────────────────────────────────────────────────────────── */

static void state_lock(void)   { if (g_lock) WaitForSingleObject(g_lock, INFINITE); }
static void state_unlock(void) { if (g_lock) ReleaseMutex(g_lock); }

/* ── map ──────────────────────────────────────────────────────────────────── */

ModStateBlock* ModStateMap(void) {
    HANDLE sec;
    void*  view;
    int    created;

    if (g_block) return g_block;

    if (g_lock == NULL)
        g_lock = CreateMutexW(NULL, FALSE, MOD_STATE_LOCK_NAME);
    state_lock();

    /* A second thread may have mapped while we waited on the lock. */
    if (g_block) { state_unlock(); return g_block; }

    sec = CreateFileMappingW((HANDLE)INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                             0, sizeof(ModStateBlock), MOD_STATE_SECTION_NAME);
    if (sec == NULL) {
        slog(L"CreateFileMapping failed err=%lu", GetLastError());
        state_unlock();
        return NULL;
    }
    created = (GetLastError() != ERROR_ALREADY_EXISTS);

    view = MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ModStateBlock));
    if (view == NULL) {
        slog(L"MapViewOfFile failed err=%lu", GetLastError());
        CloseHandle(sec);
        state_unlock();
        return NULL;
    }

    g_section = sec;
    g_block   = (ModStateBlock*)view;

    /* Pagefile-backed sections are zero-filled on first creation, so
       version==0 reliably distinguishes a fresh block from one a prior
       process already initialised. */
    if (created && g_block->version == 0) {
        g_block->version = MOD_STATE_VERSION;
        g_block->count   = 0;
        slog(L"created+initialised (pid=%lu va=0x%08x sz=%u)", GetCurrentProcessId(),
             (unsigned)(DWORD)g_block, (unsigned)sizeof(ModStateBlock));
    } else {
        slog(L"mapped existing (pid=%lu va=0x%08x sz=%u version=%lu count=%lu)",
             GetCurrentProcessId(), (unsigned)(DWORD)g_block, (unsigned)sizeof(ModStateBlock),
             (unsigned long)g_block->version, (unsigned long)g_block->count);
    }

    state_unlock();
    return g_block;
}

/* ── slot find / assign (caller holds the lock) ───────────────────────────── */

static int find_or_assign(ModStateBlock* b, const char* key, int assign,
                          int init_state, DWORD init_owner, int* was_new) {
    int i, free_idx = -1;
    if (was_new) *was_new = 0;
    for (i = 0; i < MOD_STATE_MAX_SLOTS; i++) {
        if (b->slots[i].key[0] == 0) { if (free_idx < 0) free_idx = i; continue; }
        if (key_eq(b->slots[i].key, key)) return i;
    }
    if (!assign || free_idx < 0) return -1;

    pack_key(b->slots[free_idx].key, key);
    b->slots[free_idx].state     = (BYTE)(init_state < 0 ? 0 : init_state);
    b->slots[free_idx]._pad[0]   = 0;
    b->slots[free_idx]._pad[1]   = 0;
    b->slots[free_idx]._pad[2]   = 0;
    b->slots[free_idx].owner_pid = init_owner;
    if ((DWORD)(free_idx + 1) > b->count) b->count = (DWORD)(free_idx + 1);
    if (was_new) *was_new = 1;
    return free_idx;
}

/* ── public API ───────────────────────────────────────────────────────────── */

int ModStateSlotIndex(const char* key, int assign) {
    ModStateBlock* b = ModStateMap();
    int idx, was_new = 0;
    if (!b || !key) return -1;
    state_lock();
    idx = find_or_assign(b, key, assign, 0, 0, &was_new);
    state_unlock();
    if (was_new) slog(L"assigned slot %d -> %S", idx, key);
    return idx;
}

int ModStateGetState(const char* key) {
    ModStateBlock* b = ModStateMap();
    int idx;
    if (!b) return -1;
    idx = ModStateSlotIndex(key, 0);
    if (idx < 0) return -1;
    return (int)b->slots[idx].state;   /* aligned single-byte read; no lock */
}

void ModStateSetState(const char* key, int state, DWORD owner_pid) {
    ModStateBlock* b = ModStateMap();
    int idx, was_new = 0, changed;
    BYTE want = (BYTE)(state < 0 ? 0 : state);
    if (!b || !key) return;

    /* Assign + write under the lock: the assign-then-mutate sequence must be
       atomic against a concurrent writer in the other host process. */
    state_lock();
    idx = find_or_assign(b, key, 1, want, owner_pid, &was_new);
    if (idx >= 0 && !was_new) {
        changed = (b->slots[idx].state != want) ||
                  (b->slots[idx].owner_pid != owner_pid);
        b->slots[idx].state     = want;
        b->slots[idx].owner_pid = owner_pid;
    } else {
        changed = (idx >= 0);   /* a freshly-assigned slot is a change */
    }
    state_unlock();

    if (idx < 0) { slog(L"set %S: table full", key); return; }
    if (!changed) return;       /* no change → caller skips the notify */
    slog(L"set %S state=%d owner=0x%lx", key, (int)want, (unsigned long)owner_pid);
    /* The change notification rides ModStateEventPublish at the call site (one
       fan-out path to every registered consumer); this writer does not signal. */
}

void ModStateSeed(const char* key, int state, DWORD owner_pid) {
    ModStateBlock* b = ModStateMap();
    int idx, was_new = 0;
    if (!b || !key) return;
    state_lock();
    idx = find_or_assign(b, key, 1, state, owner_pid, &was_new);
    state_unlock();
    if (idx < 0) {
        slog(L"seed %S: table full", key);
    } else if (was_new) {
        slog(L"seed %S: slot %d state=%d", key, idx, state < 0 ? 0 : state);
    } else {
        slog(L"seed %S: slot %d present (state=%d) - left live",
             key, idx, (int)b->slots[idx].state);
    }
}

int ModStateReapDeadOwners(void) {
    ModStateBlock* b = ModStateMap();
    int i, reset = 0;
    if (!b) return 0;
    if (!kerncore_is_ready()) return 0;   /* PID liveness needs kerncore */
    state_lock();
    for (i = 0; i < MOD_STATE_MAX_SLOTS; i++) {
        DWORD pid = b->slots[i].owner_pid;
        if (b->slots[i].key[0] == 0 || pid == 0) continue;
        if (!key_role_is_status(b->slots[i].key)) continue;
        if (kerncore_find_proc_struct(pid) != 0) continue;   /* owner alive */
        if (b->slots[i].state != 0) {
            slog(L"reap status '%S' owner=0x%lx dead -> state 0",
                 b->slots[i].key, (unsigned long)pid);
            b->slots[i].state = 0;
            reset++;
        }
        b->slots[i].owner_pid = 0;   /* clear so a re-acquire re-stamps a live owner */
    }
    state_unlock();
    /* The reaper thread (mods_phase2.c) calls ModStateEventPublish when this
       returns > 0; the reset wake rides that one fan-out path. */
    return reset;
}

void ModStateLogSnapshot(const wchar_t* role) {
    ModStateBlock* b = ModStateMap();
    int i;
    if (!b) { slog(L"snapshot[%s]: block unavailable", role ? role : L"?"); return; }
    slog(L"snapshot[%s]: version=%lu count=%lu", role ? role : L"?",
         (unsigned long)b->version, (unsigned long)b->count);
    state_lock();
    for (i = 0; i < MOD_STATE_MAX_SLOTS; i++) {
        char id[MOD_STATE_ID_LEN + 1];   /* key isn't NUL-terminated at full length */
        if (b->slots[i].key[0] == 0) continue;
        memcpy(id, b->slots[i].key, MOD_STATE_ID_LEN);
        id[MOD_STATE_ID_LEN] = 0;
        slog(L"  slot %d: '%S' state=%d owner=0x%lx",
             i, id, (int)b->slots[i].state, (unsigned long)b->slots[i].owner_pid);
    }
    state_unlock();
}
