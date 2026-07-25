#include <windows.h>
#include <string.h>
#include <stdio.h>

#include "failed_set.h"

#define FAILED_PATH  L"\\flash2\\automation\\mods\\failed.ids"
#define FAILED_BUF   1024

void FailedSetClear(void) {
    DeleteFileW(FAILED_PATH);
}

void FailedSetAdd(const char* mod_id) {
    HANDLE h;
    DWORD w = 0;
    char line[ENABLED_ID_LEN + 2];
    int n;

    if (!mod_id || !mod_id[0]) return;
    n = _snprintf(line, sizeof(line), "%s\n", mod_id);
    if (n <= 0) return;
    h = CreateFileW(FAILED_PATH, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, line, (DWORD)n, &w, NULL);
    CloseHandle(h);
}

int FailedSetContains(const char* mod_id) {
    char buf[FAILED_BUF];
    HANDLE h;
    DWORD sz, got = 0;

    h = CreateFileW(FAILED_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz >= FAILED_BUF ||
        !ReadFile(h, buf, sz, &got, NULL)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    buf[got] = 0;
    return FailedSetBufContains(buf, mod_id);
}
