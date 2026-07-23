#include "repo_client.h"
#include <windows.h>

/* coredll ord871 (MsgWaitForMultipleObjectsEx) import slot in the gemstone image;
   patched so a reposd DONE wakes the pump thread and we deliver on it. */
#define MSGWAIT_IAT_GEMSTONE  0x00096244u
#define MWMO_WAITALL          0x00000001u

typedef DWORD (WINAPI *MsgWaitFn)(DWORD, const HANDLE*, DWORD, DWORD, DWORD);

static MsgWaitFn  g_orig_wait = 0;
static RepoBlock* g_repo      = 0;   /* daemon writes, we read */
static HANDLE     g_wake      = 0;
static HANDLE     g_done      = 0;
static void     (*g_on_done)(void) = 0;

RepoBlock* RepoClientBlock(void) { return g_repo; }
void RepoClientSetOnDone(void (*cb)(void)) { g_on_done = cb; }

void RepoClientRequestFeed(void) {
    if (!g_repo || !g_wake) return;
    g_repo->request = REPO_REQ_FEED;
    InterlockedIncrement(&g_repo->req_seq);
    SetEvent(g_wake);
}

static void copy_id(char* dst, const char* id) {
    int i = 0;
    for (; id[i] && i < REPO_ID_LEN - 1; i++) dst[i] = id[i];
    dst[i] = 0;
}

static void set_install_id(const char* id) { copy_id(g_repo->install_id, id); }

/* Every install is an ordered set the daemon runs in one request; a plain install is a
   set of one. */
static void request_install_set(const char (*ids)[REPO_ID_LEN], int n) {
    int i;
    if (!g_repo || !g_wake || n < 1) return;
    if (n > REPO_MAX_INSTALL_SET) n = REPO_MAX_INSTALL_SET;
    for (i = 0; i < n; i++) copy_id(g_repo->install_set[i], ids[i]);
    g_repo->install_set_count = n;
    g_repo->install_set_index = 0;
    set_install_id(ids[0]);
    g_repo->install_status = REPO_INSTALL_IDLE;
    g_repo->request = REPO_REQ_INSTALL;
    InterlockedIncrement(&g_repo->req_seq);
    SetEvent(g_wake);
}

void RepoClientRequestInstall(const char* id) {
    char one[1][REPO_ID_LEN];
    if (!id || !id[0]) return;
    copy_id(one[0], id);
    request_install_set(one, 1);
}

void RepoClientRequestInstallSet(const char (*ids)[REPO_ID_LEN], int n) {
    request_install_set(ids, n);
}

void RepoClientRequestUninstall(const char* id) {
    if (!g_repo || !g_wake || !id || !id[0]) return;
    set_install_id(id);
    g_repo->install_status = REPO_INSTALL_IDLE;
    g_repo->request = REPO_REQ_UNINSTALL;
    InterlockedIncrement(&g_repo->req_seq);
    SetEvent(g_wake);
}

/* Append g_done to the pump's wait set; on its signal, deliver on this (UI) thread
   and report a timeout so the pump loops without treating it as one of its handles. */
static DWORD WINAPI MsgWait_proxy(DWORD count, const HANDLE* handles,
                                  DWORD ms, DWORD mask, DWORD flags) {
    HANDLE local[64];
    DWORD r, i;
    if (g_orig_wait == 0) return WAIT_FAILED;
    if (g_done == 0 || count == 0 || count >= 63 || (flags & MWMO_WAITALL))
        return g_orig_wait(count, handles, ms, mask, flags);
    for (i = 0; i < count; i++) local[i] = handles[i];
    local[count] = g_done;
    r = g_orig_wait(count + 1, local, ms, mask, flags);
    if (r == WAIT_OBJECT_0 + count) { if (g_on_done) g_on_done(); return WAIT_TIMEOUT; }
    if (r == WAIT_OBJECT_0 + count + 1) return WAIT_OBJECT_0 + count;
    return r;
}

static RepoBlock* map_repo_section(void) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                  sizeof(RepoBlock), REPO_SECTION_NAME);
    RepoBlock* b;
    if (!h) return 0;
    b = (RepoBlock*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (b && b->version == 0) b->version = REPO_VERSION;
    return b;
}

void RepoClientInstall(void) {
    g_repo = map_repo_section();
    g_wake = CreateEventW(NULL, FALSE, FALSE, REPO_WAKE_EVENT);
    g_done = CreateEventW(NULL, FALSE, FALSE, REPO_DONE_EVENT);
    if (!g_done) return;
    __try {
        DWORD orig = *(volatile DWORD*)MSGWAIT_IAT_GEMSTONE;
        g_orig_wait = (MsgWaitFn)orig;
        *(volatile DWORD*)MSGWAIT_IAT_GEMSTONE = (DWORD)&MsgWait_proxy;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
