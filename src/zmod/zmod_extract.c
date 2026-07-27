#include <windows.h>
#include <string.h>
#include "unzip.h"
#include "zmod_io.h"
#include "zmod_extract.h"

static void mkdirs_parents(const wchar_t* full) {
    wchar_t tmp[MAX_PATH];
    wchar_t* p;
    wcsncpy(tmp, full, MAX_PATH - 1); tmp[MAX_PATH - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == L'\\') { *p = 0; CreateDirectoryW(tmp, NULL); *p = L'\\'; }
    }
}

static int name_equal_ci(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y) return 0;
    }
    return *a == 0 && *b == 0;
}

static int is_deferred(const char* name, const char* const* defer, int n) {
    int i;
    for (i = 0; i < n; i++) if (name_equal_ci(name, defer[i])) return 1;
    return 0;
}

static void fail(zmod_error* err, int stage, long rc, const char* entry) {
    if (!err) return;
    err->stage = stage; err->rc = rc;
    err->entry[0] = 0;
    if (entry) { strncpy(err->entry, entry, sizeof(err->entry) - 1); err->entry[sizeof(err->entry) - 1] = 0; }
}

static int write_entry(unzFile uf, const wchar_t* dest, const char* name, zmod_error* err) {
    static BYTE buf[16384];
    wchar_t out[MAX_PATH];
    HANDLE hf;
    int o = 0, i, r;
    DWORD w;

    for (i = 0; dest[i] && o < MAX_PATH - 2; i++) out[o++] = dest[i];
    out[o++] = L'\\';
    for (i = 0; name[i] && o < MAX_PATH - 1; i++)
        out[o++] = (name[i] == '/') ? L'\\' : (wchar_t)(unsigned char)name[i];
    out[o] = 0;

    if (o > 0 && out[o - 1] == L'\\') {
        mkdirs_parents(out); CreateDirectoryW(out, NULL);
        return 1;
    }
    mkdirs_parents(out);

    r = unzOpenCurrentFile(uf);
    if (r != UNZ_OK) { fail(err, ZMOD_STAGE_OPENENTRY, r, name); return 0; }

    hf = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        wchar_t oldp[MAX_PATH]; int k = 0;
        for (; out[k] && k < MAX_PATH - 5; k++) oldp[k] = out[k];
        oldp[k] = L'.'; oldp[k + 1] = L'o'; oldp[k + 2] = L'l'; oldp[k + 3] = L'd'; oldp[k + 4] = 0;
        DeleteFileW(oldp);
        MoveFileW(out, oldp);
        hf = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (hf == INVALID_HANDLE_VALUE) {
        fail(err, ZMOD_STAGE_CREATEFILE, (long)GetLastError(), name);
        unzCloseCurrentFile(uf);
        return 0;
    }

    while ((r = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) WriteFile(hf, buf, r, &w, NULL);
    CloseHandle(hf);
    unzCloseCurrentFile(uf);
    if (r < 0) { fail(err, ZMOD_STAGE_READ, r, name); return 0; }
    return 1;
}

static int pass(unzFile uf, const wchar_t* dest, const char* const* defer, int n_defer,
                int want_deferred, zmod_error* err) {
    int rc = unzGoToFirstFile(uf);
    if (rc != UNZ_OK) { fail(err, ZMOD_STAGE_FIRSTFILE, rc, NULL); return 0; }
    do {
        char name[512];
        unz_file_info64 info;
        int gi = unzGetCurrentFileInfo64(uf, &info, name, sizeof(name), NULL, 0, NULL, 0);
        if (gi != UNZ_OK) { fail(err, ZMOD_STAGE_GETINFO, gi, NULL); return 0; }
        if (is_deferred(name, defer, n_defer) != want_deferred) continue;
        if (!write_entry(uf, dest, name, err)) return 0;
    } while (unzGoToNextFile(uf) == UNZ_OK);
    return 1;
}

int zmod_extract(const wchar_t* zmod, const wchar_t* dest,
                 const char* const* defer_last, int n_defer, zmod_error* err) {
    zlib_filefunc64_def ff;
    unzFile uf;
    int ok;

    fail(err, ZMOD_STAGE_NONE, 0, NULL);
    fill_ce_filefunc64W(&ff);
    uf = unzOpen2_64((const void*)zmod, &ff);
    if (!uf) { fail(err, ZMOD_STAGE_OPEN, 0, NULL); return 0; }

    ok = pass(uf, dest, defer_last, n_defer, 0, err);
    if (ok && n_defer > 0) ok = pass(uf, dest, defer_last, n_defer, 1, err);

    unzClose(uf);
    return ok;
}
