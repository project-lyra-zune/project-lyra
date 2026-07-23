#include "mods_compose.h"
#include "mods_log.h"
#include "mods_xus.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

/* Kernel-state caps (patch_bytes target=kernel, kcall, read_kernel_va,
   require_kernel_value, require_back_ref_range, require_back_ref_equal,
   install_function_hook) live in mods_phase2.c. They depend on kerncore,
   which is bootstrapped by nativeapp.exe's hax() + plant_helpers(),
   only ready by Phase 2 time. Compose-time (Phase 1, compositor boot)
   gem composition + spawn_daemon stay here. */

/* ── helpers ─────────────────────────────────────────────────────────── */

static GemComp* find_gem(ComposeState* st, const char* basename) {
    int i;
    for (i = 0; i < st->count; i++)
        if (strcmp(st->gems[i]->basename, basename) == 0) return st->gems[i];
    return NULL;
}

static GemComp* get_or_load_gem(ComposeState* st, ModsArena* arena,
                                  const char* basename) {
    GemComp* g;
    wchar_t path[MODS_MAX_PATH];
    HANDLE h;
    DWORD size, got = 0;
    unsigned char* buf;
    int i;

    g = find_gem(st, basename);
    if (g) return g;

    if ((int)strlen(basename) >= (int)sizeof(g->basename)) return NULL;

    _snwprintf(path, MODS_MAX_PATH - 1, L"\\Windows\\");
    {
        int pos = (int)wcslen(path);
        for (i = 0; basename[i] && pos < MODS_MAX_PATH - 1; i++)
            path[pos++] = (wchar_t)(unsigned char)basename[i];
        path[pos] = 0;
    }

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ModsLogf(L"    gem load: cannot open %s (err=0x%lx)",
                 path, GetLastError());
        return NULL;
    }
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) {
        CloseHandle(h);
        ModsLogf(L"    gem load: bad size %lu for %s", size, path);
        return NULL;
    }
    buf = (unsigned char*)ModsArenaAlloc(arena, size);
    if (!buf) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        CloseHandle(h);
        ModsLogf(L"    gem load: short read %lu/%lu for %s", got, size, path);
        return NULL;
    }
    CloseHandle(h);

    g = (GemComp*)ModsArenaAllocZ(arena, sizeof(GemComp));
    if (!g) return NULL;
    strncpy(g->basename, basename, sizeof(g->basename) - 1);
    if (ModsXuizDecode(arena, buf, size, &g->xuiz) < 0) {
        ModsLogf(L"    gem load: XUIZ decode failed for %s", path);
        return NULL;
    }

    if (st->count >= st->cap) {
        int new_cap = st->cap ? st->cap * 2 : 8;
        GemComp** ng = (GemComp**)ModsArenaAlloc(arena,
            new_cap * sizeof(GemComp*));
        if (!ng) return NULL;
        if (st->count) memcpy(ng, st->gems, st->count * sizeof(GemComp*));
        st->gems = ng;
        st->cap  = new_cap;
    }
    st->gems[st->count++] = g;
    ModsLogf(L"    gem load: %S (%d entries, %lu bytes)",
             basename, g->xuiz.count, (unsigned long)size);
    return g;
}

/* Read a blob file from a mod's source dir into the arena.
   Returns 0 on success. */
static int read_blob(ModsArena* arena, const wchar_t* path,
                     const unsigned char** out, int* out_len) {
    HANDLE h;
    DWORD size, got = 0;
    unsigned char* buf;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return -1; }
    buf = (unsigned char*)ModsArenaAlloc(arena, size);
    if (!buf) { CloseHandle(h); return -1; }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        CloseHandle(h); return -1;
    }
    CloseHandle(h);
    *out = buf;
    *out_len = (int)size;
    return 0;
}

/* Build a per-locale xus entry name. "en"/empty → table; else "<loc>\<table>". */
static void make_xus_entry_name(char* out, int cap,
                                const char* table, const char* locale) {
    if (!locale || !*locale || strcmp(locale, "en") == 0) {
        _snprintf(out, cap - 1, "%s", table);
    } else {
        _snprintf(out, cap - 1, "%s\\%s", locale, table);
    }
    out[cap - 1] = 0;
}

/* ── capabilities ───────────────────────────────────────────────────── */

static int cap_gem_add_entry(ComposeState* st, ModsArena* arena, ModAction* a) {
    const char* gem_name;
    wchar_t     blob_path[MODS_MAX_PATH];
    const char* entry_name;
    const unsigned char* blob_bytes;
    int blob_len;
    int gem_rc, entry_rc;
    GemComp* g;
    const unsigned char* name_le;
    int name_chars;

    gem_rc = ModActionGetString(a, "gem", arena, &gem_name, NULL, 0);
    if (gem_rc != 0) return -1;
    entry_rc = ModActionGetString(a, "entry_name", arena, &entry_name, NULL, 0);
    if (entry_rc != 0) return -1;
    if (ModActionGetString(a, "content_blob_ref", arena, NULL,
                            blob_path, MODS_MAX_PATH) != 1) return -1;
    if (read_blob(arena, blob_path, &blob_bytes, &blob_len) < 0) {
        ModsLogf(L"    gem_add_entry: missing blob");
        return -1;
    }
    g = get_or_load_gem(st, arena, gem_name);
    if (!g) return -1;
    if (ModsXuizFind(&g->xuiz, entry_name) >= 0) {
        ModsLogf(L"    gem_add_entry: %S already exists in %S", entry_name, gem_name);
        return -1;
    }
    if (ModsXuizAsciiToLe(arena, entry_name, &name_le, &name_chars) < 0)
        return -1;
    if (ModsXuizAppend(&g->xuiz, arena, name_le, name_chars,
                        blob_bytes, blob_len) < 0)
        return -1;
    g->modified = 1;
    ModsLogf(L"    gem_add_entry: %S/%S (+%d bytes)",
             gem_name, entry_name, blob_len);
    return 0;
}

static int cap_gem_add_entry_bytes(ComposeState* st, ModsArena* arena, ModAction* a) {
    const char* gem_name;
    const char* entry_name;
    const unsigned char* blob_bytes;
    int blob_len;
    GemComp* g;
    const unsigned char* name_le;
    int name_chars;

    if (ModActionGetString(a, "gem", arena, &gem_name, NULL, 0) != 0) return -1;
    if (ModActionGetString(a, "entry_name", arena, &entry_name, NULL, 0) != 0) return -1;
    if (ModActionGetBytes(a, "content_bytes", &blob_bytes, &blob_len) != 0) {
        ModsLogf(L"    gem_add_entry_bytes: missing generated blob");
        return -1;
    }
    g = get_or_load_gem(st, arena, gem_name);
    if (!g) return -1;
    if (ModsXuizFind(&g->xuiz, entry_name) >= 0) {
        ModsLogf(L"    gem_add_entry_bytes: %S already exists in %S", entry_name, gem_name);
        return -1;
    }
    if (ModsXuizAsciiToLe(arena, entry_name, &name_le, &name_chars) < 0)
        return -1;
    if (ModsXuizAppend(&g->xuiz, arena, name_le, name_chars,
                        blob_bytes, blob_len) < 0)
        return -1;
    g->modified = 1;
    ModsLogf(L"    gem_add_entry_bytes: %S/%S (+%d bytes)",
             gem_name, entry_name, blob_len);
    return 0;
}

static int cap_gem_replace_entry(ComposeState* st, ModsArena* arena, ModAction* a) {
    const char* gem_name;
    const char* entry_name;
    wchar_t     blob_path[MODS_MAX_PATH];
    const unsigned char* blob_bytes;
    int blob_len, idx;
    GemComp* g;

    if (ModActionGetString(a, "gem", arena, &gem_name, NULL, 0) != 0) return -1;
    if (ModActionGetString(a, "entry_name", arena, &entry_name, NULL, 0) != 0) return -1;
    if (ModActionGetString(a, "new_content_ref", arena, NULL,
                            blob_path, MODS_MAX_PATH) != 1) return -1;
    if (read_blob(arena, blob_path, &blob_bytes, &blob_len) < 0) return -1;
    g = get_or_load_gem(st, arena, gem_name);
    if (!g) return -1;
    idx = ModsXuizFind(&g->xuiz, entry_name);
    if (idx < 0) {
        ModsLogf(L"    gem_replace_entry: %S not in %S", entry_name, gem_name);
        return -1;
    }
    if (ModsXuizReplaceData(&g->xuiz, idx, blob_bytes, blob_len) < 0) return -1;
    g->modified = 1;
    ModsLogf(L"    gem_replace_entry: %S/%S (%d bytes)",
             gem_name, entry_name, blob_len);
    return 0;
}

static int cap_gem_remove_entry(ComposeState* st, ModsArena* arena, ModAction* a) {
    const char* gem_name;
    const char* entry_name;
    int idx;
    GemComp* g;
    if (ModActionGetString(a, "gem", arena, &gem_name, NULL, 0) != 0) return -1;
    if (ModActionGetString(a, "entry_name", arena, &entry_name, NULL, 0) != 0) return -1;
    g = get_or_load_gem(st, arena, gem_name);
    if (!g) return -1;
    idx = ModsXuizFind(&g->xuiz, entry_name);
    if (idx < 0) {
        ModsLogf(L"    gem_remove_entry: %S not in %S (already absent)",
                 entry_name, gem_name);
        return 0;
    }
    if (ModsXuizRemove(&g->xuiz, idx) < 0) return -1;
    g->modified = 1;
    ModsLogf(L"    gem_remove_entry: %S/%S", gem_name, entry_name);
    return 0;
}

static int cap_xus_add_string(ComposeState* st, ModsArena* arena, ModAction* a) {
    const char* gem_name;
    const char* table;
    const char* locale = NULL;
    const char* value;
    char entry_name[128];
    GemComp* g;
    int idx, assigned;
    ModsXus x;
    unsigned char* enc_buf;
    size_t enc_len;

    if (ModActionGetString(a, "gem", arena, &gem_name, NULL, 0) != 0) return -1;
    if (ModActionGetString(a, "table", arena, &table, NULL, 0) != 0) return -1;
    (void)ModActionGetString(a, "locale", arena, &locale, NULL, 0);
    if (ModActionGetString(a, "value", arena, &value, NULL, 0) != 0) return -1;

    make_xus_entry_name(entry_name, sizeof(entry_name), table, locale);
    g = get_or_load_gem(st, arena, gem_name);
    if (!g) return -1;
    idx = ModsXuizFind(&g->xuiz, entry_name);
    if (idx < 0) {
        ModsLogf(L"    xus_add_string: %S/%S not found", gem_name, entry_name);
        return -1;
    }
    if (ModsXusDecode(arena, g->xuiz.entries[idx].data,
                       g->xuiz.entries[idx].data_len, &x) < 0) {
        ModsLogf(L"    xus_add_string: decode failed for %S/%S",
                 gem_name, entry_name);
        return -1;
    }
    if (x.version != MODS_XUS_V_DENSE) {
        ModsLogf(L"    xus_add_string: %S/%S is not v0x0102 dense (got 0x%04x)",
                 gem_name, entry_name, x.version);
        return -1;
    }
    if (ModsXusAppendDense(&x, arena, value, &assigned) < 0) {
        ModsLogf(L"    xus_add_string: non-ASCII or OOM");
        return -1;
    }
    if (ModsXusEncode(arena, &x, &enc_buf, &enc_len) < 0) return -1;
    if (ModsXuizReplaceData(&g->xuiz, idx, enc_buf, (int)enc_len) < 0) return -1;
    g->modified = 1;
    ModsLogf(L"    xus_add_string: %S/%S index=%d value=%S",
             gem_name, entry_name, assigned, value);

    if (a->back_ref) {
        if (ModScopeSet(a->mod, arena, a->back_ref, assigned) < 0) return -1;
        ModsLogf(L"      back-ref $%S = %d", a->back_ref, assigned);
    }
    return 0;
}

static int cap_xus_set_string(ComposeState* st, ModsArena* arena, ModAction* a) {
    const char* gem_name;
    const char* table;
    const char* locale = NULL;
    const char* value;
    char entry_name[128];
    GemComp* g;
    int idx, str_index;
    ModsXus x;
    unsigned char* enc_buf;
    size_t enc_len;

    if (ModActionGetString(a, "gem", arena, &gem_name, NULL, 0) != 0) return -1;
    if (ModActionGetString(a, "table", arena, &table, NULL, 0) != 0) return -1;
    (void)ModActionGetString(a, "locale", arena, &locale, NULL, 0);
    if (ModActionGetInt(a, "index", -1, &str_index) < 0 || str_index < 0)
        return -1;
    if (ModActionGetString(a, "value", arena, &value, NULL, 0) != 0) return -1;

    make_xus_entry_name(entry_name, sizeof(entry_name), table, locale);
    g = get_or_load_gem(st, arena, gem_name);
    if (!g) return -1;
    idx = ModsXuizFind(&g->xuiz, entry_name);
    if (idx < 0) return -1;
    if (ModsXusDecode(arena, g->xuiz.entries[idx].data,
                       g->xuiz.entries[idx].data_len, &x) < 0) return -1;
    if (ModsXusSetDense(&x, arena, str_index, value) < 0) {
        ModsLogf(L"    xus_set_string: bad index %d (count=%d)",
                 str_index, x.count);
        return -1;
    }
    if (ModsXusEncode(arena, &x, &enc_buf, &enc_len) < 0) return -1;
    if (ModsXuizReplaceData(&g->xuiz, idx, enc_buf, (int)enc_len) < 0) return -1;
    g->modified = 1;
    ModsLogf(L"    xus_set_string: %S/%S index=%d", gem_name, entry_name, str_index);
    return 0;
}

/* ── spawn_daemon ────────────────────────────────────────────────────── */

/* CreateProcessW the binary named by `binary_ref` (a "@/path" blob ref
   resolved against the mod's source_dir on flash). */
static int cap_spawn_daemon(ComposeState* st, ModsArena* arena, ModAction* a) {
    wchar_t bin_path[MODS_MAX_PATH];
    const char* unused = NULL;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    int rc;
    (void)st;
    bin_path[0] = 0;
    rc = ModActionGetString(a, "binary_ref", arena, &unused, bin_path, MODS_MAX_PATH);
    if (rc != 1 || bin_path[0] == 0) {
        ModsLogf(L"    spawn_daemon: missing or non-blob 'binary_ref'");
        return -1;
    }
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(bin_path, NULL, NULL, NULL, FALSE,
                        0, NULL, NULL, &si, &pi)) {
        ModsLogf(L"    spawn_daemon: CreateProcessW failed err=0x%lx path=%s",
                 GetLastError(), bin_path);
        return -1;
    }
    ModsLogf(L"    spawn_daemon: pid=0x%lx path=%s",
             pi.dwProcessId, bin_path);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

static int ensure_parent_dir(const wchar_t* path) {
    wchar_t parent[MODS_MAX_PATH];
    int n, i;
    if (!path || !path[0]) return -1;
    n = (int)wcslen(path);
    if (n <= 0 || n >= MODS_MAX_PATH) return -1;
    wcscpy(parent, path);
    for (i = n - 1; i >= 0; i--) {
        if (parent[i] == L'\\') {
            parent[i] = 0;
            if (parent[0]) CreateDirectoryW(parent, NULL);
            return 0;
        }
    }
    return 0;
}

static int cap_write_blob_bytes(ComposeState* st, ModsArena* arena, ModAction* a) {
    wchar_t path[MODS_MAX_PATH];
    const unsigned char* bytes;
    int len;
    HANDLE h;
    DWORD written = 0;
    (void)st;

    if (ModActionGetString(a, "path_ref", arena, NULL, path, MODS_MAX_PATH) != 1 ||
        !path[0]) {
        ModsLogf(L"    write_blob_bytes: path_ref required");
        return -1;
    }
    if (ModActionGetBytes(a, "content_bytes", &bytes, &len) != 0) {
        ModsLogf(L"    write_blob_bytes: content_bytes required");
        return -1;
    }
    ensure_parent_dir(path);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ModsLogf(L"    write_blob_bytes: create failed %s err=0x%lx",
                 path, GetLastError());
        return -1;
    }
    if (!WriteFile(h, bytes, (DWORD)len, &written, NULL) || (int)written != len) {
        ModsLogf(L"    write_blob_bytes: short write %lu/%d to %s",
                 written, len, path);
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    ModsLogf(L"    write_blob_bytes: %s (%d bytes)", path, len);
    return 0;
}

/* ── registry_write ──────────────────────────────────────────────────── */

/* Map a hive-name string to its predefined HKEY. NULL = unsupported. */
static HKEY registry_hive_from_name(const char* hive) {
    if (!hive) return NULL;
    if (strcmp(hive, "HKLM") == 0 || strcmp(hive, "HKEY_LOCAL_MACHINE") == 0)
        return HKEY_LOCAL_MACHINE;
    if (strcmp(hive, "HKCU") == 0 || strcmp(hive, "HKEY_CURRENT_USER") == 0)
        return HKEY_CURRENT_USER;
    if (strcmp(hive, "HKCR") == 0 || strcmp(hive, "HKEY_CLASSES_ROOT") == 0)
        return HKEY_CLASSES_ROOT;
    if (strcmp(hive, "HKU") == 0 || strcmp(hive, "HKEY_USERS") == 0)
        return HKEY_USERS;
    return NULL;
}

/* ASCII (manifest UTF-8 restricted to ASCII) -> wide into a caller buffer.
   Registry key/value names on this device are ASCII; a non-ASCII byte is
   rejected rather than silently mangled. 0 on success, -1 on overflow/non-ASCII. */
static int registry_ascii_to_wide(const char* s, wchar_t* out, int cap) {
    int i;
    if (!s || !out || cap <= 0) return -1;
    for (i = 0; s[i]; i++) {
        if (i >= cap - 1) return -1;
        if ((unsigned char)s[i] >= 0x80) return -1;
        out[i] = (wchar_t)(unsigned char)s[i];
    }
    out[i] = 0;
    return 0;
}

/* Write one registry value; the key is created if absent. Manifest action:
     { "type":"registry_write", "hive":"HKLM",
       "key":"Comm\\AR6K_SD1\\Parms", "name":"DisablePowerManagement",
       "value_type":"DWORD", "value":1 }
   value_type in { DWORD, SZ, MULTI_SZ }. value is a u32 (decimal/0xHEX) for
   DWORD, an ASCII string for SZ/MULTI_SZ (single string; MULTI_SZ gets the
   trailing empty-list terminator). hive in { HKLM, HKCU, HKCR, HKU } (long forms accepted). "name" is optional; omit it (or pass "") to write
   the key's (Default) value. */
static int cap_registry_write(ComposeState* st, ModsArena* arena, ModAction* a) {
    const char* hive_s = NULL;
    const char* key_s  = NULL;
    const char* name_s = NULL;
    const char* type_s = NULL;
    const char* val_s  = NULL;
    HKEY    root, hKey = NULL;
    wchar_t key_w[MODS_MAX_PATH];
    wchar_t name_w[128];
    wchar_t val_w[MODS_MAX_PATH];
    DWORD   disp = 0, dw = 0, type_id, cb;
    size_t  n;
    BYTE*   buf;
    LONG    rc, sr = ERROR_SUCCESS;
    (void)st;

    if (ModActionGetString(a, "hive", arena, &hive_s, NULL, 0) != 0) {
        ModsLogf(L"    registry_write: missing 'hive'"); return -1;
    }
    root = registry_hive_from_name(hive_s);
    if (root == NULL) {
        ModsLogf(L"    registry_write: unsupported hive %S", hive_s); return -1;
    }
    if (ModActionGetString(a, "key", arena, &key_s, NULL, 0) != 0 ||
        registry_ascii_to_wide(key_s, key_w, MODS_MAX_PATH) != 0) {
        ModsLogf(L"    registry_write: missing/invalid 'key'"); return -1;
    }
    if (ModActionGetString(a, "name", arena, &name_s, NULL, 0) != 0) {
        name_s = "";
        name_w[0] = 0;
    } else if (registry_ascii_to_wide(name_s, name_w,
                               (int)(sizeof(name_w)/sizeof(name_w[0]))) != 0) {
        ModsLogf(L"    registry_write: invalid 'name'"); return -1;
    }
    if (ModActionGetString(a, "value_type", arena, &type_s, NULL, 0) != 0) {
        ModsLogf(L"    registry_write: missing 'value_type'"); return -1;
    }

    rc = RegCreateKeyExW(root, key_w, 0, NULL, REG_OPTION_NON_VOLATILE,
                         KEY_ALL_ACCESS, NULL, &hKey, &disp);
    if (rc != ERROR_SUCCESS) {
        ModsLogf(L"    registry_write: RegCreateKeyExW(%S) failed rc=0x%lx",
                 key_s, rc);
        return -1;
    }

    if (strcmp(type_s, "DWORD") == 0) {
        if (ModActionGetU32Required(a, "value", &dw) != 0) {
            ModsLogf(L"    registry_write: missing/invalid DWORD 'value'");
            RegCloseKey(hKey); return -1;
        }
        sr = RegSetValueExW(hKey, name_w[0] ? name_w : NULL, 0, REG_DWORD,
                            (const BYTE*)&dw, sizeof(dw));
        if (sr == ERROR_SUCCESS)
            ModsLogf(L"    registry_write: %S\\%S = 0x%lx (DWORD)%S",
                     key_s, name_s, dw,
                     disp == REG_CREATED_NEW_KEY ? L" [key created]" : L"");
    } else if (strcmp(type_s, "SZ") == 0 || strcmp(type_s, "MULTI_SZ") == 0) {
        /* A @/-prefixed value resolves to the referenced file's on-device path
           (e.g. a COM InprocServer32 DLL), so the manifest cites the payload by
           reference instead of hard-coding \flash2\...; a plain string is used
           verbatim. */
        int vr = ModActionGetString(a, "value", arena, &val_s, val_w, MODS_MAX_PATH);
        if (vr < 0 ||
            (vr == 0 && registry_ascii_to_wide(val_s, val_w, MODS_MAX_PATH) != 0)) {
            ModsLogf(L"    registry_write: missing/invalid string 'value'");
            RegCloseKey(hKey); return -1;
        }
        n = wcslen(val_w);
        type_id = (strcmp(type_s, "MULTI_SZ") == 0) ? REG_MULTI_SZ : REG_SZ;
        /* SZ: string + NUL. MULTI_SZ: string + NUL + empty-list terminator NUL. */
        cb = (DWORD)((n + (type_id == REG_MULTI_SZ ? 2 : 1)) * sizeof(wchar_t));
        buf = (BYTE*)ModsArenaAllocZ(arena, cb);
        if (!buf) { RegCloseKey(hKey); return -1; }
        memcpy(buf, val_w, n * sizeof(wchar_t));
        sr = RegSetValueExW(hKey, name_w[0] ? name_w : NULL, 0, type_id, buf, cb);
        if (sr == ERROR_SUCCESS)
            ModsLogf(L"    registry_write: %S\\%S = \"%s\" (%S)%S",
                     key_s, name_s, val_w, type_s,
                     disp == REG_CREATED_NEW_KEY ? L" [key created]" : L"");
    } else {
        ModsLogf(L"    registry_write: unsupported value_type %S", type_s);
        RegCloseKey(hKey); return -1;
    }
    RegCloseKey(hKey);
    if (sr != ERROR_SUCCESS) {
        ModsLogf(L"    registry_write: RegSetValueExW(%S) failed rc=0x%lx",
                 name_s, sr);
        return -1;
    }
    return 0;
}


/* ── dispatch ───────────────────────────────────────────────────────── */

int ModsComposeApplyAction(ComposeState* st, ModsArena* arena, ModAction* a) {
    const char* t = a->type;
    int phase = ModsCapabilityPhase(t);
    if (phase == MODS_CAP_PHASE_NONE) {
        ModsLogf(L"    unknown capability: %S (skipped)", t);
        return MODS_ACTION_SKIPPED;
    }
    /* A Phase 2 cap needs kerncore (kreadu32, kcall, ...), bootstrapped by
       nativeapp's hax()+plant_helpers(), not ready until after compositor boot.
       Defer it to Phase 2 (gemstone/servicesd-side). */
    if (phase != 1) return 1;
    /* Phase 1 capabilities (compositor: file I/O, no kerncore). */
    if (strcmp(t, "lyra.gem_add_entry")       == 0) return cap_gem_add_entry(st, arena, a);
    if (strcmp(t, "lyra.gem_add_entry_bytes") == 0) return cap_gem_add_entry_bytes(st, arena, a);
    if (strcmp(t, "lyra.gem_replace_entry")   == 0) return cap_gem_replace_entry(st, arena, a);
    if (strcmp(t, "lyra.gem_remove_entry")    == 0) return cap_gem_remove_entry(st, arena, a);
    if (strcmp(t, "lyra.xus_add_string")      == 0) return cap_xus_add_string(st, arena, a);
    if (strcmp(t, "lyra.xus_set_string")      == 0) return cap_xus_set_string(st, arena, a);
    if (strcmp(t, "lyra.spawn_daemon")        == 0) return cap_spawn_daemon(st, arena, a);
    if (strcmp(t, "lyra.write_blob_bytes")    == 0) return cap_write_blob_bytes(st, arena, a);
    if (strcmp(t, "lyra.registry_write")      == 0) return cap_registry_write(st, arena, a);
    ModsLogf(L"    capability %S classified Phase 1 but has no handler", t);
    return -1;
}

/* ── flush ──────────────────────────────────────────────────────────── */

static int write_atomic(const wchar_t* dst, const unsigned char* buf, size_t len) {
    wchar_t tmp[MODS_MAX_PATH];
    HANDLE h;
    DWORD written = 0;
    _snwprintf(tmp, MODS_MAX_PATH - 1, L"%s.new", dst);
    tmp[MODS_MAX_PATH - 1] = 0;

    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ModsLogf(L"    flush: cannot create %s (err=0x%lx)", tmp, GetLastError());
        return -1;
    }
    if (!WriteFile(h, buf, (DWORD)len, &written, NULL) || written != len) {
        ModsLogf(L"    flush: short write %lu/%lu to %s",
                 written, (unsigned long)len, tmp);
        CloseHandle(h);
        DeleteFileW(tmp);
        return -1;
    }
    CloseHandle(h);

    /* WinCE 6 lacks MoveFileExW(REPLACE_EXISTING); do explicit two-step
       rename. Brief window between delete and rename is acceptable;
       gemstone hasn't opened \Windows\<gem> yet at this point in boot
       (compositor's ZUxHookInit runs before gemstone is launched). */
    DeleteFileW(dst);
    if (!MoveFileW(tmp, dst)) {
        ModsLogf(L"    flush: rename %s -> %s failed (err=0x%lx)",
                 tmp, dst, GetLastError());
        DeleteFileW(tmp);
        return -1;
    }
    return 0;
}

int ModsComposeFlushAll(ComposeState* st, ModsArena* arena) {
    int i, failures = 0;
    for (i = 0; i < st->count; i++) {
        GemComp* g = st->gems[i];
        unsigned char* enc;
        size_t enc_len;
        unsigned char* roundtrip;
        size_t rt_len;
        wchar_t dst[MODS_MAX_PATH];
        int j;

        if (!g->modified) continue;

        if (ModsXuizEncode(arena, &g->xuiz, &enc, &enc_len) < 0) {
            ModsLogf(L"    flush: encode failed for %S", g->basename);
            failures++;
            continue;
        }

        /* Round-trip-safety check: encode → decode → encode and verify
           byte-identical with the first encode. Catches subtle codec
           inconsistencies before we clobber \Windows\. */
        {
            ModsXuiz rt;
            if (ModsXuizDecode(arena, enc, enc_len, &rt) < 0 ||
                ModsXuizEncode(arena, &rt, &roundtrip, &rt_len) < 0 ||
                rt_len != enc_len ||
                memcmp(roundtrip, enc, enc_len) != 0) {
                ModsLogf(L"    flush: round-trip mismatch on %S; refusing to write",
                         g->basename);
                failures++;
                continue;
            }
        }

        _snwprintf(dst, MODS_MAX_PATH - 1, L"\\Windows\\");
        j = (int)wcslen(dst);
        {
            int k;
            for (k = 0; g->basename[k] && j < MODS_MAX_PATH - 1; k++)
                dst[j++] = (wchar_t)(unsigned char)g->basename[k];
            dst[j] = 0;
        }

        if (write_atomic(dst, enc, enc_len) == 0) {
            ModsLogf(L"    flush: wrote %s (%lu bytes)",
                     dst, (unsigned long)enc_len);
        } else {
            failures++;
        }
    }
    return failures ? -1 : 0;
}
