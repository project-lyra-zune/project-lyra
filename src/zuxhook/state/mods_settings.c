#include "mods_settings.h"
#include "mods_state_block.h"
#include "mods_toggles.h"
#include "mods_curation.h"
#include "mods_json.h"
#include "mods_arena.h"
#include "mods_log.h"

#include <windows.h>
#include <string.h>

#define MOD_SETTINGS_PATH    L"\\flash2\\automation\\mods\\mod-settings.json"
#define MOD_SETTINGS_VERSION 3

/* Read a whole file into the arena. 0 on success, -1 if absent / empty / short. */
static int read_file(ModsArena* arena, const wchar_t* path,
                     unsigned char** out, size_t* outlen) {
    HANDLE h;
    DWORD size, got = 0;
    unsigned char* buf;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(h); return -1; }
    buf = (unsigned char*)ModsArenaAlloc(arena, size);
    if (!buf) { CloseHandle(h); return -1; }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) { CloseHandle(h); return -1; }
    CloseHandle(h);
    *out = buf;
    *outlen = size;
    return 0;
}

/* Parse mod-settings.json: restore `values` into ModStateBlock and load the
   `quick_toggles` array into the curation store. Returns 1 if a quick_toggles
   array was present (so the caller knows whether to seed curation from declared
   defaults instead). The file's `version` must equal MOD_SETTINGS_VERSION; any
   other version is rejected outright (the file is rewritten on the next save). */
static int load_from_file(void) {
    ModsArena arena;
    unsigned char* src;
    size_t srclen;
    ModsJson j;
    int values_tok, qt_tok, child, end, applied = 0, qt_present = 0;
    int ver_tok, ver = 0;

    if (ModsArenaInit(&arena, 16 * 1024) < 0) return 0;
    if (read_file(&arena, MOD_SETTINGS_PATH, &src, &srclen) < 0) {
        ModsArenaFree(&arena);   /* first boot - no saved state */
        return 0;
    }
    if (ModsJsonParse(&arena, (const char*)src, srclen, &j) < 0 ||
        j.ntoks == 0 || j.toks[0].type != MODS_JSON_OBJECT) {
        ModsLogf(L"  mod-settings: parse error");
        ModsArenaFree(&arena);
        return 0;
    }

    ver_tok = ModsJsonObjectFind(&j, 0, "version");
    if (ver_tok < 0 || ModsJsonInt(&j, ver_tok, &ver) != 0 ||
        ver != MOD_SETTINGS_VERSION) {
        ModsLogf(L"  mod-settings: version %d != %d - ignored", ver, MOD_SETTINGS_VERSION);
        ModsArenaFree(&arena);
        return 0;
    }

    values_tok = ModsJsonObjectFind(&j, 0, "values");
    if (values_tok >= 0 && j.toks[values_tok].type == MODS_JSON_OBJECT) {
        end = ModsJsonSkip(&j, values_tok);
        child = values_tok + 1;
        while (child < end) {
            int val = ModsJsonSkip(&j, child);
            if (val < end && j.toks[child].type == MODS_JSON_STRING) {
                char* key = ModsJsonStrdup(&arena, &j, child);
                int active = 0;
                int idx = key ? ModToggleIndexForKey(key) : -1;
                /* Only restore a registered toggle whose value persists.
                   ModToggleGetPersist(-1) returns the default (1), so an
                   unregistered key must be gated out here or it would write an
                   unowned slot. Transient (persist=false) settings are never
                   restored; they keep their boot-seeded default. */
                if (idx >= 0 && ModToggleGetPersist(idx) &&
                    ModsJsonInt(&j, val, &active) == 0) {
                    ModStateSetState(key, active ? 1 : 0, 0);   /* control slot, owner 0 */
                    applied++;
                }
            }
            child = ModsJsonSkip(&j, val);
        }
        ModsLogf(L"  mod-settings: restored %d value(s)", applied);
    } else {
        ModsLogf(L"  mod-settings: no 'values' object - defaults stand");
    }

    qt_tok = ModsJsonObjectFind(&j, 0, "quick_toggles");
    if (qt_tok >= 0 && j.toks[qt_tok].type == MODS_JSON_ARRAY) {
        int sz = j.toks[qt_tok].size, k;
        qt_present = 1;
        ModCurationClear();
        for (k = 0; k < sz; k++) {
            int at = ModsJsonArrayAt(&j, qt_tok, k);
            if (at >= 0 && j.toks[at].type == MODS_JSON_STRING) {
                char* key = ModsJsonStrdup(&arena, &j, at);
                if (key) ModCurationAdd(key);
            }
        }
        ModsLogf(L"  mod-settings: curation loaded %d key(s)", ModCurationCount());
    }

    ModsArenaFree(&arena);
    return qt_present;
}

void ModSettingsLoad(void) {
    int i, n;
    int qt_present = load_from_file();    /* on success, curation = persisted set */
    if (!qt_present) ModCurationClear();  /* no persisted curation: start empty */

    /* Always merge in every quick_toggle:"default" setting, even when a
       persisted mod-settings.json pinned an older curated set: a newly
       installed mod declares a default toggle whose key isn't in that file, so
       loading the file verbatim would hide it. ModCurationAdd is idempotent and
       append-only, so already-curated keys (persisted defaults and user-added
       eligible ones loaded above) keep their position; only genuinely new
       defaults are appended. */
    n = ModTogglesCount();
    for (i = 0; i < n; i++)
        if (ModToggleGetQuickToggle(i) == MOD_QT_DEFAULT)
            ModCurationAdd(ModToggleGetKey(i));

    ModsLogf(L"  mod-settings: curation %d key(s) (persisted=%d)",
             ModCurationCount(), qt_present);
}

/* Append `"key"` (JSON string) to buf at *len, guarding capacity. Returns 1 on
   success, 0 if it wouldn't fit (caller stops). `tail` is the bytes that must
   still follow (so the closing structure always fits). */
static int append_quoted(char* buf, int cap, int* len, int* wrote,
                         const char* key, int extra_each, int tail) {
    int sl = (int)strlen(key);
    if (*len + (*wrote ? 1 : 0) + sl + extra_each + tail >= cap) return 0;
    if (*wrote) buf[(*len)++] = ',';
    buf[(*len)++] = '"';
    memcpy(buf + *len, key, sl); *len += sl;
    buf[(*len)++] = '"';
    *wrote = 1;
    return 1;
}

/* Persist the store: every registered setting's live ModStateBlock value under
   `values`, and the curated HUD list under `quick_toggles`. Call after a menu
   flip (value change) or a curation edit. */
void ModSettingsSave(void) {
    char buf[2048];
    int len = 0, i, n, c, wrote;
    HANDLE h;
    DWORD written;
    const char header[] = "{\"version\":3,\"values\":{";
    const char mid[]    = "},\"quick_toggles\":[";
    const char footer[] = "]}";
    int hlen = (int)sizeof(header) - 1;
    int mlen = (int)sizeof(mid) - 1;
    int flen = (int)sizeof(footer) - 1;

    n = ModTogglesCount();
    c = ModCurationCount();
    if ((int)sizeof(buf) < hlen + mlen + flen + 1) return;
    memcpy(buf + len, header, hlen); len += hlen;

    wrote = 0;
    for (i = 0; i < n; i++) {
        const char* key = ModToggleGetKey(i);
        int active;
        if (!key) continue;
        /* Transient settings carry no persisted value; keep them out of the
           file so a stale state can't be read back at boot. */
        if (!ModToggleGetPersist(i)) continue;
        /* reserve room for the value digit + the mid/footer that must follow */
        if (!append_quoted(buf, (int)sizeof(buf), &len, &wrote, key,
                           1 /* ':' */ + 1 /* digit */, mlen + flen + 1)) break;
        active = ModStateGetState(key);
        buf[len++] = ':';
        buf[len++] = (active == 1) ? '1' : '0';
    }

    memcpy(buf + len, mid, mlen); len += mlen;

    wrote = 0;
    for (i = 0; i < c; i++) {
        const char* key = ModCurationGetKey(i);
        if (!key) continue;
        if (!append_quoted(buf, (int)sizeof(buf), &len, &wrote, key, 0, flen + 1)) break;
    }

    memcpy(buf + len, footer, flen); len += flen;
    buf[len] = 0;

    h = CreateFileW(MOD_SETTINGS_PATH, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { ModsLogf(L"  mod-settings: save open failed"); return; }
    WriteFile(h, buf, (DWORD)len, &written, NULL);
    CloseHandle(h);
    ModsLogf(L"  mod-settings: saved %d value(s) / %d curated", n, c);
}
