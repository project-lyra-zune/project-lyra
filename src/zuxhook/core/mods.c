#include "mods.h"
#include "mods_arena.h"
#include "mods_log.h"
#include "mods_manifest.h"
#include "mods_compose.h"
#include "mods_json.h"
#include "mods_resolve.h"
#include "mod_scanner.h"
#include "mods_phase2.h"     /* ModsPlatformProvides (platform-provides predicate for ModsResolve) */

#include <windows.h>
#include <string.h>
#include <stdio.h>

/* Compose the per-mod backrefs.json path. */
static void backrefs_path(const Mod* m, wchar_t* out, int cap) {
    int n = (int)wcslen(m->source_dir);
    int i, j;
    if (n + 16 > cap) { out[0] = 0; return; }
    wcscpy(out, m->source_dir);
    if (out[n - 1] != L'\\') out[n++] = L'\\';
    /* append "backrefs.json" */
    {
        const char* s = "backrefs.json";
        for (i = 0, j = 0; s[i] && n + j < cap - 1; i++, j++)
            out[n + j] = (wchar_t)(unsigned char)s[i];
        out[n + j] = 0;
    }
}

int ModsWriteBackRefs(Mod* m) {
    wchar_t path[MODS_MAX_PATH];
    HANDLE h;
    char buf[1024];
    int len = 0, i;
    DWORD written = 0;

    if (m->scope.count == 0) return 0;
    backrefs_path(m, path, MODS_MAX_PATH);

    len += _snprintf(buf + len, sizeof(buf) - len, "{");
    for (i = 0; i < m->scope.count; i++) {
        /* Emit hex so VAs > 2^31 round-trip cleanly. */
        len += _snprintf(buf + len, sizeof(buf) - len,
                         "%s\"%s\":\"0x%lx\"",
                         (i > 0) ? "," : "",
                         m->scope.refs[i].name,
                         (unsigned long)m->scope.refs[i].value);
        if (len >= (int)sizeof(buf) - 2) return -1;
    }
    len += _snprintf(buf + len, sizeof(buf) - len, "}");

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ModsLogf(L"    backrefs write: cannot create %s (err=0x%lx)",
                 path, GetLastError());
        return -1;
    }
    if (!WriteFile(h, buf, (DWORD)len, &written, NULL) || (int)written != len) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    ModsLogf(L"    backrefs written: %S (%d bytes, %d refs)",
             m->mod_id, len, m->scope.count);
    return 0;
}

int ModsLoadBackRefs(Mod* m, ModsArena* arena) {
    wchar_t path[MODS_MAX_PATH];
    HANDLE h;
    DWORD size, got = 0;
    char* buf;
    ModsJson j;
    int root, i, remaining;
    int loaded = 0;

    backrefs_path(m, path, MODS_MAX_PATH);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;       /* absent = no Phase 1 back-refs */
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(h); return 0; }
    buf = (char*)ModsArenaAlloc(arena, size + 1);
    if (!buf) { CloseHandle(h); return -1; }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    buf[size] = 0;

    if (ModsJsonParse(arena, buf, size, &j) < 0) return -1;
    if (j.ntoks == 0 || j.toks[0].type != MODS_JSON_OBJECT) return -1;

    /* Walk key-value pairs at the root. Accepts:
         "name": 12345              (legacy decimal/hex int)
         "name": "0x1234abcd"       (hex string, the current format,
                                      needed for VAs > 2^31) */
    root = 0;
    i = root + 1;
    remaining = j.toks[root].size;
    while (remaining > 0 && i < j.ntoks) {
        int key_idx = i;
        int val_idx = i + 1;
        const ModsJsonTok* vt;
        char* name;
        DWORD val = 0;
        if (j.toks[key_idx].type != MODS_JSON_STRING) return -1;
        name = ModsJsonStrdup(arena, &j, key_idx);
        if (!name) return -1;
        vt = &j.toks[val_idx];
        if (vt->type == MODS_JSON_PRIMITIVE) {
            int sv;
            if (ModsJsonInt(&j, val_idx, &sv) < 0) return -1;
            val = (DWORD)sv;
        } else if (vt->type == MODS_JSON_STRING) {
            const char* s = j.src + vt->start;
            int n = vt->end - vt->start;
            int sp = 0;
            DWORD v = 0;
            /* Tolerate leading "0x". */
            if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                sp = 2;
            if (sp >= n) return -1;
            for (; sp < n; sp++) {
                char c = s[sp];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= (DWORD)(c - '0');
                else if (c >= 'a' && c <= 'f') v |= (DWORD)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= (DWORD)(c - 'A' + 10);
                else return -1;
            }
            val = v;
        } else {
            return -1;
        }
        if (ModScopeSet(m, arena, name, val) < 0) return -1;
        loaded++;
        i = ModsJsonSkip(&j, val_idx);
        remaining -= 2;
    }
    if (loaded > 0)
        ModsLogf(L"    backrefs loaded: %S (%d refs)", m->mod_id, loaded);
    return 0;
}

/* Arena size: scenes_standard.gem is ~2-3 MB; round-trip safety check
   doubles the in-flight buffers. 24 MB gives comfortable headroom for
   typical mods. Compositor has plenty of address space. */
#define MODS_ARENA_BYTES (24 * 1024 * 1024)

int ModsApplyPhase1(void) {
    ModsArena    arena;
    ModSet       mods;
    ComposeState st;
    int i, j;
    int total = 0, p1 = 0, p2 = 0, fail = 0;

    /* Ensure the mods directory exists; create it lazily on first boot. */
    CreateDirectoryW(L"\\flash2\\automation\\mods", NULL);

    ModsLogOpen(L"\\flash2\\automation\\mods\\boot.log");
    ModsLogf(L"== ModsApplyPhase1 start ==");

    /* Clear rename-aside leftovers before daemons spawn: a *.old binary from a
     * prior update/remove is no longer held now, so delete it (and any mod dir
     * left empty by a removal). Safe by construction: archived mods keep real
     * files, so their dirs survive. */
    ModScanSweepOld();
    ModScanSweepPlatformOld();

    if (ModsArenaInit(&arena, MODS_ARENA_BYTES) < 0) {
        ModsLogf(L"  arena init (%lu bytes) failed",
                 (unsigned long)MODS_ARENA_BYTES);
        ModsLogClose();
        return -1;
    }

    memset(&mods, 0, sizeof(mods));
    memset(&st,   0, sizeof(st));

    if (ModsManifestLoadAll(&arena, L"\\flash2\\automation\\mods", &mods) < 0) {
        ModsLogf(L"  manifest load fatal");
        goto done;
    }
    if (mods.count == 0) {
        ModsLogf(L"  no enabled mods");
        goto done;
    }

    ModsResolve(&mods, &arena, ModsPlatformProvides);

    ModsLogf(L"  applying %d mod(s) in priority order", mods.count);
    for (i = 0; i < mods.count; i++) {
        Mod* m = &mods.mods[i];
        if (m->disabled) {
            ModsLogf(L"  [%d/%d] %S - skipped (resolver disabled)",
                     i + 1, mods.count, m->mod_id);
            continue;
        }
        ModsLogf(L"  [%d/%d] %S v%S - %d action(s)",
                 i + 1, mods.count, m->mod_id, m->version, m->actions_count);
        for (j = 0; j < m->actions_count; j++) {
            int rc;
            ModAction* a = &m->actions[j];
            total++;
            rc = ModsComposeApplyAction(&st, &arena, a);
            if (rc == 0) {
                p1++;
            } else if (rc == 1) {
                p2++;          /* Phase 2 capability; not run here */
            } else {
                fail++;
                ModsLogf(L"    action[%d] %S FAILED", j, a->type);
            }
        }
        /* Persist this mod's back-ref scope so Phase 2 (in gemstone)
           can resolve back-refs assigned by Phase 1 capabilities. */
        ModsWriteBackRefs(m);
    }
    ModsLogf(L"  %d total action(s): %d applied / %d phase2-deferred / %d failed",
             total, p1, p2, fail);

    if (ModsComposeFlushAll(&st, &arena) < 0)
        ModsLogf(L"  some flushes failed (see lines above)");

done:
    ModsLogf(L"== ModsApplyPhase1 done (arena used: %lu / %lu) ==",
             (unsigned long)arena.used, (unsigned long)arena.size);
    ModsArenaFree(&arena);
    ModsLogClose();
    return 0;
}
