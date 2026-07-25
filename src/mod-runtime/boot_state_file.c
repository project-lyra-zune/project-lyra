#include <windows.h>
#include <string.h>

#include "boot_state.h"

#define BOOT_STATE_PATH  L"\\flash2\\automation\\boot.state"

static int boot_state_write(const BootState* st) {
    char   buf[BOOT_STATE_BUF_BYTES];
    HANDLE h;
    DWORD  w = 0;
    BOOL   ok;
    int    n;

    n = BootStateFormat(st, buf, sizeof(buf));
    if (n < 0) return -1;

    /* Never write-new-then-rename: a torn CREATE_ALWAYS leaves a truncated
       marker, which holds the next boot at SAFE; a torn rename leaves none,
       which reads as a clean history and reapplies the wedging mod forever. */
    h = CreateFileW(BOOT_STATE_PATH, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = WriteFile(h, buf, (DWORD)n, &w, NULL);
    CloseHandle(h);
    return (ok && w == (DWORD)n) ? 0 : -1;
}

void BootStateRead(BootState* out) {
    char   buf[BOOT_STATE_BUF_BYTES];
    HANDLE h;
    DWORD  sz, got = 0;

    memset(out, 0, sizeof(*out));
    out->level = BOOT_LEVEL_NORMAL;

    h = CreateFileW(BOOT_STATE_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz >= BOOT_STATE_BUF_BYTES ||
        !ReadFile(h, buf, sz, &got, NULL)) {
        CloseHandle(h);
        out->level = BOOT_LEVEL_SAFE;
        return;
    }
    CloseHandle(h);
    BootStateParse(buf, got, out);
}

BootLevel BootStateBeginBoot(void) {
    BootState st;
    BootLevel next;

    BootStateRead(&st);
    next             = BootLevelDemote(st.level, st.failures);
    st.failures      = (next == st.level) ? st.failures + 1 : 1;
    st.level         = next;
    st.shell_started = 0;
    boot_state_write(&st);
    return st.level;
}

void BootStateCommit(void) {
    BootState st;

    BootStateRead(&st);
    st.failures = 0;
    boot_state_write(&st);
}

/* Small on purpose: an idle loop can block for a long time between iterations. */
#define BOOT_COMMIT_TICKS  8

static int process_is_shell(void) {
    wchar_t  path[MAX_PATH];
    wchar_t* base;
    DWORD    len = GetModuleFileNameW(NULL, path, MAX_PATH);

    if (len == 0 || len >= MAX_PATH) return 0;
    base = path + len;
    while (base > path && *(base - 1) != L'\\') base--;
    return _wcsicmp(base, L"gemstone.exe") == 0;
}

static volatile LONG g_apply_done = 0;   /* Phase 2 worker thread to UI thread */

void BootStateApplyComplete(void) {
    InterlockedExchange(&g_apply_done, 1);
}

void BootStateTick(void) {
    static int claimed = 0;   /* UI thread only; no locking */
    static int ticks   = 0;
    static int done    = 0;

    if (done) return;

    /* Claim before the apply gate, not after: the claim is what marks a shell
       instance as having run, and a respawn must be caught even when the one
       before it died mid-apply. */
    if (!claimed) {
        BootState st;
        /* servicesd pumping proves the HUD host is alive, not that the device
           is usable; only the shell's loop counts. */
        if (!process_is_shell()) { done = 1; return; }
        /* A shell already ran this boot, so this process is a respawn and the
           one before it died. That is not a boot worth committing. */
        BootStateRead(&st);
        if (st.shell_started) { done = 1; return; }
        st.shell_started = 1;
        boot_state_write(&st);
        claimed = 1;
    }

    if (!g_apply_done) return;
    if (++ticks < BOOT_COMMIT_TICKS) return;
    BootStateCommit();
    done = 1;
}

void BootStateClear(void) {
    BootState st;

    memset(&st, 0, sizeof(st));
    st.level = BOOT_LEVEL_NORMAL;
    boot_state_write(&st);
}
