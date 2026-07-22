/* Mod-manager UI scanner. See mod_scanner.h.

   Enumerates \flash2\automation\mods\* via FindFirstFileW, parses each
   subdir's manifest.json with the existing mods_json/mods_manifest
   tokenizer, cross-references the enabled set (via enabled_set), and
   converts the minimal set of fields the UI renders (mod_id, name,
   version) from manifest UTF-8 into UTF-16.

   The row set is a projection of disk state, rebuilt whenever the Manage
   scene is entered (ModScanRebuild), not a boot snapshot. reposd mutates
   enabled.json + the mods dirs live from the Browse UI. Held-back annotation
   is the one genuinely boot-time input (the resolver runs only at boot), so
   it lives in a small persistent table built once and consulted on every rebuild. */

#include <windows.h>
#include <string.h>
#include "mod_scanner.h"
#include "mods_arena.h"
#include "mods_json.h"
#include "mods_manifest.h"   /* ModSet */
#include "enabled_set.h"

#define MODS_ROOT_PATH        L"\\flash2\\automation\\mods"
#define PLATFORM_ROOT_PATH    L"\\flash2\\automation\\platform"
#define AUTOMATION_ROOT_PATH  L"\\flash2\\automation"
/* Rebuildable scan arena: source bytes of every mod's manifest + the
   token array each one parses into. The JSON parser's token buffer
   grows by doubling (256 -> 512 -> 1024 -> ...) and leaks intermediate
   buffers in the arena, so a single large manifest (~22 KB, ~880 tokens,
   ~50 KB cumulative scan footprint with doublings) can exhaust a small
   arena and trip arena.oom, which is sticky and silently fails every
   subsequent mod's load. This walk parses every manifest on disk (the
   only pass that surfaces archived mods, which ModsManifestLoadAll
   skips), so size for the whole mods/ directory. */
#define SCAN_ARENA_BYTES  (2 * 1024 * 1024)
#define MAX_FILE_BYTES    (64 * 1024)

#define HELDBACK_MAX         32
#define HELDBACK_REASON_LEN  64

typedef struct {
    char mod_id[ENABLED_ID_LEN];
    char reason[HELDBACK_REASON_LEN];
} HeldBackEntry;

/* Boot-persistent held-back table: never freed, built once from the
   resolver's result. Separate from the scan arena so ModScanRebuild can
   free + re-walk the filesystem without losing resolver context. */
static HeldBackEntry g_heldback[HELDBACK_MAX];
static int           g_heldback_count = 0;
static int           g_heldback_built = 0;

static ModsArena g_arena;         /* rebuilt each ModScanRebuild */
static int       g_arena_live = 0;
static ModRow*   g_rows  = NULL;
static int       g_count = 0;
static ModRowSet g_set   = { NULL, 0 };

/* Slurp a UTF-8 file into arena-owned memory. Returns NULL on error
   or empty. Sets *out_len to the byte length (no NUL terminator). */
static char* slurp_file(ModsArena* arena, const wchar_t* path,
                        DWORD* out_len) {
    HANDLE h;
    DWORD size, got = 0;
    char* buf;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > MAX_FILE_BYTES) {
        CloseHandle(h);
        return NULL;
    }
    buf = (char*)ModsArenaAlloc(arena, size + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    buf[size] = 0;
    *out_len = size;
    return buf;
}

/* Convert a manifest JSON string token to an arena-allocated UTF-16
   wchar_t*. Goes through ModsJsonStrdup first so backslash-escape
   sequences (\uXXXX, \n, ...) decode into their actual UTF-8 bytes,
   then MultiByteToWideChar maps UTF-8 -> UTF-16. Returns NULL on
   failure. */
static const wchar_t* json_strdup_w(ModsArena* arena, const ModsJson* j,
                                     int tok_idx) {
    const char* decoded;
    int byte_len, wchars, written;
    wchar_t* out;

    if (tok_idx < 0 || j->toks[tok_idx].type != MODS_JSON_STRING) return NULL;
    decoded = ModsJsonStrdup(arena, j, tok_idx);
    if (!decoded) return NULL;
    byte_len = (int)strlen(decoded);
    if (byte_len <= 0) {
        out = (wchar_t*)ModsArenaAlloc(arena, sizeof(wchar_t));
        if (!out) return NULL;
        out[0] = 0;
        return out;
    }
    wchars = MultiByteToWideChar(CP_UTF8, 0, decoded, byte_len, NULL, 0);
    if (wchars <= 0) return NULL;
    out = (wchar_t*)ModsArenaAlloc(arena, (wchars + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    written = MultiByteToWideChar(CP_UTF8, 0, decoded, byte_len, out, wchars);
    if (written <= 0) return NULL;
    out[written] = 0;
    return out;
}

/* Compare a UTF-16 string to an ASCII one (mod_id is ASCII by construction). */
static int wide_eq_ascii(const wchar_t* w, const char* a) {
    if (!w || !a) return 0;
    while (*w && *a) {
        if (*w != (wchar_t)(unsigned char)*a) return 0;
        w++; a++;
    }
    return *w == 0 && *a == 0;
}

/* Truncate a UTF-16 mod_id into an ASCII buffer (mod_ids are ASCII). */
static void wide_to_ascii(const wchar_t* w, char* out, int cap) {
    int i = 0;
    if (!w) { out[0] = 0; return; }
    for (; w[i] && i < cap - 1; i++) out[i] = (char)(w[i] & 0x7F);
    out[i] = 0;
}

/* 1 if mod_id (as wstring) is in the enabled-id array read from disk. */
static int is_enabled(const wchar_t* mod_id,
                      const char ids[][ENABLED_ID_LEN], int id_count) {
    char a[ENABLED_ID_LEN];
    wide_to_ascii(mod_id, a, ENABLED_ID_LEN);
    return EnabledSetContains(ids, id_count, a);
}

/* The manifest's experimental flag (author-declared unfinished). */
static int manifest_is_experimental(const ModsJson* j, int root) {
    int b = 0;
    return (ModsJsonBool(j, ModsJsonObjectFind(j, root, "experimental"), &b) == 0 && b) ? 1 : 0;
}

/* Read \flash2\automation\mods\<dir>\manifest.json, parse minimal
   fields into `row`. Returns 0 on success, -1 on failure. */
static int load_one_row(ModsArena* arena, const wchar_t* mod_dir,
                         const char ids[][ENABLED_ID_LEN], int id_count,
                         ModRow* row) {
    wchar_t path[MAX_PATH];
    char* src;
    DWORD src_len = 0;
    ModsJson mj;
    int root, mod_id_tok, name_tok, version_tok, author_tok, desc_tok;

    _snwprintf(path, MAX_PATH - 1, L"%s\\manifest.json", mod_dir);
    path[MAX_PATH - 1] = 0;

    src = slurp_file(arena, path, &src_len);
    if (!src) return -1;

    if (ModsJsonParse(arena, src, src_len, &mj) < 0) return -1;
    root = 0;
    if (mj.toks[root].type != MODS_JSON_OBJECT) return -1;

    row->is_platform = 0;   /* mods\ holds feature mods; the platform is a synthetic row */

    mod_id_tok = ModsJsonObjectFind(&mj, root, "mod_id");
    if (mod_id_tok < 0) return -1;

    row->id   = json_strdup_w(arena, &mj, mod_id_tok);
    if (!row->id) return -1;

    name_tok = ModsJsonObjectFind(&mj, root, "name");
    row->name = (name_tok >= 0) ? json_strdup_w(arena, &mj, name_tok) : row->id;
    if (!row->name) row->name = row->id;

    /* Pre-compose "<name> (disabled)" so the get-item handler just picks
       by enabled flag, with no per-render allocation. */
    {
        static const wchar_t kSuffix[] = L" (disabled)";
        size_t name_chars = wcslen(row->name);
        size_t total = name_chars + (sizeof(kSuffix) / sizeof(wchar_t));
        wchar_t* composed = (wchar_t*)ModsArenaAlloc(arena, total * sizeof(wchar_t));
        if (composed) {
            memcpy(composed, row->name, name_chars * sizeof(wchar_t));
            memcpy(composed + name_chars, kSuffix, sizeof(kSuffix));
            row->name_disabled = composed;
        } else {
            row->name_disabled = row->name;
        }
    }

    version_tok = ModsJsonObjectFind(&mj, root, "version");
    row->version = (version_tok >= 0) ? json_strdup_w(arena, &mj, version_tok)
                                       : NULL;

    author_tok = ModsJsonObjectFind(&mj, root, "author");
    row->author = (author_tok >= 0) ? json_strdup_w(arena, &mj, author_tok)
                                     : NULL;

    desc_tok = ModsJsonObjectFind(&mj, root, "description");
    row->description = (desc_tok >= 0) ? json_strdup_w(arena, &mj, desc_tok)
                                        : NULL;

    row->enabled = is_enabled(row->id, ids, id_count);
    row->experimental = manifest_is_experimental(&mj, root);
    row->source  = MOD_SOURCE_LOCAL;
    return 0;
}

/* Build L"<name> (held back: <reason>)" in the arena; reason is ASCII. */
static const wchar_t* compose_held_back(ModsArena* arena, const wchar_t* name,
                                        const char* reason) {
    static const wchar_t pre[] = L" (held back: ";
    size_t nl = name ? wcslen(name) : 0;
    size_t pl = sizeof(pre) / sizeof(pre[0]) - 1;
    size_t rl = reason ? strlen(reason) : 0;
    size_t p = 0, i;
    wchar_t* out = (wchar_t*)ModsArenaAlloc(arena,
                       (nl + pl + rl + 2) * sizeof(wchar_t));
    if (!out) return name;
    for (i = 0; i < nl; i++) out[p++] = name[i];
    for (i = 0; i < pl; i++) out[p++] = pre[i];
    for (i = 0; i < rl; i++) out[p++] = (wchar_t)(unsigned char)reason[i];
    out[p++] = L')';
    out[p] = 0;
    return out;
}

#define LYRA_MARKER_PATH  L"\\flash2\\automation\\lyra.json"

/* Copy a wide literal into the scan arena so a row can own it. */
static const wchar_t* arena_wdup(ModsArena* arena, const wchar_t* s) {
    int n = 0; wchar_t* out;
    while (s[n]) n++;
    out = (wchar_t*)ModsArenaAllocZ(arena, (n + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    { int i; for (i = 0; i < n; i++) out[i] = s[i]; }
    return out;
}

/* Append the one synthetic Lyra platform row from the lyra.json version marker. The
   platform is not a mods\ dir; its platform mods (in platform\) are folded into this
   single row and never surfaced individually. Absent marker -> no row added. */
static void append_lyra_row(ModsArena* arena, ModRow* row, int* loaded) {
    char* src; DWORD n = 0; ModsJson mj; int vt, nt, at, dt;
    src = slurp_file(arena, LYRA_MARKER_PATH, &n);
    if (!src) return;
    if (ModsJsonParse(arena, src, n, &mj) < 0) return;
    if (mj.toks[0].type != MODS_JSON_OBJECT) return;
    vt = ModsJsonObjectFind(&mj, 0, "version");
    nt = ModsJsonObjectFind(&mj, 0, "name");
    at = ModsJsonObjectFind(&mj, 0, "author");
    dt = ModsJsonObjectFind(&mj, 0, "description");
    row->id             = arena_wdup(arena, L"lyra");
    row->name           = (nt >= 0) ? json_strdup_w(arena, &mj, nt) : arena_wdup(arena, L"Lyra");
    if (!row->name) row->name = arena_wdup(arena, L"Lyra");
    row->name_disabled  = row->name;
    row->version        = (vt >= 0) ? json_strdup_w(arena, &mj, vt) : NULL;
    row->author         = (at >= 0) ? json_strdup_w(arena, &mj, at) : NULL;
    row->description    = (dt >= 0) ? json_strdup_w(arena, &mj, dt) : NULL;
    row->enabled        = 1;
    row->held_back      = 0;
    row->name_held_back = NULL;
    row->is_platform    = 1;
    row->experimental   = 0;
    row->source         = MOD_SOURCE_LOCAL;
    if (row->id) (*loaded)++;
}

static void build_rows(void) {
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pattern[MAX_PATH];
    char enabled_ids[ENABLED_ID_MAX][ENABLED_ID_LEN];
    int enabled_count;
    int discovered = 0, loaded = 0;

    g_rows = NULL;
    g_count = 0;
    g_set.rows = NULL;
    g_set.count = 0;

    if (ModsArenaInit(&g_arena, SCAN_ARENA_BYTES) < 0) return;
    g_arena_live = 1;

    enabled_count = EnabledSetRead(enabled_ids, ENABLED_ID_MAX);
    if (enabled_count < 0) enabled_count = 0;   /* malformed -> treat as none enabled */

    /* First pass: count subdirectories so we can size the row array. */
    _snwprintf(pattern, MAX_PATH - 1, L"%s\\*", MODS_ROOT_PATH);
    pattern[MAX_PATH - 1] = 0;

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) goto done;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        discovered++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    /* Room for every feature mod under mods\ plus the one synthetic Lyra row. */
    g_rows = (ModRow*)ModsArenaAllocZ(&g_arena, (discovered + 1) * sizeof(ModRow));
    if (!g_rows) goto done;

    /* Second pass: parse + populate the feature mods. mods\ holds only feature mods
       now (the platform's platform mods live in platform\), so there is nothing to
       exclude. Failures are tolerated: a subdir without a valid manifest just isn't
       surfaced. */
    if (discovered > 0) {
        h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t mod_dir[MAX_PATH];
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == L'.' &&
                    (fd.cFileName[1] == 0 ||
                     (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
                if (loaded >= discovered) break;
                _snwprintf(mod_dir, MAX_PATH - 1, L"%s\\%s",
                            MODS_ROOT_PATH, fd.cFileName);
                mod_dir[MAX_PATH - 1] = 0;
                if (load_one_row(&g_arena, mod_dir, enabled_ids, enabled_count,
                                  &g_rows[loaded]) == 0)
                    loaded++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    /* Overlay the boot-time held-back verdict: an enabled mod the resolver
       couldn't satisfy this boot renders with its reason. The table is the
       resolver's single result; mods installed after boot aren't in it and
       show as plain enabled (pending the next boot's apply). */
    {
        int hi, ri;
        for (hi = 0; hi < g_heldback_count; hi++) {
            for (ri = 0; ri < loaded; ri++) {
                if (wide_eq_ascii(g_rows[ri].id, g_heldback[hi].mod_id)) {
                    g_rows[ri].held_back = 1;
                    g_rows[ri].name_held_back = compose_held_back(
                        &g_arena, g_rows[ri].name, g_heldback[hi].reason);
                    break;
                }
            }
        }
    }

    /* The platform itself, folded into one row from its lyra.json marker. */
    append_lyra_row(&g_arena, &g_rows[loaded], &loaded);

done:
    g_count = loaded;
    g_set.rows  = g_rows;
    g_set.count = g_count;
}

static void build_heldback_table(const ModSet* resolved) {
    int si;
    if (g_heldback_built) return;
    g_heldback_built = 1;
    if (!resolved) return;
    for (si = 0; si < resolved->count && g_heldback_count < HELDBACK_MAX; si++) {
        if (!resolved->mods[si].disabled) continue;
        strncpy(g_heldback[g_heldback_count].mod_id,
                resolved->mods[si].mod_id, ENABLED_ID_LEN - 1);
        g_heldback[g_heldback_count].mod_id[ENABLED_ID_LEN - 1] = 0;
        if (resolved->mods[si].disabled_reason) {
            strncpy(g_heldback[g_heldback_count].reason,
                    resolved->mods[si].disabled_reason, HELDBACK_REASON_LEN - 1);
            g_heldback[g_heldback_count].reason[HELDBACK_REASON_LEN - 1] = 0;
        } else {
            g_heldback[g_heldback_count].reason[0] = 0;
        }
        g_heldback_count++;
    }
}

void ModScanBuild(const ModSet* resolved) {
    build_heldback_table(resolved);
    ModScanRebuild();
}

void ModScanRebuild(void) {
    if (g_arena_live) {
        ModsArenaFree(&g_arena);
        g_arena_live = 0;
    }
    build_rows();
}

const ModRowSet* ModScanGet(void) {
    return &g_set;
}

int ModScanToggleEnabled(int idx) {
    char id[ENABLED_ID_LEN];
    int want;
    if (idx < 0 || idx >= g_count || !g_rows) return 0;
    /* System mods are platform shell, never user-toggled. Defense in depth:
       they are already excluded from the catalog by build_rows. */
    if (g_rows[idx].is_platform) return 0;
    want = !g_rows[idx].enabled;
    wide_to_ascii(g_rows[idx].id, id, ENABLED_ID_LEN);
    if ((want ? EnabledSetAdd(id) : EnabledSetRemove(id)) < 0) return 0;
    g_rows[idx].enabled = want;
    return 1;
}

/* Recursively delete every file under `dir`, then `dir` itself. CE 6 has
   no tree-delete, so FindFirstFile/FindNextFile and recurse on subdirs.
   Returns 0 on success, -1 if any operation fails. */
static int rmtree(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int rc = 0;
    _snwprintf(pattern, MAX_PATH - 1, L"%s\\*", dir);
    pattern[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t child[MAX_PATH];
            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == 0 ||
                 (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            _snwprintf(child, MAX_PATH - 1, L"%s\\%s", dir, fd.cFileName);
            child[MAX_PATH - 1] = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (rmtree(child) < 0) rc = -1;
            } else if (!DeleteFileW(child)) {
                /* In use, i.e. a running daemon binary. Rename it aside so removal
                 * makes progress now; the boot-time .old sweep clears it once the
                 * process is gone. */
                wchar_t oldp[MAX_PATH]; int k = 0;
                for (; child[k] && k < MAX_PATH - 5; k++) oldp[k] = child[k];
                oldp[k] = L'.'; oldp[k+1] = L'o'; oldp[k+2] = L'l'; oldp[k+3] = L'd'; oldp[k+4] = 0;
                DeleteFileW(oldp);
                if (!MoveFileW(child, oldp)) rc = -1;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir)) rc = -1;
    return rc;
}

int ModScanDelete(int idx) {
    wchar_t dir[MAX_PATH];
    char id[ENABLED_ID_LEN];
    if (idx < 0 || idx >= g_count || !g_rows) return 0;
    if (g_rows[idx].is_platform) return 0;
    wide_to_ascii(g_rows[idx].id, id, ENABLED_ID_LEN);
    _snwprintf(dir, MAX_PATH - 1, L"%s\\%s",
                MODS_ROOT_PATH, g_rows[idx].id);
    dir[MAX_PATH - 1] = 0;
    /* Drop it from the enabled set first: the mod leaves Manage and won't be
     * re-applied or re-spawned on the next boot, even if a running daemon binary
     * can only be renamed aside now (rmtree does that; the boot .old sweep and
     * this dir's removal complete once the process is gone). */
    EnabledSetRemove(id);
    rmtree(dir);
    /* Slide remaining rows down. The arena owns each row's wstrings;
       we leak the deleted row's storage on purpose. the arena is
       rebuilt on the next Manage entry, and a delete is rare enough that
       no in-place GC is worth the bookkeeping. */
    {
        int i;
        for (i = idx; i < g_count - 1; i++) g_rows[i] = g_rows[i + 1];
    }
    g_count--;
    g_set.count = g_count;
    return 1;
}

/* Delete every *.old file under `dir` and remove any subdir left empty. Returns
   1 if `dir` itself ended up with no remaining entries. */
static int sweep_old(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int any = 0;
    _snwprintf(pattern, MAX_PATH - 1, L"%s\\*", dir);
    pattern[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        wchar_t child[MAX_PATH];
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        _snwprintf(child, MAX_PATH - 1, L"%s\\%s", dir, fd.cFileName);
        child[MAX_PATH - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (sweep_old(child)) { if (!RemoveDirectoryW(child)) any = 1; }
            else any = 1;
        } else {
            int n = 0; while (fd.cFileName[n]) n++;
            if (n >= 4 && fd.cFileName[n-4] == L'.' &&
                (fd.cFileName[n-3] | 32) == L'o' &&
                (fd.cFileName[n-2] | 32) == L'l' &&
                (fd.cFileName[n-1] | 32) == L'd') {
                if (!DeleteFileW(child)) any = 1;
            } else {
                any = 1;   /* a real file keeps the dir alive */
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return !any;
}

void ModScanSweepOld(void) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pattern, MAX_PATH - 1, L"%s\\*", MODS_ROOT_PATH);
    pattern[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t child[MAX_PATH];
        if (fd.cFileName[0] == L'.') continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        _snwprintf(child, MAX_PATH - 1, L"%s\\%s", MODS_ROOT_PATH, fd.cFileName);
        child[MAX_PATH - 1] = 0;
        if (sweep_old(child)) RemoveDirectoryW(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Clear the rename-aside leftovers a Lyra update leaves outside mods\ (which the
   mods sweep above covers): the platform binaries' *.old at the automation root
   (zuxhook.dll.old, nativeapp.exe.old), and the platform-mod daemons' *.old under
   platform\<id>\ (e.g. reposd.exe.old). */
void ModScanSweepPlatformOld(void) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    /* Automation-root *.old (files only, non-recursive). */
    _snwprintf(pattern, MAX_PATH - 1, L"%s\\*.old", AUTOMATION_ROOT_PATH);
    pattern[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t child[MAX_PATH];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            _snwprintf(child, MAX_PATH - 1, L"%s\\%s", AUTOMATION_ROOT_PATH, fd.cFileName);
            child[MAX_PATH - 1] = 0;
            DeleteFileW(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    /* Each platform\<id>\ platform-mod dir: delete its *.old (sweep_old keeps the dir
       alive because it still holds real files). */
    _snwprintf(pattern, MAX_PATH - 1, L"%s\\*", PLATFORM_ROOT_PATH);
    pattern[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t child[MAX_PATH];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        _snwprintf(child, MAX_PATH - 1, L"%s\\%s", PLATFORM_ROOT_PATH, fd.cFileName);
        child[MAX_PATH - 1] = 0;
        sweep_old(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

#define UNINSTALL_MARKER_PATH  L"\\flash2\\automation\\uninstall.pending"
#define ZUXHOOK_PATH           L"\\flash2\\automation\\zuxhook.dll"
#define ZUXHOOK_OLD_PATH       L"\\flash2\\automation\\zuxhook.dll.old"

int ModScanUninstallArmed(void) {
    return GetFileAttributesW(UNINSTALL_MARKER_PATH) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
}

int ModScanUninstallArm(void) {
    HANDLE h = CreateFileW(UNINSTALL_MARKER_PATH, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

/* Delete `path`, or rename it aside to .old if it is locked. At uninstall-boot time no
   Lyra process has started, so the fallback is only defensive. */
static void delete_or_aside(const wchar_t* path) {
    wchar_t oldp[MAX_PATH];
    int k = 0;
    if (DeleteFileW(path)) return;
    for (; path[k] && k < MAX_PATH - 5; k++) oldp[k] = path[k];
    oldp[k] = L'.'; oldp[k+1] = L'o'; oldp[k+2] = L'l'; oldp[k+3] = L'd'; oldp[k+4] = 0;
    DeleteFileW(oldp);
    MoveFileW(path, oldp);
}

void ModScanUninstall(void) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    /* Everything under the automation root except zuxhook.dll and the marker, both
       handled last. A stale zuxhook.dll.old from a prior update is deletable now and
       falls in the general case. */
    _snwprintf(pattern, MAX_PATH - 1, L"%s\\*", AUTOMATION_ROOT_PATH);
    pattern[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t child[MAX_PATH];
            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == 0 ||
                 (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            if (wide_eq_ascii(fd.cFileName, "zuxhook.dll")) continue;
            if (wide_eq_ascii(fd.cFileName, "uninstall.pending")) continue;
            _snwprintf(child, MAX_PATH - 1, L"%s\\%s", AUTOMATION_ROOT_PATH, fd.cFileName);
            child[MAX_PATH - 1] = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmtree(child);
            else delete_or_aside(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    /* The two stray flash-root logs. The XNA installer app (\gametitle\584E07D1\) is kept by
       design: uninstall removes only the Lyra platform, not the app. */
    DeleteFileW(L"\\flash2\\zpod-wk.log");
    DeleteFileW(L"\\flash2\\zd-tee.pcm");

    /* zuxhook.dll is mapped in this and sibling hosts for the boot's lifetime, so it can
       only be renamed. Renamed aside, the firmware finds no zuxhook.dll to auto-load next
       boot; zuxhook.dll.old is the one inert remnant. */
    DeleteFileW(ZUXHOOK_OLD_PATH);
    MoveFileW(ZUXHOOK_PATH, ZUXHOOK_OLD_PATH);

    /* Marker last: sibling hosts keep short-circuiting on it until the wipe is done. */
    DeleteFileW(UNINSTALL_MARKER_PATH);
}
