#include <windows.h>
#include <string.h>

#include "mod_fault.h"

#define FAULT_BUF_BYTES  512

static int fault_path(const wchar_t* mod_dir, wchar_t* out, int cap) {
    int n = 0, k;
    static const wchar_t leaf[] = L"fault.json";
    if (mod_dir == NULL) return -1;
    while (mod_dir[n] && n < cap - 12) { out[n] = mod_dir[n]; n++; }
    if (n == 0 || n >= cap - 12) return -1;
    if (out[n - 1] != L'\\') out[n++] = L'\\';
    for (k = 0; leaf[k]; k++) out[n++] = leaf[k];
    out[n] = 0;
    return 0;
}

int ModFaultRecord(const wchar_t* mod_dir, const char* mod_version,
                   int phase, int action, const char* cap, unsigned long code) {
    wchar_t  path[MAX_PATH];
    char     buf[FAULT_BUF_BYTES];
    ModFault f;
    HANDLE   h;
    DWORD    w = 0;
    BOOL     ok;
    int      n;

    if (fault_path(mod_dir, path, MAX_PATH) < 0) return -1;

    memset(&f, 0, sizeof(f));
    f.phase  = phase;
    f.action = action;
    f.code   = code;
    strncpy(f.cap, cap ? cap : "", MOD_FAULT_CAP_LEN - 1);
    strncpy(f.version, mod_version ? mod_version : "", MOD_FAULT_VER_LEN - 1);

    n = ModFaultFormat(&f, buf, sizeof(buf));
    if (n < 0) return -1;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = WriteFile(h, buf, (DWORD)n, &w, NULL);
    CloseHandle(h);
    return (ok && w == (DWORD)n) ? 0 : -1;
}

int ModFaultRead(const wchar_t* mod_dir, const char* installed_version,
                 ModFault* out) {
    wchar_t path[MAX_PATH];
    char    buf[FAULT_BUF_BYTES];
    HANDLE  h;
    DWORD   sz, got = 0;

    memset(out, 0, sizeof(*out));
    out->action = -1;
    if (fault_path(mod_dir, path, MAX_PATH) < 0) return 0;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz >= FAULT_BUF_BYTES ||
        !ReadFile(h, buf, sz, &got, NULL)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    return ModFaultParse(buf, got, installed_version, out);
}

void ModFaultClear(const wchar_t* mod_dir) {
    wchar_t path[MAX_PATH];
    if (fault_path(mod_dir, path, MAX_PATH) < 0) return;
    DeleteFileW(path);
}
