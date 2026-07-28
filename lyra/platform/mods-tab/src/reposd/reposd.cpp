/* reposd.exe the Lyra mod-repository daemon. Runs the blocking HTTPS feed
 * fetch and package install off the gemstone UI thread; the Browse UI drives it
 * over a shared section (repo_ipc.h). Mirrors zune-yt's ytsearchd. */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "ce_https.h"
#include "ce_log.h"
#include "repo_ipc.h"
#include "repo_feed.h"
#include "unzip.h"
#include "enabled_set.h"
#include "lyra_platform.h"
#include "zmod_extract.h"
#include "zmod_io.h"
#include "zmod_sha256.h"

/* The pinned official platform channel: the authority for the Lyra update remedy
   (version + provides), fetched here regardless of any feed a user might later
   subscribe to for mods. A fork that runs its own platform points these at its repo. */
#define REPO_HOST       "repo.zune.moe"
#define REPO_FEED_PATH  "/feed.json"
#define AUTOMATION_DIR  L"\\flash2\\automation"
#define MODS_DIR        L"\\flash2\\automation\\mods"
#define TMP_ZMOD        L"\\flash2\\automation\\_repo_dl.zmod"
#define MAX_ZMOD_BYTES  (8u * 1024u * 1024u)   /* hard ceiling against a bad feed */

CE_LOGGER(L, L"\\flash2\\automation\\reposd.log")


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

/* A cert rejected on its dates is the device clock, the one failure here a user can
   fix, so it does not fold in with the other cert verdicts. */
static long classify_feed_failure(enum ce_https_result hr) {
    switch (hr) {
    case CE_HTTPS_ERR_RESOLVE:
    case CE_HTTPS_ERR_CONNECT:
        return REPO_FEED_NO_NET;
    case CE_HTTPS_ERR_CERT:
        return ce_https_cert_failed_on_date() ? REPO_FEED_CLOCK : REPO_FEED_CERT;
    case CE_HTTPS_ERR_TLS:
        return REPO_FEED_TLS;
    case CE_HTTPS_ERR_SEND:
    case CE_HTTPS_ERR_RECV:
    case CE_HTTPS_ERR_PROTOCOL:
        return REPO_FEED_TRANSFER;
    default:
        return REPO_FEED_INTERNAL;
    }
}

/* Only a TLS or cert verdict has a fresh wolfSSL error; for the rest it is stale. */
static long feed_failure_detail(enum ce_https_result hr, long cls) {
    if (cls == REPO_FEED_CERT || cls == REPO_FEED_TLS) return (long)ce_https_last_tls_error();
    return (long)hr;
}

/* reposd serves the catalog only. Whether each row is installed (and at what version)
   is disk truth the UI reads from the scanner; the daemon does not compute it. */
static void do_feed(RepoBlock* blk) {
    struct ce_https_response resp;
    enum ce_https_result hr = ce_https_request(REPO_HOST, REPO_FEED_PATH, "GET",
                                               NULL, NULL, 0, NULL, &resp);
    int ct = 0, tl = 0, rc = 0;
    ce_https_last_timing(&ct, &tl, &rc);
    char line[256];
    blk->feed_error = REPO_FEED_OK; blk->feed_detail = 0;
    if (hr != CE_HTTPS_OK) {
        blk->status = (long)hr; blk->count = 0;
        blk->feed_error = classify_feed_failure(hr);
        blk->feed_detail = feed_failure_detail(hr, blk->feed_error);
        if (hr == CE_HTTPS_ERR_TLS || hr == CE_HTTPS_ERR_CERT)
            _snprintf(line, sizeof(line), "feed %s%s -> %s (connect=%dms tls=%dms tls_err=%d)",
                      REPO_HOST, REPO_FEED_PATH, ce_https_result_str(hr), ct, tl,
                      ce_https_last_tls_error());
        else
            _snprintf(line, sizeof(line), "feed %s%s -> %s (connect=%dms)",
                      REPO_HOST, REPO_FEED_PATH, ce_https_result_str(hr), ct);
        line[sizeof(line) - 1] = 0; L(line);
        return;
    }
    if (resp.status != 200) {
        blk->status = 1000 + resp.status; blk->count = 0;
        blk->feed_error = REPO_FEED_HTTP; blk->feed_detail = resp.status;
        _snprintf(line, sizeof(line), "feed %s%s -> HTTP http=%d (connect=%dms tls=%dms)",
                  REPO_HOST, REPO_FEED_PATH, resp.status, ct, tl);
        line[sizeof(line) - 1] = 0; ce_https_response_free(&resp); L(line);
        return;
    }
    int trunc = 0, pp = 0;
    int n = repo_parse_feed(resp.body, blk->rows, REPO_MAX_ROWS, &trunc,
                            blk->plat_provides, REPO_PLAT_PROV_MAX, &pp);
    unsigned long blen = (unsigned long)resp.body_len;
    ce_https_response_free(&resp);
    blk->count = n; blk->truncated = trunc; blk->plat_provides_count = pp; blk->status = 0;
    blk->feed_error = n > 0 ? REPO_FEED_OK : REPO_FEED_EMPTY;
    /* Hoist the official channel's Lyra version into the platform-authority field, so the
       update remedy does not depend on a `lyra` row surviving in the browsable set. */
    blk->plat_version[0] = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(blk->rows[i].id, LYRA_PLATFORM_ID) == 0) {
            int k;
            for (k = 0; blk->rows[i].version[k] && k < REPO_VERSION_LEN - 1; k++)
                blk->plat_version[k] = blk->rows[i].version[k];
            blk->plat_version[k] = 0;
            break;
        }
    }
    _snprintf(line, sizeof(line),
              "feed %s%s -> OK http=200 rows=%d%s (connect=%dms tls=%dms recv=%dms %luB)",
              REPO_HOST, REPO_FEED_PATH, n, trunc ? " TRUNCATED" : "", ct, tl, rc, blen);
    line[sizeof(line) - 1] = 0; L(line);
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

static void do_install_one(RepoBlock* blk, HANDLE done) {
    char id[REPO_ID_LEN]; strncpy(id, blk->install_id, REPO_ID_LEN - 1); id[REPO_ID_LEN - 1] = 0;
    RepoRow* row = NULL;
    for (int i = 0; i < blk->count; i++) if (strcmp(blk->rows[i].id, id) == 0) { row = &blk->rows[i]; break; }
    if (!row || !row->url[0]) { blk->status = 1; blk->install_status = REPO_INSTALL_ERROR; L("install: unknown id"); return; }
    { char line[128]; _snprintf(line, sizeof(line), "install: start id=%s", id); line[sizeof(line) - 1] = 0; L(line); }

    blk->install_total = row->size; blk->install_done = 0;
    blk->install_status = REPO_INSTALL_FETCHING; SetEvent(done);
    DeleteFileW(TMP_ZMOD);
    int st = 0; unsigned long got = 0;
    enum ce_https_result hr = ce_https_download_url(row->url, NULL, TMP_ZMOD, MAX_ZMOD_BYTES, &st, &got);
    if (hr != CE_HTTPS_OK || st != 200 || got == 0) {
        DeleteFileW(TMP_ZMOD); blk->status = (long)hr; blk->install_status = REPO_INSTALL_ERROR;
        char line[192];
        _snprintf(line, sizeof(line), "install: download fail id=%s %s http=%d got=%lu",
                  id, ce_https_result_str(hr), st, got);
        line[sizeof(line) - 1] = 0; L(line);
        return;
    }
    blk->install_done = got;

    blk->install_status = REPO_INSTALL_VERIFYING; SetEvent(done);
    char hex[REPO_SHA_LEN];
    if (!zmod_sha256_file(TMP_ZMOD, hex) || !zmod_sha256_hex_equal(hex, row->sha256)) {
        DeleteFileW(TMP_ZMOD); blk->status = 2; blk->install_status = REPO_INSTALL_ERROR;
        L("install: sha256 mismatch"); return;
    }

    blk->install_status = REPO_INSTALL_UNPACKING; SetEvent(done);

    if (is_platform(row)) {
        /* Platform bundle: extract into the automation root itself - the zip paths
         * are the real relative device paths (binaries at top, platform mods under
         * platform\<id>\, lyra.json marker). No wipe_except: the root holds other mods,
         * logs, and state. zmod_extract renames the in-use zuxhook.dll/nativeapp.exe/
         * reposd.exe aside; the boot .old sweep clears them. Takes effect on reboot. */
        static const char* const defer_last[] = LYRA_INSTALL_DEFER_LAST;
        zmod_error ze;
        if (!zmod_extract(TMP_ZMOD, AUTOMATION_DIR, defer_last,
                          LYRA_INSTALL_DEFER_LAST_COUNT, &ze)) {
            DeleteFileW(TMP_ZMOD); blk->status = 3; blk->install_status = REPO_INSTALL_ERROR;
            L("install: platform unpack fail stage=%d rc=%ld entry=%s", ze.stage, ze.rc, ze.entry);
            return;
        }
        DeleteFileW(TMP_ZMOD);
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
    { zmod_error ze;
      if (!zmod_extract(TMP_ZMOD, dest, NULL, 0, &ze)) {
          DeleteFileW(TMP_ZMOD); blk->status = 3; blk->install_status = REPO_INSTALL_ERROR;
          L("install: unpack fail stage=%d rc=%ld entry=%s", ze.stage, ze.rc, ze.entry);
          return;
      } }
    DeleteFileW(TMP_ZMOD);

    blk->install_status = REPO_INSTALL_ENABLING; SetEvent(done);
    EnabledSetAdd(id);

    blk->status = 0; blk->install_status = REPO_INSTALL_DONE;
    L("install: done");
}

/* Install install_set[0..count-1] in order (dependencies first, target last). Only the last
   member's DONE is surfaced as terminal (the main loop's done event); a member's ERROR aborts
   the rest, leaving whatever fully installed before it. */
static void do_install_set(RepoBlock* blk, HANDLE done) {
    int n = blk->install_set_count, i;
    if (n < 1 || n > REPO_MAX_INSTALL_SET) {
        blk->status = 1; blk->install_status = REPO_INSTALL_ERROR; L("install set: bad count"); return;
    }
    for (i = 0; i < n; i++) {
        blk->install_set_index = i;
        strncpy(blk->install_id, blk->install_set[i], REPO_ID_LEN - 1);
        blk->install_id[REPO_ID_LEN - 1] = 0;
        do_install_one(blk, done);
        if (blk->install_status == REPO_INSTALL_ERROR) {
            char line[128];
            _snprintf(line, sizeof(line), "install set: aborted at %d/%d id=%s",
                      i + 1, n, blk->install_id);
            line[sizeof(line) - 1] = 0; L(line);
            return;
        }
    }
}

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, LPWSTR c, int d) {
    (void)a; (void)b; (void)c; (void)d;
    L("=== reposd start ===");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }

    RepoBlock* blk = map_block();
    if (!blk) { L("map_block failed"); return -1; }
    if (blk->version == 0) blk->version = REPO_VERSION;
    blk->daemon_started = 1;

    HANDLE wake = CreateEventW(NULL, FALSE, FALSE, REPO_WAKE_EVENT);   /* auto-reset */
    HANDLE done = CreateEventW(NULL, FALSE, FALSE, REPO_DONE_EVENT);   /* auto-reset */
    if (!wake || !done) { L("event create failed"); return -2; }

    for (;;) {
        WaitForSingleObject(wake, INFINITE);
        long seq = blk->req_seq;
        long req = blk->request;
        if (req == REPO_REQ_FEED)           do_feed(blk);
        else if (req == REPO_REQ_INSTALL)   do_install_set(blk, done);
        else if (req == REPO_REQ_UNINSTALL) do_uninstall(blk);
        blk->done_seq = seq;
        SetEvent(done);
    }
}
