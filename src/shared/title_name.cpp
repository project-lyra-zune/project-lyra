#include <windows.h>
#include <string.h>
#include "title_name.h"

// Set the XNA title's Apps-list display name through the media service. A raw
// \Flash2\zunedb.dat edit does not stick (the service owns the DB and flushes over it) and
// the .zcp container is not re-indexed on boot, so the service API is the only edit that
// takes. See project_zos_zmdb_title_rename_setstring and the wiki media-database page.
//
//   ZDKMedia_Item_GetFilePath export = 0x4197af20   (VA-slide anchor; slide 0 on device)
//   ZDKMedia_Item_GetName export     = 0x4197aec8   get_name(handle, buf, cap)
//   set-string entry (unexported)    = 0x4198474c   set_str(handle, propId, wstr)
// Name attribute id = 0x20001.
//
// The item handle encodes the container's storage slot, so it is not stable across deploys.
// \Flash2\Content\<AAAA>\<BB>\<CC>.zcp maps to (AAAA<<16)|(BB<<8)|CC, AAAA = (kind<<8)|bucket.
// Resolve it at runtime from the live kind-0x13 container carrying GUID 584E07D1.
#define ANCHOR_VA   0x4197af20u
#define SETSTR_VA   0x4198474cu
#define GETNAME_VA  0x4197aec8u
#define PROP_NAME   0x20001u

typedef int (*FN_setstr)(unsigned handle, unsigned propId, const wchar_t* wstr);
typedef int (*FN_getname)(unsigned handle, wchar_t* buf, int cap);

static unsigned parse_hex_w(const wchar_t* s) {
    unsigned v = 0;
    for (; *s; s++) {
        wchar_t c = *s;
        unsigned d;
        if (c >= L'0' && c <= L'9') d = (unsigned)(c - L'0');
        else if (c >= L'a' && c <= L'f') d = 10u + (unsigned)(c - L'a');
        else if (c >= L'A' && c <= L'F') d = 10u + (unsigned)(c - L'A');
        else break;   // stops at the '.' before an extension
        v = (v << 4) | d;
    }
    return v;
}

// True if the .zcp carries our container GUID. The EXEC block holds it as the ASCII id
// "584E07D1..." near the file start, so scanning the first 64 KB suffices.
static int zcp_is_ours(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    static char buf[65536];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf), &got, NULL);
    CloseHandle(h);
    if (!ok || got < 8) return 0;
    static const char guid[8] = { '5','8','4','E','0','7','D','1' };
    for (DWORD i = 0; i + 8 <= got; i++)
        if (memcmp(buf + i, guid, 8) == 0) return 1;
    return 0;
}

static FN_getname resolve_getname(void) {
    HMODULE zdk = LoadLibraryW(L"zdksystem.dll");
    if (zdk == NULL) return NULL;
    void* anchor = (void*)GetProcAddress(zdk, L"ZDKMedia_Item_GetFilePath");
    if (anchor == NULL) return NULL;
    return (FN_getname)(GETNAME_VA + ((unsigned)anchor - ANCHOR_VA));
}

// A prior install's .zcp can stay on disk with no live DB record, so a GUID match alone
// can hit an orphan; accept a handle only if the media DB answers a name query for it
// (S_OK is live, 0x803A0002 is not-found). If the query fn can't be resolved, accept.
static int handle_is_live(FN_getname get, unsigned handle) {
    if (get == NULL) return 1;
    wchar_t nm[128]; int hr = -1;
    __try { hr = get(handle, nm, 128); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
    return hr == 0;
}

// Resolve the kind-0x13 title's current zmdb handle by scanning the content store for the
// container carrying GUID 584E07D1 whose record is live. Returns 0 if none.
static unsigned FindTitleHandle(void) {
    WIN32_FIND_DATAW da, db, dc;
    wchar_t pat[MAX_PATH], zcp[MAX_PATH];
    unsigned handle = 0;
    FN_getname get = resolve_getname();

    HANDLE ha = FindFirstFileW(L"\\Flash2\\Content\\13*", &da);
    if (ha == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(da.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || da.cFileName[0] == L'.') continue;
        unsigned aaaa = parse_hex_w(da.cFileName);
        _snwprintf(pat, MAX_PATH - 1, L"\\Flash2\\Content\\%s\\*", da.cFileName);
        pat[MAX_PATH - 1] = 0;
        HANDLE hb = FindFirstFileW(pat, &db);
        if (hb == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(db.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || db.cFileName[0] == L'.') continue;
            unsigned bb = parse_hex_w(db.cFileName);
            _snwprintf(pat, MAX_PATH - 1, L"\\Flash2\\Content\\%s\\%s\\*.zcp", da.cFileName, db.cFileName);
            pat[MAX_PATH - 1] = 0;
            HANDLE hc = FindFirstFileW(pat, &dc);
            if (hc == INVALID_HANDLE_VALUE) continue;
            do {
                unsigned cc = parse_hex_w(dc.cFileName);
                _snwprintf(zcp, MAX_PATH - 1, L"\\Flash2\\Content\\%s\\%s\\%s",
                           da.cFileName, db.cFileName, dc.cFileName);
                zcp[MAX_PATH - 1] = 0;
                if (zcp_is_ours(zcp)) {
                    unsigned h = (aaaa << 16) | (bb << 8) | (cc & 0xff);
                    if (handle_is_live(get, h)) { handle = h; break; }
                }
            } while (FindNextFileW(hc, &dc));
            FindClose(hc);
            if (handle) break;
        } while (FindNextFileW(hb, &db));
        FindClose(hb);
        if (handle) break;
    } while (FindNextFileW(ha, &da));
    FindClose(ha);
    return handle;
}

// Force the media service to write \Flash2\zunedb.dat now. A property write is otherwise
// flushed on the service's own schedule, which a forced reset skips, so a rename made just
// before the reboot would be lost. ZME0: 0x200 method 0x23 is the commit case; its buffer
// must be exactly 12 bytes or the handler is skipped without flushing.
static void ZmdbFlush(void) {
    HANDLE h = CreateFileW(L"ZME0:", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    unsigned char buf[0x0c];
    memset(buf, 0, sizeof(buf));
    *(unsigned short*)(buf + 0x00) = 0x0023;
    DWORD ret = 0;
    __try { DeviceIoControl(h, 0x200, buf, sizeof(buf), buf, sizeof(buf), &ret, NULL); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    CloseHandle(h);
}

// Best-effort and never throws. Each set-string attempt is guarded on its own, so a
// transient fault while the media service is still coming up just retries.
void SetTitleName(const wchar_t* name) {
    unsigned handle = 0;
    FN_setstr set_str = NULL;
    __try {
        handle = FindTitleHandle();
        if (handle == 0) return;

        HMODULE zdk = LoadLibraryW(L"zdksystem.dll");
        if (zdk == NULL) return;
        void* anchor = (void*)GetProcAddress(zdk, L"ZDKMedia_Item_GetFilePath");
        if (anchor == NULL) return;
        set_str = (FN_setstr)(SETSTR_VA + ((unsigned)anchor - ANCHOR_VA));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    for (int attempt = 0; attempt < 10; attempt++) {
        int hr = -1;
        __try { hr = set_str(handle, PROP_NAME, name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
        if (hr == 0) { ZmdbFlush(); return; }
        Sleep(500);
    }
}
