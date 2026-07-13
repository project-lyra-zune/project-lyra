/* reposd.exe the Lyra mod-repository daemon. Runs the blocking HTTPS feed
 * fetch and package install off the gemstone UI thread; the Browse UI drives it
 * over a shared section (repo_ipc.h). Mirrors zune-yt's ytsearchd. */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "ce_https.h"
#include "repo_ipc.h"
#include "repo_feed.h"
#include "unzip.h"
#include "repo_ceio.h"
#include "enabled_set.h"
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>

#define REPO_HOST       "repo.zune.moe"
#define REPO_FEED_PATH  "/feed.json"
#define AUTOMATION_DIR  L"\\flash2\\automation"
#define MODS_DIR        L"\\flash2\\automation\\mods"
#define TMP_ZMOD        L"\\flash2\\automation\\_repo_dl.zmod"
#define LOG_PATH        L"\\flash2\\automation\\reposd.log"
#define MAX_ZMOD_BYTES  (8u * 1024u * 1024u)   /* hard ceiling against a bad feed */

static void L(const char* s) {
    HANDLE f = CreateFileW(LOG_PATH, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    DWORD n; WriteFile(f, s, (DWORD)strlen(s), &n, NULL); WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}
static void Lx(const char* t, long v) { char b[160]; _snprintf(b, sizeof(b), "%s=%ld", t, v); L(b); }

static void ascii_to_wide(const char* s, wchar_t* out, int cap) {
    int o = 0; for (int i = 0; s[i] && o < cap - 1; i++) out[o++] = (wchar_t)(unsigned char)s[i];
    out[o] = 0;
}

static RepoBlock* map_block(void) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                  sizeof(RepoBlock), REPO_SECTION_NAME);
    if (!h) return NULL;
    return (RepoBlock*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
}

/* The platform is the reserved catalog id `lyra`; everything else is a feature mod. */
static int is_platform(const RepoRow* r) { return strcmp(r->id, LYRA_PLATFORM_ID) == 0; }

/* reposd serves the catalog only. Whether each row is installed (and at what version)
   is disk truth the UI reads from the scanner; the daemon does not compute it. */
static void do_feed(RepoBlock* blk) {
    struct ce_https_response resp;
    enum ce_https_result hr = ce_https_request(REPO_HOST, REPO_FEED_PATH, "GET",
                                               NULL, NULL, 0, NULL, &resp);
    if (hr != CE_HTTPS_OK) { blk->status = (long)hr; blk->count = 0; L(ce_https_result_str(hr)); return; }
    if (resp.status != 200) { blk->status = 1000 + resp.status; blk->count = 0; ce_https_response_free(&resp); return; }
    int trunc = 0;
    int n = repo_parse_feed(resp.body, blk->rows, REPO_MAX_ROWS, &trunc);
    ce_https_response_free(&resp);
    blk->count = n; blk->truncated = trunc; blk->status = 0;
    Lx("feed rows", n);
    if (trunc) L("feed truncated (more mods than REPO_MAX_ROWS)");
}

static int sha256_file(const wchar_t* path, char* hex_out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    wc_Sha256 s; wc_InitSha256(&s);
    static BYTE buf[16384]; DWORD n;
    while (ReadFile(h, buf, sizeof(buf), &n, NULL) && n > 0) wc_Sha256Update(&s, buf, n);
    CloseHandle(h);
    BYTE dig[32]; wc_Sha256Final(&s, dig);
    static const char* HX = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { hex_out[i * 2] = HX[dig[i] >> 4]; hex_out[i * 2 + 1] = HX[dig[i] & 0xf]; }
    hex_out[64] = 0;
    return 1;
}

static void mkdirs_parents(const wchar_t* full) {
    wchar_t tmp[MAX_PATH]; wcsncpy(tmp, full, MAX_PATH - 1); tmp[MAX_PATH - 1] = 0;
    for (wchar_t* p = tmp + 1; *p; p++) {
        if (*p == L'\\') { *p = 0; CreateDirectoryW(tmp, NULL); *p = L'\\'; }
    }
}

static int extract_zmod(const wchar_t* zmod, const wchar_t* dest) {
    zlib_filefunc64_def ff; fill_ce_filefunc64W(&ff);
    unzFile uf = unzOpen2_64((const void*)zmod, &ff);
    if (!uf) { L("unpack: unzOpen2_64 NULL"); return 0; }
    int rc = unzGoToFirstFile(uf);
    if (rc != UNZ_OK) { Lx("unpack: firstfile rc", rc); unzClose(uf); return 0; }

    int ok = 1;
    do {
        char name[512]; unz_file_info64 info;
        int gi = unzGetCurrentFileInfo64(uf, &info, name, sizeof(name), NULL, 0, NULL, 0);
        if (gi != UNZ_OK) { Lx("unpack: getinfo rc", gi); ok = 0; break; }

        wchar_t out[MAX_PATH]; int o = 0;
        for (int i = 0; dest[i] && o < MAX_PATH - 2; i++) out[o++] = dest[i];
        out[o++] = L'\\';
        for (int i = 0; name[i] && o < MAX_PATH - 1; i++)
            out[o++] = (name[i] == '/') ? L'\\' : (wchar_t)(unsigned char)name[i];
        out[o] = 0;

        if (o > 0 && out[o - 1] == L'\\') { mkdirs_parents(out); CreateDirectoryW(out, NULL); continue; }
        mkdirs_parents(out);

        int oc = unzOpenCurrentFile(uf);
        if (oc != UNZ_OK) { Lx("unpack: opencur rc", oc); ok = 0; break; }
        HANDLE hf = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
            /* The target is in use, i.e. a mod's boot-spawned daemon binary being
             * updated live. Rename it aside so the new bytes land now; the running
             * image keeps its old file until the next boot spawns the fresh one. */
            wchar_t oldp[MAX_PATH]; int k = 0;
            for (; out[k] && k < MAX_PATH - 5; k++) oldp[k] = out[k];
            oldp[k] = L'.'; oldp[k+1] = L'o'; oldp[k+2] = L'l'; oldp[k+3] = L'd'; oldp[k+4] = 0;
            DeleteFileW(oldp);
            MoveFileW(out, oldp);
            hf = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (hf == INVALID_HANDLE_VALUE) { Lx("unpack: createfile err", (long)GetLastError()); unzCloseCurrentFile(uf); ok = 0; break; }
        static BYTE buf[16384]; int r; DWORD w;
        while ((r = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) WriteFile(hf, buf, r, &w, NULL);
        CloseHandle(hf); unzCloseCurrentFile(uf);
        if (r < 0) { Lx("unpack: read rc", r); ok = 0; break; }
    } while (unzGoToNextFile(uf) == UNZ_OK);

    unzClose(uf);
    return ok;
}

#define REPO_PERSIST_MAX  8
#define REPO_GLOB_LEN     64

/* Read manifest.json out of the .zmod into out. Returns 1 if found + non-empty. */
static int read_zmod_manifest(const wchar_t* zmod, char* out, int cap) {
    zlib_filefunc64_def ff; fill_ce_filefunc64W(&ff);
    unzFile uf = unzOpen2_64((const void*)zmod, &ff);
    int n = 0;
    if (!uf) { out[0] = 0; return 0; }
    if (unzLocateFile(uf, "manifest.json", 0) == UNZ_OK && unzOpenCurrentFile(uf) == UNZ_OK) {
        int r;
        while (n < cap - 1 && (r = unzReadCurrentFile(uf, out + n, cap - 1 - n)) > 0) n += r;
        unzCloseCurrentFile(uf);
    }
    unzClose(uf);
    out[n] = 0;
    return n > 0;
}

/* Extract the manifest's "persistent": ["glob", ...] entries into globs. */
static void parse_persistent(const char* json, char globs[][REPO_GLOB_LEN], int maxg, int* ng) {
    *ng = 0;
    const char* p = strstr(json, "\"persistent\"");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    const char* end = strchr(p, ']');
    if (!end) return;
    while (p < end && *ng < maxg) {
        const char* q = strchr(p, '"');
        if (!q || q >= end) break;
        q++;
        int o = 0;
        while (*q && *q != '"' && q < end && o < REPO_GLOB_LEN - 1) globs[*ng][o++] = *q++;
        globs[*ng][o] = 0;
        if (o > 0) (*ng)++;
        p = (*q) ? q + 1 : end;
    }
}

/* Wildcard match ('*' any run incl '/', '?' one char), ASCII case-insensitive. */
static int glob_match(const char* pat, const char* str) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (; *str; str++) if (glob_match(pat, str)) return 1;
            return glob_match(pat, str);
        } else if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
        } else {
            char a = *pat, b = *str;
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return 0;
            pat++; str++;
        }
    }
    return *str == 0;
}

/* Remove every file under a mod dir whose path (relative to rootlen, '/'-joined)
 * matches no persistent glob. A file in use (the running daemon binary) can't be
 * deleted, so rename it aside as <file>.old; the boot .old sweep clears it. Empty
 * subdirs are removed; a dir kept alive by a persistent file stays. No-op if the
 * dir is absent (a fresh install). */
static void wipe_except(const wchar_t* dir, int rootlen, char globs[][REPO_GLOB_LEN], int ng) {
    wchar_t pat[MAX_PATH]; WIN32_FIND_DATAW fd;
    _snwprintf(pat, MAX_PATH - 1, L"%s\\*", dir); pat[MAX_PATH - 1] = 0;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        wchar_t child[MAX_PATH];
        _snwprintf(child, MAX_PATH - 1, L"%s\\%s", dir, fd.cFileName); child[MAX_PATH - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wipe_except(child, rootlen, globs, ng);
            RemoveDirectoryW(child);   /* removed only if now empty */
        } else {
            char rel[MAX_PATH]; int o = 0;
            for (int i = rootlen + 1; child[i] && o < MAX_PATH - 1; i++)
                rel[o++] = (child[i] == L'\\') ? '/' : (char)(unsigned char)child[i];
            rel[o] = 0;
            int keep = 0;
            for (int g = 0; g < ng; g++) if (glob_match(globs[g], rel)) { keep = 1; break; }
            if (keep) continue;
            if (!DeleteFileW(child)) {
                wchar_t oldp[MAX_PATH]; int k = 0;
                for (; child[k] && k < MAX_PATH - 5; k++) oldp[k] = child[k];
                oldp[k] = L'.'; oldp[k+1] = L'o'; oldp[k+2] = L'l'; oldp[k+3] = L'd'; oldp[k+4] = 0;
                DeleteFileW(oldp);
                MoveFileW(child, oldp);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void do_uninstall(RepoBlock* blk) {
    char id[REPO_ID_LEN]; strncpy(id, blk->install_id, REPO_ID_LEN - 1); id[REPO_ID_LEN - 1] = 0;
    EnabledSetRemove(id);
    blk->status = 0; blk->install_status = REPO_INSTALL_DONE;
    L("uninstall: done");
}

static void do_install(RepoBlock* blk, HANDLE done) {
    char id[REPO_ID_LEN]; strncpy(id, blk->install_id, REPO_ID_LEN - 1); id[REPO_ID_LEN - 1] = 0;
    RepoRow* row = NULL;
    for (int i = 0; i < blk->count; i++) if (strcmp(blk->rows[i].id, id) == 0) { row = &blk->rows[i]; break; }
    if (!row || !row->url[0]) { blk->status = 1; blk->install_status = REPO_INSTALL_ERROR; L("install: unknown id"); return; }
    L(id);

    blk->reboot_required = 0;
    blk->install_total = row->size; blk->install_done = 0;
    blk->install_status = REPO_INSTALL_FETCHING; SetEvent(done);
    DeleteFileW(TMP_ZMOD);
    int st = 0; unsigned long got = 0;
    enum ce_https_result hr = ce_https_download_url(row->url, NULL, TMP_ZMOD, MAX_ZMOD_BYTES, &st, &got);
    if (hr != CE_HTTPS_OK || st != 200 || got == 0) {
        DeleteFileW(TMP_ZMOD); blk->status = (long)hr; blk->install_status = REPO_INSTALL_ERROR;
        Lx("install: download fail http", st); return;
    }
    blk->install_done = got;

    blk->install_status = REPO_INSTALL_VERIFYING; SetEvent(done);
    char hex[REPO_SHA_LEN];
    if (!sha256_file(TMP_ZMOD, hex) || _stricmp(hex, row->sha256) != 0) {
        DeleteFileW(TMP_ZMOD); blk->status = 2; blk->install_status = REPO_INSTALL_ERROR;
        L("install: sha256 mismatch"); return;
    }

    blk->install_status = REPO_INSTALL_UNPACKING; SetEvent(done);

    if (is_platform(row)) {
        /* Platform bundle: extract into the automation root itself - the zip paths
         * are the real relative device paths (binaries at top, platform mods under
         * platform\<id>\, lyra.json marker). No wipe_except: the root holds other mods,
         * logs, and state. extract_zmod renames the in-use zuxhook.dll/nativeapp.exe/
         * reposd.exe aside; the boot .old sweep clears them. Takes effect on reboot. */
        if (!extract_zmod(TMP_ZMOD, AUTOMATION_DIR)) {
            DeleteFileW(TMP_ZMOD); blk->status = 3; blk->install_status = REPO_INSTALL_ERROR;
            L("install: platform unpack fail"); return;
        }
        DeleteFileW(TMP_ZMOD);
        blk->reboot_required = 1;
        blk->status = 0; blk->install_status = REPO_INSTALL_DONE;
        L("install: platform done, reboot required");
        return;
    }

    wchar_t idw[REPO_ID_LEN], dest[MAX_PATH];
    ascii_to_wide(id, idw, REPO_ID_LEN);
    _snwprintf(dest, MAX_PATH - 1, L"%s\\%s", MODS_DIR, idw);
    /* Update-safe unpack: wipe the existing mod dir down to only its declared
     * persistent files (rename any in-use daemon binary aside), so files removed
     * between versions do not linger and config/state survives. A fresh install
     * finds nothing to wipe. Then extract the new version over the clean dir. */
    { char mjson[4096]; char globs[REPO_PERSIST_MAX][REPO_GLOB_LEN]; int ng = 0;
      if (read_zmod_manifest(TMP_ZMOD, mjson, sizeof(mjson))) parse_persistent(mjson, globs, REPO_PERSIST_MAX, &ng);
      wipe_except(dest, (int)wcslen(dest), globs, ng); }
    CreateDirectoryW(MODS_DIR, NULL); CreateDirectoryW(dest, NULL);
    if (!extract_zmod(TMP_ZMOD, dest)) {
        DeleteFileW(TMP_ZMOD); blk->status = 3; blk->install_status = REPO_INSTALL_ERROR;
        L("install: unpack fail"); return;
    }
    DeleteFileW(TMP_ZMOD);

    blk->install_status = REPO_INSTALL_ENABLING; SetEvent(done);
    EnabledSetAdd(id);

    blk->status = 0; blk->install_status = REPO_INSTALL_DONE;
    L("install: done");
}

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, LPWSTR c, int d) {
    (void)a; (void)b; (void)c; (void)d;
    L("=== reposd start ===");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }

    RepoBlock* blk = map_block();
    if (!blk) { L("map_block failed"); return -1; }
    if (blk->version == 0) blk->version = REPO_VERSION;

    HANDLE wake = CreateEventW(NULL, FALSE, FALSE, REPO_WAKE_EVENT);   /* auto-reset */
    HANDLE done = CreateEventW(NULL, FALSE, FALSE, REPO_DONE_EVENT);   /* auto-reset */
    if (!wake || !done) { L("event create failed"); return -2; }

    for (;;) {
        WaitForSingleObject(wake, INFINITE);
        long seq = blk->req_seq;
        long req = blk->request;
        if (req == REPO_REQ_FEED)           do_feed(blk);
        else if (req == REPO_REQ_INSTALL)   do_install(blk, done);
        else if (req == REPO_REQ_UNINSTALL) do_uninstall(blk);
        blk->done_seq = seq;
        SetEvent(done);
    }
}
