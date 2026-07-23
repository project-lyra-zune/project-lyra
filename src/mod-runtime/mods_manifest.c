#include "mods_manifest.h"
#include "mods_log.h"
#include "mods_xur_builder.h"
#include "enabled_set.h"

#include <stdio.h>
#include <string.h>

/* ── file IO helpers ────────────────────────────────────────────────── */

/* Read the entire file at `path` into a fresh arena buffer.
   Returns 0 on success; *out / *out_len populated. */
static int read_file(ModsArena* arena, const wchar_t* path,
                     unsigned char** out, size_t* out_len) {
    HANDLE h;
    DWORD size, got = 0;
    unsigned char* buf;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return -1; }
    buf = (unsigned char*)ModsArenaAlloc(arena, size + 1);
    if (!buf) { CloseHandle(h); return -1; }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    buf[size] = 0;
    *out = buf;
    *out_len = size;
    return 0;
}

/* Join a directory + relative path. relative may contain forward slashes
   from manifest values (@/path/inside/mod), which we convert to backslash. */
static void join_path(wchar_t* out, int cap,
                       const wchar_t* dir, const char* rel_utf8) {
    int dlen = (int)wcslen(dir);
    int i, j = 0;
    if (dlen >= cap - 2) { out[0] = 0; return; }
    wcscpy(out, dir);
    if (out[dlen - 1] != L'\\') out[dlen++] = L'\\';
    i = (rel_utf8[0] == '@' && rel_utf8[1] == '/') ? 2 : 0;
    for (; rel_utf8[i] && dlen + j < cap - 1; i++) {
        out[dlen + j] = (rel_utf8[i] == '/') ? L'\\' : (wchar_t)(unsigned char)rel_utf8[i];
        j++;
    }
    out[dlen + j] = 0;
}

static int is_blob_ref(const char* s) {
    return s && s[0] == '@' && s[1] == '/';
}

static int is_asset_ref(const char* s) {
    return s && strncmp(s, "@asset/", 7) == 0;
}

static int is_mod_ref(const char* s) {
    return s && strncmp(s, "@mod/", 5) == 0;
}

static void join_manifest_ref(wchar_t* out, int cap, const wchar_t* dir,
                              const char* ref_utf8) {
    const char* rel = ref_utf8;
    int dlen, i, j;
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (is_blob_ref(ref_utf8)) {
        join_path(out, cap, dir, ref_utf8);
        return;
    }
    if (is_asset_ref(ref_utf8)) {
        rel = ref_utf8 + 7;
        dlen = (int)wcslen(dir);
        if (dlen >= cap - 9) return;
        wcscpy(out, dir);
        if (out[dlen - 1] != L'\\') out[dlen++] = L'\\';
        out[dlen++] = L'a'; out[dlen++] = L's'; out[dlen++] = L's';
        out[dlen++] = L'e'; out[dlen++] = L't'; out[dlen++] = L's';
        out[dlen++] = L'\\';
        for (i = 0, j = 0; rel[i] && dlen + j < cap - 1; i++, j++)
            out[dlen + j] = (rel[i] == '/') ? L'\\' : (wchar_t)(unsigned char)rel[i];
        out[dlen + j] = 0;
        return;
    }
    if (is_mod_ref(ref_utf8)) {
        rel = ref_utf8 + 5;
        dlen = (int)wcslen(dir);
        if (dlen >= cap - 2) return;
        wcscpy(out, dir);
        if (out[dlen - 1] != L'\\') out[dlen++] = L'\\';
        for (i = 0, j = 0; rel[i] && dlen + j < cap - 1; i++, j++)
            out[dlen + j] = (rel[i] == '/') ? L'\\' : (wchar_t)(unsigned char)rel[i];
        out[dlen + j] = 0;
        return;
    }
}

static int build_file_url(wchar_t* out, int cap, const wchar_t* path) {
    const wchar_t prefix[] = L"file://";
    int p = 0, i;
    if (!out || cap <= 0 || !path || !path[0]) return -1;
    for (i = 0; prefix[i] && p < cap - 1; i++) out[p++] = prefix[i];
    for (i = 0; path[i] && p < cap - 1; i++) out[p++] = path[i];
    out[p] = 0;
    return path[i] ? -1 : 0;
}

static int sanitize_id(char* out, int cap, const char* s) {
    int i, p = 0;
    if (!out || cap <= 0 || !s || !s[0]) return -1;
    for (i = 0; s[i] && p < cap - 1; i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) out[p++] = c;
        else out[p++] = '_';
    }
    out[p] = 0;
    return s[i] ? -1 : 0;
}

/* Lookup a scope name. Returns scope index or -1. */
static int scope_find(const ModsBackRefScope* s, const char* name) {
    int i;
    for (i = 0; i < s->count; i++)
        if (strcmp(s->refs[i].name, name) == 0) return i;
    return -1;
}

int ModScopeSet(Mod* m, ModsArena* arena, const char* name, DWORD value) {
    int existing = scope_find(&m->scope, name);
    if (existing >= 0) {
        m->scope.refs[existing].value = value;
        return 0;
    }
    if (m->scope.count >= m->scope.cap) {
        int new_cap = m->scope.cap ? m->scope.cap * 2 : 8;
        ModsBackRef* nb = (ModsBackRef*)ModsArenaAlloc(arena,
            new_cap * sizeof(ModsBackRef));
        if (!nb) return -1;
        if (m->scope.count)
            memcpy(nb, m->scope.refs, m->scope.count * sizeof(ModsBackRef));
        m->scope.refs = nb;
        m->scope.cap  = new_cap;
    }
    m->scope.refs[m->scope.count].name  = ModsArenaStrdup(arena, name);
    m->scope.refs[m->scope.count].value = value;
    if (!m->scope.refs[m->scope.count].name) return -1;
    m->scope.count++;
    return 0;
}

static const ModActionArg* synthetic_find(const ModAction* a, const char* key) {
    int i;
    if (!a || !a->args || !key) return NULL;
    for (i = 0; i < a->args_count; i++) {
        if (a->args[i].key && strcmp(a->args[i].key, key) == 0)
            return &a->args[i];
    }
    return NULL;
}

int ModActionGetString(const ModAction* a, const char* key,
                       ModsArena* arena,
                       const char** out_utf8,
                       wchar_t* out_path_w, int path_cap) {
    int vi;
    const ModsJsonTok* t;
    const char* s;
    char* dup;

    if (out_path_w && path_cap) out_path_w[0] = 0;
    if (out_utf8) *out_utf8 = NULL;

    {
        const ModActionArg* sa = synthetic_find(a, key);
        if (sa) {
            if (sa->kind != MOD_ACTION_ARG_STRING || !sa->string_value) return -1;
            if (is_blob_ref(sa->string_value)) {
                if (!out_path_w || path_cap <= 0) return -1;
                join_path(out_path_w, path_cap, a->mod->source_dir,
                          sa->string_value);
                return 1;
            }
            if (out_utf8) *out_utf8 = sa->string_value;
            return 0;
        }
    }

    vi = ModsJsonObjectFind(&a->mod->json, a->action_tok, key);
    if (vi < 0) return -1;
    t = &a->mod->json.toks[vi];
    if (t->type != MODS_JSON_STRING) return -1;
    dup = ModsJsonStrdup(arena, &a->mod->json, vi);
    if (!dup) return -1;
    s = dup;

    /* @/-prefixed blob reference: resolve against mod's source_dir. */
    if (s[0] == '@' && s[1] == '/') {
        if (!out_path_w || path_cap <= 0) return -1;
        join_path(out_path_w, path_cap, a->mod->source_dir, s);
        return 1;
    }

    if (out_utf8) *out_utf8 = dup;
    return 0;
}

int ModActionGetInt(const ModAction* a, const char* key,
                    int default_value, int* out) {
    DWORD u;
    int rc;
    if (out) *out = default_value;
    rc = ModActionGetU32Required(a, key, &u);   /* absent -> -2, distinct from 0 */
    if (rc == 0) {
        if (out) *out = (int)u;
        return 0;
    }
    if (rc == -2) return 0;   /* absent: keep default, report success */
    return -1;                /* real error (unresolved ref, bad format, type) */
}

int ModActionGetBool(const ModAction* a, const char* key,
                     int default_value, int* out) {
    int vi;
    const ModsJsonTok* t;
    const char* s;
    int n;
    if (out) *out = default_value;
    {
        const ModActionArg* sa = synthetic_find(a, key);
        if (sa) {
            if (sa->kind != MOD_ACTION_ARG_BOOL) return -1;
            if (out) *out = sa->bool_value ? 1 : 0;
            return 0;
        }
    }
    vi = ModsJsonObjectFind(&a->mod->json, a->action_tok, key);
    if (vi < 0) return 0;                       /* absent -> default */
    t = &a->mod->json.toks[vi];
    if (t->type != MODS_JSON_PRIMITIVE) return -1;
    s = a->mod->json.src + t->start;
    n = t->end - t->start;
    if (n == 4 && strncmp(s, "true",  4) == 0) { if (out) *out = 1; return 0; }
    if (n == 5 && strncmp(s, "false", 5) == 0) { if (out) *out = 0; return 0; }
    return -1;                                   /* not true/false -> malformed */
}

/* ── DWORD-typed arg reader with arithmetic back-ref support ────────── */

/* Decode "0xHEX" or decimal into a DWORD. Returns 0 on success. */
static int parse_dword_cstr(const char* s, int n, DWORD* out) {
    int i = 0;
    DWORD v = 0;
    if (n <= 0) return -1;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
        if (i >= n) return -1;
        for (; i < n; i++) {
            char c = s[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (DWORD)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (DWORD)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (DWORD)(c - 'A' + 10);
            else return -1;
        }
    } else {
        for (; i < n; i++) {
            char c = s[i];
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (DWORD)(c - '0');
        }
    }
    *out = v;
    return 0;
}

/* Parse a "$name [+/- offset]" back-ref expression. `s`/`n` is the token
   body including the leading '$' (expected at s[0]). Resolves $name
   against the mod's scope, then optionally adds/subtracts the offset. */
static int resolve_back_ref_expr(const Mod* m, const char* s, int n,
                                  DWORD* out) {
    char name_buf[64];
    int i, name_end, sign, off_start, off_end, idx;
    DWORD base, off = 0;

    if (n < 2 || s[0] != '$') return -1;
    /* name: starts at s[1], runs while [A-Za-z0-9_] */
    i = 1;
    if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')
          || s[i] == '_')) return -1;
    i++;
    while (i < n) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z') || c == '_') { i++; continue; }
        break;
    }
    name_end = i;
    /* Skip whitespace. */
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;

    /* Copy name into buf. */
    {
        int nlen = name_end - 1;
        if (nlen <= 0 || nlen >= (int)sizeof(name_buf)) return -1;
        memcpy(name_buf, s + 1, nlen);
        name_buf[nlen] = 0;
    }
    idx = scope_find(&m->scope, name_buf);
    if (idx < 0) return -1;
    base = m->scope.refs[idx].value;

    if (i >= n) { *out = base; return 0; }
    if (s[i] != '+' && s[i] != '-') return -1;
    sign = (s[i] == '-') ? -1 : 1;
    i++;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    off_start = i;
    off_end   = n;
    if (off_end <= off_start) return -1;
    if (parse_dword_cstr(s + off_start, off_end - off_start, &off) < 0)
        return -1;

    *out = (sign > 0) ? (base + off) : (base - off);
    return 0;
}

int ModResolveTokU32(const Mod* m, int tok_idx, DWORD* out) {
    const ModsJsonTok* t;
    if (out) *out = 0;
    if (tok_idx < 0 || tok_idx >= m->json.ntoks) return -1;
    t = &m->json.toks[tok_idx];
    if (t->type == MODS_JSON_PRIMITIVE) {
        int sv;
        if (ModsJsonInt(&m->json, tok_idx, &sv) < 0) return -1;
        if (out) *out = (DWORD)sv;
        return 0;
    }
    if (t->type == MODS_JSON_STRING) {
        const char* s = m->json.src + t->start;
        int n = t->end - t->start;
        if (n <= 0) return -1;
        if (s[0] == '$') {
            return resolve_back_ref_expr(m, s, n, out);
        }
        /* "0xHEX" / decimal literal in a string. */
        return parse_dword_cstr(s, n, out);
    }
    return -1;
}

int ModActionGetU32(const ModAction* a, const char* key, DWORD* out) {
    int vi;
    if (out) *out = 0;
    {
        const ModActionArg* sa = synthetic_find(a, key);
        if (sa) {
            if (sa->kind != MOD_ACTION_ARG_U32) return -1;
            if (out) *out = sa->u32_value;
            return 0;
        }
    }
    vi = ModsJsonObjectFind(&a->mod->json, a->action_tok, key);
    if (vi < 0) return 0;
    return ModResolveTokU32(a->mod, vi, out);
}

int ModActionGetU32Required(const ModAction* a, const char* key,
                             DWORD* out) {
    int vi;
    if (out) *out = 0;
    {
        const ModActionArg* sa = synthetic_find(a, key);
        if (sa) {
            if (sa->kind != MOD_ACTION_ARG_U32) return -1;
            if (out) *out = sa->u32_value;
            return 0;
        }
    }
    vi = ModsJsonObjectFind(&a->mod->json, a->action_tok, key);
    if (vi < 0) return -2;
    return ModResolveTokU32(a->mod, vi, out);
}

int ModActionGetBytes(const ModAction* a, const char* key,
                      const unsigned char** out, int* out_len) {
    const ModActionArg* sa;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    sa = synthetic_find(a, key);
    if (!sa || sa->kind != MOD_ACTION_ARG_BYTES ||
        !sa->bytes_value || sa->bytes_len <= 0) return -1;
    if (out) *out = sa->bytes_value;
    if (out_len) *out_len = sa->bytes_len;
    return 0;
}

/* ── mod loader ─────────────────────────────────────────────────────── */

static int json_array_size(const ModsJson* j, int root, const char* key) {
    int t = ModsJsonObjectFind(j, root, key);
    if (t < 0) return 0;
    if (j->toks[t].type != MODS_JSON_ARRAY) return -1;
    return j->toks[t].size;
}

/* The capability table: the single source the phase dispatchers and the
   platform-provides advertisement both consult. `phase` = which dispatcher owns it
   (1 compositor, 2 servicesd/gemstone); neither enumerates the other phase's caps.
   `[min_compat, cur]` = the compatibility window this platform advertises: it serves
   any required revision r with min_compat <= r <= cur (revisions start at 1). A
   backward-compatible change raises cur; a breaking change also raises min_compat to cut
   off the revisions it no longer serves. Adding a capability means one entry here plus its
   handler in the owning dispatcher; forgetting either is a loud error, not a silent skip. */
static const struct { const char* name; int phase; int cur; int min_compat; } CAPS[] = {
    /* Phase 1 - compositor: gem/xus composition, registry, blob + daemon
       (file I/O / CreateProcess; no kerncore). */
    { "lyra.gem_add_entry",          1, 1, 1 },
    { "lyra.gem_add_entry_bytes",    1, 1, 1 },
    { "lyra.gem_replace_entry",      1, 1, 1 },
    { "lyra.gem_remove_entry",       1, 1, 1 },
    { "lyra.xus_add_string",         1, 1, 1 },
    { "lyra.xus_set_string",         1, 1, 1 },
    { "lyra.registry_write",         1, 1, 1 },
    { "lyra.write_blob_bytes",       1, 1, 1 },
    { "lyra.spawn_daemon",           1, 1, 1 },
    /* Phase 2 - servicesd/gemstone: XUI surfaces + kerncore-backed kernel
       caps (kerncore isn't ready until after compositor boot). */
    { "lyra.register_setting",       2, 1, 1 },
    { "lyra.register_status",        2, 1, 1 },
    { "lyra.add_status_icon",        2, 1, 1 },
    { "lyra.tint_element",           2, 1, 1 },
    { "lyra.register_visuals",       2, 1, 1 },
    { "lyra.register_xui_class",     2, 1, 1 },
    { "lyra.inject_menu_entry",      2, 1, 1 },
    { "lyra.inject_settings_row",    2, 1, 1 },
    { "lyra.suppress_scene",         2, 1, 1 },
    { "lyra.patch_bytes",            2, 1, 1 },
    { "lyra.kcall",                  2, 1, 1 },
    { "lyra.require_kernel_value",   2, 1, 1 },
    { "lyra.read_kernel_va",         2, 1, 1 },
    { "lyra.require_back_ref_range", 2, 1, 1 },
    { "lyra.require_back_ref_equal", 2, 1, 1 },
    { "lyra.install_function_hook",  2, 1, 1 },
    { "lyra.load_module",            2, 1, 1 },   /* in-process twin of spawn_daemon: LoadLibrary a mod DLL into target_proc */
};
#define NCAPS ((int)(sizeof(CAPS) / sizeof(CAPS[0])))

int ModsCapabilityPhase(const char* type) {
    int i;
    if (!type) return MODS_CAP_PHASE_NONE;
    for (i = 0; i < NCAPS; i++)
        if (strcmp(CAPS[i].name, type) == 0) return CAPS[i].phase;
    return MODS_CAP_PHASE_NONE;
}

int ModsCapabilityProvidedRange(const char* type, int* cur, int* min_compat) {
    int i;
    if (cur) *cur = 0;
    if (min_compat) *min_compat = 0;
    if (!type) return 0;
    for (i = 0; i < NCAPS; i++)
        if (strcmp(CAPS[i].name, type) == 0) {
            if (CAPS[i].cur < 1) return 0;   /* guard: cur 0 is not advertised */
            if (cur) *cur = CAPS[i].cur;
            if (min_compat) *min_compat = CAPS[i].min_compat;
            return 1;
        }
    return 0;
}

static int lowered_action_count(const ModsJson* j, int root) {
    int n = 0, v;
    /* requires/provides are dependency metadata resolved by ModsResolve; they
       produce no actions. Validate they're arrays (if present), don't count. */
    if (json_array_size(j, root, "requires") < 0) return -1;
    if (json_array_size(j, root, "provides") < 0) return -1;
    v = json_array_size(j, root, "settings");     if (v > 0) n += v; else if (v < 0) return -1;
    v = json_array_size(j, root, "status");       if (v > 0) n += v; else if (v < 0) return -1;
    v = json_array_size(j, root, "daemons");      if (v > 0) n += v; else if (v < 0) return -1;
    /* Upper bound only: the array is sized from this, but the authoritative
       action count is whatever the lowering actually writes (load_one_mod sets
       actions_count = write_pos). A status_icon lowers to 2 gem entries + 1
       add_status_icon + 2 actions per visible frame, so 2*MAX_FRAMES+3 bounds it
       regardless of how many frames are null. */
    v = json_array_size(j, root, "status_icons");
    if (v > 0) n += v * (2 * MOD_ICON_MAX_FRAMES + 3); else if (v < 0) return -1;
    return n;
}

static ModAction* synth_action(ModsArena* arena, Mod* m, int* pos,
                               const char* type, int argc) {
    ModAction* a;
    a = &m->actions[*pos];
    memset(a, 0, sizeof(*a));
    a->type = ModsArenaStrdup(arena, type);
    a->action_tok = -1;
    a->index = *pos;
    a->mod = m;
    if (argc > 0) {
        a->args = (ModActionArg*)ModsArenaAllocZ(arena,
            argc * sizeof(ModActionArg));
        if (!a->args || !a->type) return NULL;
    }
    (*pos)++;
    return a;
}

static int synth_arg_string(ModsArena* arena, ModAction* a,
                            const char* key, const char* value) {
    ModActionArg* arg;
    if (!a || !a->args || !key || !value) return -1;
    arg = &a->args[a->args_count++];
    arg->key = ModsArenaStrdup(arena, key);
    arg->kind = MOD_ACTION_ARG_STRING;
    arg->string_value = ModsArenaStrdup(arena, value);
    return (!arg->key || !arg->string_value) ? -1 : 0;
}

static int synth_arg_bool(ModsArena* arena, ModAction* a,
                          const char* key, int value) {
    ModActionArg* arg;
    if (!a || !a->args || !key) return -1;
    arg = &a->args[a->args_count++];
    arg->key = ModsArenaStrdup(arena, key);
    arg->kind = MOD_ACTION_ARG_BOOL;
    arg->bool_value = value ? 1 : 0;
    return !arg->key ? -1 : 0;
}

static int synth_arg_bytes(ModsArena* arena, ModAction* a,
                           const char* key,
                           const unsigned char* bytes, int len) {
    ModActionArg* arg;
    unsigned char* copy;
    if (!a || !a->args || !key || !bytes || len <= 0) return -1;
    copy = (unsigned char*)ModsArenaAlloc(arena, len);
    if (!copy) return -1;
    memcpy(copy, bytes, len);
    arg = &a->args[a->args_count++];
    arg->key = ModsArenaStrdup(arena, key);
    arg->kind = MOD_ACTION_ARG_BYTES;
    arg->bytes_value = copy;
    arg->bytes_len = len;
    return !arg->key ? -1 : 0;
}

static char* json_strdup_req(ModsArena* arena, const ModsJson* j, int tok,
                             const char* context, const char* mod_id) {
    if (tok < 0 || j->toks[tok].type != MODS_JSON_STRING) {
        ModsLogf(L"  %S: %S must be a string", mod_id, context);
        return NULL;
    }
    return ModsJsonStrdup(arena, j, tok);
}

static int copy_optional_string_arg(ModsArena* arena, ModAction* a,
                                    const ModsJson* j, int obj,
                                    const char* key) {
    int t = ModsJsonObjectFind(j, obj, key);
    char* s;
    if (t < 0) return 0;
    if (j->toks[t].type != MODS_JSON_STRING) return -1;
    s = ModsJsonStrdup(arena, j, t);
    if (!s) return -1;
    return synth_arg_string(arena, a, key, s);
}

static int copy_optional_bool_arg(ModsArena* arena, ModAction* a,
                                  const ModsJson* j, int obj,
                                  const char* key) {
    int t = ModsJsonObjectFind(j, obj, key);
    int b;
    if (t < 0) return 0;
    if (ModsJsonBool(j, t, &b) != 0) return -1;
    return synth_arg_bool(arena, a, key, b);
}

/* High-level declarations (settings/status_icons/daemons) describe intent; the
   runtime owns process routing. An entry that sets `target_proc` is a category
   error; reject it rather than silently overriding. Returns -1 if present. */
static int reject_target_proc(const ModsJson* j, int obj,
                              const char* ctx, const char* mod_id) {
    if (ModsJsonObjectFind(j, obj, "target_proc") >= 0) {
        ModsLogf(L"  %S: %S must not set target_proc (runtime owns routing)",
                 mod_id, ctx);
        return -1;
    }
    return 0;
}

/* Does string field `field` of any earlier array entry arr[0..i-1] equal `val`?
   Catches duplicate setting ids / status-icon slots within one manifest, which
   would collide on the shared ModStateBlock key. */
static int dup_in_prior(const ModsJson* j, int arr, int i,
                        const char* field, const char* val) {
    int p;
    for (p = 0; p < i; p++) {
        int o = ModsJsonArrayAt(j, arr, p);
        int f = (o >= 0) ? ModsJsonObjectFind(j, o, field) : -1;
        if (f >= 0 && ModsJsonStrEq(j, f, val)) return 1;
    }
    return 0;
}

/* Does the manifest declare an entry in `array` whose id == `id`? Used to
   validate an indicator's `source` (setting/<id> -> settings[], status/<id> ->
   status[]) against a real declaration, so a typo is a load error rather than a
   silently-inert indicator. */
static int decl_has_id(const ModsJson* j, int root, const char* array,
                       const char* id) {
    int arr = ModsJsonObjectFind(j, root, array);
    int i, n;
    if (arr < 0 || j->toks[arr].type != MODS_JSON_ARRAY) return 0;
    n = j->toks[arr].size;
    for (i = 0; i < n; i++) {
        int o = ModsJsonArrayAt(j, arr, i);
        int f = (o >= 0) ? ModsJsonObjectFind(j, o, "id") : -1;
        if (f >= 0 && ModsJsonStrEq(j, f, id)) return 1;
    }
    return 0;
}

static int settings_has_id(const ModsJson* j, int root, const char* id) {
    return decl_has_id(j, root, "settings", id);
}

static int status_has_id(const ModsJson* j, int root, const char* id) {
    return decl_has_id(j, root, "status", id);
}


static int lower_settings(ModsArena* arena, Mod* m, int root, int* pos) {
    int arr = ModsJsonObjectFind(&m->json, root, "settings");
    int i, n;
    if (arr < 0) return 0;
    if (m->json.toks[arr].type != MODS_JSON_ARRAY) return -1;
    n = m->json.toks[arr].size;
    for (i = 0; i < n; i++) {
        int obj = ModsJsonArrayAt(&m->json, arr, i);
        int tt, qt, idt;
        char* type_s;
        char* id_s;
        ModAction* a;
        if (obj < 0 || m->json.toks[obj].type != MODS_JSON_OBJECT) return -1;
        if (reject_target_proc(&m->json, obj, "settings[]", m->mod_id) != 0) return -1;
        tt = ModsJsonObjectFind(&m->json, obj, "type");
        type_s = json_strdup_req(arena, &m->json, tt, "settings[].type", m->mod_id);
        if (!type_s || strcmp(type_s, "bool") != 0) {
            ModsLogf(L"  %S: settings[%d] unsupported type %S", m->mod_id, i, type_s ? type_s : "?");
            return -1;
        }
        idt = ModsJsonObjectFind(&m->json, obj, "id");
        id_s = json_strdup_req(arena, &m->json, idt, "settings[].id", m->mod_id);
        if (!id_s) return -1;
        if (dup_in_prior(&m->json, arr, i, "id", id_s)) {
            ModsLogf(L"  %S: settings[%d] duplicate id %S", m->mod_id, i, id_s);
            return -1;
        }
        a = synth_action(arena, m, pos, "lyra.register_setting", 15);
        if (!a) return -1;
        if (synth_arg_string(arena, a, "target_proc", "servicesd") != 0 ||
            synth_arg_string(arena, a, "value_type", "bool") != 0 ||
            synth_arg_string(arena, a, "id", id_s) != 0 ||
            copy_optional_string_arg(arena, a, &m->json, obj, "label") != 0 ||
            copy_optional_bool_arg(arena, a, &m->json, obj, "default") != 0 ||
            copy_optional_bool_arg(arena, a, &m->json, obj, "persist") != 0 ||
            copy_optional_string_arg(arena, a, &m->json, obj, "quick_icon") != 0 ||
            copy_optional_string_arg(arena, a, &m->json, obj, "disabled_label") != 0 ||
            copy_optional_string_arg(arena, a, &m->json, obj, "enabled_label") != 0) return -1;
        qt = ModsJsonObjectFind(&m->json, obj, "quick_toggle");
        if (qt >= 0) {
            if (m->json.toks[qt].type == MODS_JSON_STRING) {
                char* qts = ModsJsonStrdup(arena, &m->json, qt);
                if (!qts || synth_arg_string(arena, a, "quick_toggle", qts) != 0) return -1;
            } else {
                int qb;
                if (ModsJsonBool(&m->json, qt, &qb) != 0) return -1;
                if (qb && synth_arg_string(arena, a, "quick_toggle", "default") != 0) return -1;
            }
        }
        /* holds[]: subsystems demanded while this setting is on (e.g.
           "wifi_awake"). Lowered to a comma-joined "holds" arg the runtime
           registers as a pull-based demand source. */
        {
            int ht = ModsJsonObjectFind(&m->json, obj, "holds");
            if (ht >= 0 && m->json.toks[ht].type == MODS_JSON_ARRAY) {
                char joined[128];
                int p = 0, hn = m->json.toks[ht].size, x;
                for (x = 0; x < hn; x++) {
                    int el = ModsJsonArrayAt(&m->json, ht, x);
                    char* s = (el >= 0 && m->json.toks[el].type == MODS_JSON_STRING)
                              ? ModsJsonStrdup(arena, &m->json, el) : NULL;
                    int sl;
                    if (!s) continue;
                    sl = (int)strlen(s);
                    if (p + (p ? 1 : 0) + sl >= (int)sizeof(joined)) break;
                    if (p) joined[p++] = ',';
                    memcpy(joined + p, s, sl); p += sl;
                }
                joined[p] = 0;
                if (p > 0 && synth_arg_string(arena, a, "holds", joined) != 0) return -1;
            }
        }
        /* status: link the row sub-label to a status output's state names, so the
           row shows the effect (Connecting/Casting/...) not just on/off. Copies
           the linked status[]'s state names as the row's per-state labels. */
        {
            int stt = ModsJsonObjectFind(&m->json, obj, "status");
            if (stt >= 0 && m->json.toks[stt].type == MODS_JSON_STRING) {
                char* status_id = ModsJsonStrdup(arena, &m->json, stt);
                int sarr = ModsJsonObjectFind(&m->json, root, "status");
                int found = -1, k, n2;
                char source[64], labels[256];
                int p = 0;
                if (!status_id) return -1;
                if (sarr >= 0 && m->json.toks[sarr].type == MODS_JSON_ARRAY) {
                    n2 = m->json.toks[sarr].size;
                    for (k = 0; k < n2; k++) {
                        int so = ModsJsonArrayAt(&m->json, sarr, k);
                        int idf = (so >= 0) ? ModsJsonObjectFind(&m->json, so, "id") : -1;
                        if (idf >= 0 && ModsJsonStrEq(&m->json, idf, status_id)) { found = so; break; }
                    }
                }
                if (found < 0) {
                    ModsLogf(L"  %S: settings[%d] status %S has no matching status[]",
                             m->mod_id, i, status_id);
                    return -1;
                }
                _snprintf(source, sizeof(source) - 1, "status/%s", status_id);
                source[sizeof(source) - 1] = 0;
                if (synth_arg_string(arena, a, "status_source", source) != 0) return -1;
                {
                    int statest = ModsJsonObjectFind(&m->json, found, "states");
                    if (statest >= 0 && m->json.toks[statest].type == MODS_JSON_ARRAY) {
                        int sn = m->json.toks[statest].size, x;
                        for (x = 0; x < sn; x++) {
                            int se = ModsJsonArrayAt(&m->json, statest, x);
                            char* nm = (se >= 0 && m->json.toks[se].type == MODS_JSON_STRING)
                                       ? ModsJsonStrdup(arena, &m->json, se) : NULL;
                            int nl;
                            if (!nm) continue;
                            nl = (int)strlen(nm);
                            if (p + (p ? 1 : 0) + nl >= (int)sizeof(labels)) break;
                            if (p) labels[p++] = ',';
                            memcpy(labels + p, nm, nl); p += nl;
                        }
                    }
                }
                labels[p] = 0;
                if (p > 0 && synth_arg_string(arena, a, "state_labels", labels) != 0) return -1;
            }
        }
        /* context: a long-press picker for this row, sourced from the setting's
           cross-process list channel (a daemon publishes options; the picker
           writes the selection back). "kind" reserves room for future picker
           kinds; "select" opens a single-choice device/option list. Lowered to
           a context_kind arg the runtime registers the generic provider from. */
        {
            int ct = ModsJsonObjectFind(&m->json, obj, "context");
            if (ct >= 0 && m->json.toks[ct].type == MODS_JSON_OBJECT) {
                int kt = ModsJsonObjectFind(&m->json, ct, "kind");
                char* kind = (kt >= 0 && m->json.toks[kt].type == MODS_JSON_STRING)
                             ? ModsJsonStrdup(arena, &m->json, kt) : NULL;
                if (!kind) {
                    ModsLogf(L"  %S: settings[%d] context.kind must be a string", m->mod_id, i);
                    return -1;
                }
                if (synth_arg_string(arena, a, "context_kind", kind) != 0) return -1;
            }
        }
        ModsLogf(L"  %S: lowered settings[%d] -> register_setting", m->mod_id, i);
    }
    return 0;
}

static int lower_daemons(ModsArena* arena, Mod* m, int root, int* pos) {
    int arr = ModsJsonObjectFind(&m->json, root, "daemons");
    int i, n;
    if (arr < 0) return 0;
    if (m->json.toks[arr].type != MODS_JSON_ARRAY) return -1;
    n = m->json.toks[arr].size;
    for (i = 0; i < n; i++) {
        int obj = ModsJsonArrayAt(&m->json, arr, i);
        int bt;
        char* bin;
        ModAction* a;
        if (obj < 0 || m->json.toks[obj].type != MODS_JSON_OBJECT) return -1;
        if (reject_target_proc(&m->json, obj, "daemons[]", m->mod_id) != 0) return -1;
        bt = ModsJsonObjectFind(&m->json, obj, "binary");
        bin = json_strdup_req(arena, &m->json, bt, "daemons[].binary", m->mod_id);
        if (!bin) return -1;
        a = synth_action(arena, m, pos, "lyra.spawn_daemon", 1);
        if (!a || synth_arg_string(arena, a, "binary_ref", bin) != 0) return -1;
        ModsLogf(L"  %S: lowered daemon %S -> spawn_daemon", m->mod_id, bin);
    }
    return 0;
}

/* Parse "RRGGBB"/"AARRGGBB" hex to 0xAARRGGBB (6-digit defaults alpha 0xFF). */
static int mf_parse_argb(const char* s, DWORD* out) {
    DWORD v = 0;
    int   n = 0;
    if (!s || !out) return -1;
    for (; s[n]; n++) {
        char c = s[n];
        DWORD d;
        if      (c >= '0' && c <= '9') d = (DWORD)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (DWORD)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (DWORD)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    if (n == 6) v |= 0xFF000000u;
    else if (n != 8) return -1;
    *out = v;
    return 0;
}

/* Build one visual XUR from an image and emit its write_blob + register_visuals.
   Returns 0 and an arena-owned visual id (stable for the fragment build). */
static int emit_status_visual(ModsArena* arena, Mod* m, int* pos,
                              const char* mod_s, const char* slot_s,
                              const char* suffix, const wchar_t* image_url,
                              const char** out_visual_id) {
    char visual_id[256], visual_rel[256], visual_ref[256];
    const unsigned char* vis;
    int vis_len;
    ModAction* a;
    _snprintf(visual_id, sizeof(visual_id) - 1, "StatusIcon_%s_%s_%s", mod_s, slot_s, suffix);
    visual_id[sizeof(visual_id) - 1] = 0;
    _snprintf(visual_rel, sizeof(visual_rel) - 1, "_gen\\StatusIcon_%s_%s_%s.xur", mod_s, slot_s, suffix);
    visual_rel[sizeof(visual_rel) - 1] = 0;
    _snprintf(visual_ref, sizeof(visual_ref) - 1, "@/%s", visual_rel);
    visual_ref[sizeof(visual_ref) - 1] = 0;
    if (ModsBuildStatusIconVisualXur(arena, visual_id, image_url, &vis, &vis_len) != 0)
        return -1;
    a = synth_action(arena, m, pos, "lyra.write_blob_bytes", 2);
    if (!a || synth_arg_string(arena, a, "path_ref", visual_ref) != 0 ||
        synth_arg_bytes(arena, a, "content_bytes", vis, vis_len) != 0) return -1;
    a = synth_action(arena, m, pos, "lyra.register_visuals", 3);
    if (!a || synth_arg_string(arena, a, "target_proc", "all") != 0 ||
        synth_arg_string(arena, a, "content_blob_ref", visual_ref) != 0 ||
        synth_arg_string(arena, a, "verify_visual_id", visual_id) != 0) return -1;
    *out_visual_id = ModsArenaStrdup(arena, visual_id);
    return *out_visual_id ? 0 : -1;
}

/* Pack the per-state frame indices as "f0,f1,..." for the add_status_icon arg. */
static void pack_frame_map(char* out, int cap, const signed char* frame_of, int n) {
    int p = 0, k;
    for (k = 0; k < n; k++) {
        char t[8];
        int tl = _snprintf(t, sizeof(t), (k ? ",%d" : "%d"), (int)frame_of[k]);
        if (tl < 0 || p + tl >= cap) break;
        memcpy(out + p, t, tl); p += tl;
    }
    out[p < cap ? p : cap - 1] = 0;
}

/* Pack the per-state tints as "AARRGGBB,..." for the add_status_icon arg. */
static void pack_tint_map(char* out, int cap, const DWORD* tint_of, int n) {
    int p = 0, k;
    for (k = 0; k < n; k++) {
        char t[12];
        int tl = _snprintf(t, sizeof(t), (k ? ",%08lX" : "%08lX"), (unsigned long)tint_of[k]);
        if (tl < 0 || p + tl >= cap) break;
        memcpy(out + p, t, tl); p += tl;
    }
    out[p < cap ? p : cap - 1] = 0;
}

static int lower_status_icons(ModsArena* arena, Mod* m, int root, int* pos) {
    int arr = ModsJsonObjectFind(&m->json, root, "status_icons");
    int i, n;
    if (arr < 0) return 0;
    if (m->json.toks[arr].type != MODS_JSON_ARRAY) return -1;
    n = m->json.toks[arr].size;
    for (i = 0; i < n; i++) {
        int obj = ModsJsonArrayAt(&m->json, arr, i);
        int srct, frt, nstates, k, nvisuals = 0, base_frame = -1;
        char *source, *bare, *image_base = NULL, mod_s[80], slot_s[80];
        char entry[256], frames_str[160], tints_str[256];
        const char* visual_ids[MOD_ICON_MAX_FRAMES];
        signed char frame_of[MOD_ICON_MAX_FRAMES];
        DWORD       tint_of[MOD_ICON_MAX_FRAMES];
        const unsigned char *frag;
        int frag_len;
        ModAction *a;
        if (obj < 0 || m->json.toks[obj].type != MODS_JSON_OBJECT) return -1;
        if (reject_target_proc(&m->json, obj, "status_icons[]", m->mod_id) != 0) return -1;

        /* source = "setting/<id>" | "status/<id>"; the bare id (after the role/)
           names the XUR element + per-state visuals. The role (writer) is the
           source's prefix, validated against the declared setting/status. */
        srct = ModsJsonObjectFind(&m->json, obj, "source");
        source = json_strdup_req(arena, &m->json, srct, "status_icons[].source", m->mod_id);
        if (!source) return -1;
        bare = strchr(source, '/');
        if (!bare || (strncmp(source, "setting/", 8) != 0 &&
                      strncmp(source, "status/", 7) != 0)) {
            ModsLogf(L"  %S: status_icons[%d] source %S must be setting/<id> or status/<id>",
                     m->mod_id, i, source);
            return -1;
        }
        bare++;
        if (dup_in_prior(&m->json, arr, i, "source", source)) {
            ModsLogf(L"  %S: status_icons[%d] duplicate source %S", m->mod_id, i, source);
            return -1;
        }
        /* An indicator on a setting/ source must reference a declared setting; a
           status/ source must reference a declared status[] output. Either way
           the bare id must exist; a typo would otherwise be a silently-inert
           icon. */
        if (source[0] == 's' && source[1] == 'e') {
            if (!settings_has_id(&m->json, root, bare)) {
                ModsLogf(L"  %S: status_icons[%d] source %S has no matching setting",
                         m->mod_id, i, source);
                return -1;
            }
        } else if (!status_has_id(&m->json, root, bare)) {
            ModsLogf(L"  %S: status_icons[%d] source %S has no matching status[]",
                     m->mod_id, i, source);
            return -1;
        }

        /* Optional base image, reused (tinted) by states whose frame is a colour. */
        {
            int it = ModsJsonObjectFind(&m->json, obj, "image");
            if (it >= 0 && m->json.toks[it].type == MODS_JSON_STRING)
                image_base = ModsJsonStrdup(arena, &m->json, it);
        }

        frt = ModsJsonObjectFind(&m->json, obj, "frames");
        if (frt < 0 || m->json.toks[frt].type != MODS_JSON_ARRAY) {
            ModsLogf(L"  %S: status_icons[%d] frames[] required", m->mod_id, i);
            return -1;
        }
        nstates = m->json.toks[frt].size;
        if (nstates < 1 || nstates > MOD_ICON_MAX_FRAMES) {
            ModsLogf(L"  %S: status_icons[%d] frames must have 1..%d entries",
                     m->mod_id, i, MOD_ICON_MAX_FRAMES);
            return -1;
        }
        if (sanitize_id(mod_s, sizeof(mod_s), m->mod_id) != 0 ||
            sanitize_id(slot_s, sizeof(slot_s), bare) != 0) return -1;

        /* Each frames[k]: null/"" = hidden; "@..." = its own image (a new frame);
           a hex colour = recolour the shared base image. Build the distinct
           visuals once; map each state to (frame index, tint). */
        for (k = 0; k < nstates; k++) {
            int el = ModsJsonArrayAt(&m->json, frt, k);
            char* s = NULL;
            DWORD argb = 0;
            frame_of[k] = -1;
            tint_of[k]  = 0xFFFFFFFFu;
            if (el >= 0 && m->json.toks[el].type == MODS_JSON_STRING) {
                s = ModsJsonStrdup(arena, &m->json, el);
                if (s && s[0] == 0) s = NULL;
            }
            if (!s) continue;                            /* hidden state */
            if (s[0] == '@') {                            /* own image -> own frame */
                wchar_t ipath[MODS_MAX_PATH], iurl[MODS_MAX_PATH + 16];
                char suffix[16];
                const char* vid;
                join_manifest_ref(ipath, MODS_MAX_PATH, m->source_dir, s);
                if (!ipath[0] || build_file_url(iurl, MODS_MAX_PATH + 16, ipath) != 0) {
                    ModsLogf(L"  %S: status_icons[%d] frame %d image %S bad ref", m->mod_id, i, k, s);
                    return -1;
                }
                _snprintf(suffix, sizeof(suffix) - 1, "s%d", k);
                suffix[sizeof(suffix) - 1] = 0;
                if (emit_status_visual(arena, m, pos, mod_s, slot_s, suffix, iurl, &vid) != 0) {
                    ModsLogf(L"  %S: status_icons[%d] visual gen failed (state %d)", m->mod_id, i, k);
                    return -1;
                }
                visual_ids[nvisuals] = vid;
                frame_of[k] = (signed char)nvisuals;
                nvisuals++;
            } else if (mf_parse_argb(s, &argb) == 0) {    /* colour -> tint the base */
                if (base_frame < 0) {
                    wchar_t ipath[MODS_MAX_PATH], iurl[MODS_MAX_PATH + 16];
                    const char* vid;
                    if (!image_base) {
                        ModsLogf(L"  %S: status_icons[%d] frame %d colour needs a base \"image\"",
                                 m->mod_id, i, k);
                        return -1;
                    }
                    join_manifest_ref(ipath, MODS_MAX_PATH, m->source_dir, image_base);
                    if (!ipath[0] || build_file_url(iurl, MODS_MAX_PATH + 16, ipath) != 0) {
                        ModsLogf(L"  %S: status_icons[%d] base image %S bad ref", m->mod_id, i, image_base);
                        return -1;
                    }
                    if (emit_status_visual(arena, m, pos, mod_s, slot_s, "base", iurl, &vid) != 0) {
                        ModsLogf(L"  %S: status_icons[%d] base visual gen failed", m->mod_id, i);
                        return -1;
                    }
                    visual_ids[nvisuals] = vid;
                    base_frame = nvisuals;
                    nvisuals++;
                }
                frame_of[k] = (signed char)base_frame;
                tint_of[k]  = argb;
            } else {
                ModsLogf(L"  %S: status_icons[%d] frame %d %S must be null, a hex colour, or a @ image ref",
                         m->mod_id, i, k, s);
                return -1;
            }
        }
        if (nvisuals < 1) {
            ModsLogf(L"  %S: status_icons[%d] has no visible frame", m->mod_id, i);
            return -1;
        }

        _snprintf(entry, sizeof(entry) - 1, "ModIcon_%s_%s.xur", mod_s, slot_s);
        entry[sizeof(entry) - 1] = 0;
        if (ModsBuildStatusIconFragmentXur(arena, bare, visual_ids, nvisuals, &frag, &frag_len) != 0) {
            ModsLogf(L"  %S: status_icons[%d] fragment XUR gen failed", m->mod_id, i);
            return -1;
        }
        a = synth_action(arena, m, pos, "lyra.gem_add_entry_bytes", 3);
        if (!a || synth_arg_string(arena, a, "gem", "scenes_standard.gem") != 0 ||
            synth_arg_string(arena, a, "entry_name", entry) != 0 ||
            synth_arg_bytes(arena, a, "content_bytes", frag, frag_len) != 0) return -1;
        a = synth_action(arena, m, pos, "lyra.gem_add_entry_bytes", 3);
        if (!a || synth_arg_string(arena, a, "gem", "HudScenes.gem") != 0 ||
            synth_arg_string(arena, a, "entry_name", entry) != 0 ||
            synth_arg_bytes(arena, a, "content_bytes", frag, frag_len) != 0) return -1;
        pack_frame_map(frames_str, sizeof(frames_str), frame_of, nstates);
        pack_tint_map(tints_str, sizeof(tints_str), tint_of, nstates);
        a = synth_action(arena, m, pos, "lyra.add_status_icon", 5);
        if (!a || synth_arg_string(arena, a, "target_proc", "all") != 0 ||
            synth_arg_string(arena, a, "source", source) != 0 ||
            synth_arg_string(arena, a, "scene", entry) != 0 ||
            synth_arg_string(arena, a, "frames", frames_str) != 0 ||
            synth_arg_string(arena, a, "tints", tints_str) != 0) return -1;
        ModsLogf(L"  %S: lowered status_icon source=%S states=%d visuals=%d",
                 m->mod_id, source, nstates, nvisuals);
    }
    return 0;
}

/* status[] declares a mod's actor-written effect outputs. Each entry lowers to
   a register_status action that seeds status/<mod>/<id> to state 0 (off) at
   boot, so an indicator binds to a live slot before the actor first writes. The
   actor (a subsystem authority or a daemon) stamps the live state + owner. */
static int lower_status(ModsArena* arena, Mod* m, int root, int* pos) {
    int arr = ModsJsonObjectFind(&m->json, root, "status");
    int i, n;
    if (arr < 0) return 0;
    if (m->json.toks[arr].type != MODS_JSON_ARRAY) return -1;
    n = m->json.toks[arr].size;
    for (i = 0; i < n; i++) {
        int obj = ModsJsonArrayAt(&m->json, arr, i);
        int idt;
        char* id_s;
        ModAction* a;
        if (obj < 0 || m->json.toks[obj].type != MODS_JSON_OBJECT) return -1;
        if (reject_target_proc(&m->json, obj, "status[]", m->mod_id) != 0) return -1;
        idt = ModsJsonObjectFind(&m->json, obj, "id");
        id_s = json_strdup_req(arena, &m->json, idt, "status[].id", m->mod_id);
        if (!id_s) return -1;
        if (dup_in_prior(&m->json, arr, i, "id", id_s)) {
            ModsLogf(L"  %S: status[%d] duplicate id %S", m->mod_id, i, id_s);
            return -1;
        }
        a = synth_action(arena, m, pos, "lyra.register_status", 2);
        if (!a || synth_arg_string(arena, a, "target_proc", "all") != 0 ||
            synth_arg_string(arena, a, "id", id_s) != 0) return -1;
        ModsLogf(L"  %S: lowered status[%d] id=%S", m->mod_id, i, id_s);
    }
    return 0;
}

static int lower_manifest_v2(ModsArena* arena, Mod* m, int root, int* pos) {
    if (lower_settings(arena, m, root, pos) != 0) return -1;
    if (lower_status(arena, m, root, pos) != 0) return -1;
    if (lower_status_icons(arena, m, root, pos) != 0) return -1;
    if (lower_daemons(arena, m, root, pos) != 0) return -1;
    return 0;
}

static int load_one_mod(ModsArena* arena, const wchar_t* mod_dir, Mod* m) {
    wchar_t manifest_path[MODS_MAX_PATH];
    unsigned char* src;
    size_t srclen;
    int root, mod_id_tok, actions_tok, i, base_actions, extra_actions, write_pos;
    int capacity;

    memset(m, 0, sizeof(*m));
    /* Capture mod_dir as source_dir */
    if (wcslen(mod_dir) >= MODS_MAX_PATH) return -1;
    wcscpy(m->source_dir, mod_dir);

    /* Build manifest.json path */
    _snwprintf(manifest_path, MODS_MAX_PATH - 1, L"%s\\manifest.json", mod_dir);
    manifest_path[MODS_MAX_PATH - 1] = 0;

    if (read_file(arena, manifest_path, &src, &srclen) < 0) {
        ModsLogf(L"  load failed: cannot read %s", manifest_path);
        return -1;
    }
    m->json_src = (const char*)src;
    if (ModsJsonParse(arena, m->json_src, srclen, &m->json) < 0) {
        ModsLogf(L"  load failed: JSON parse error %s", manifest_path);
        return -1;
    }

    root = 0;
    if (m->json.ntoks == 0 || m->json.toks[0].type != MODS_JSON_OBJECT) {
        ModsLogf(L"  load failed: root not object %s", manifest_path);
        return -1;
    }

    mod_id_tok = ModsJsonObjectFind(&m->json, root, "mod_id");
    if (mod_id_tok < 0 ||
        m->json.toks[mod_id_tok].type != MODS_JSON_STRING) {
        ModsLogf(L"  load failed: missing mod_id %s", manifest_path);
        return -1;
    }
    m->mod_id = ModsJsonStrdup(arena, &m->json, mod_id_tok);

    {
        int t;
        t = ModsJsonObjectFind(&m->json, root, "name");
        m->name = (t >= 0 && m->json.toks[t].type == MODS_JSON_STRING)
                  ? ModsJsonStrdup(arena, &m->json, t) : m->mod_id;
        t = ModsJsonObjectFind(&m->json, root, "version");
        m->version = (t >= 0 && m->json.toks[t].type == MODS_JSON_STRING)
                  ? ModsJsonStrdup(arena, &m->json, t) : "0.0.0";
    }

    actions_tok = ModsJsonObjectFind(&m->json, root, "actions");
    base_actions = 0;
    if (actions_tok >= 0) {
        if (m->json.toks[actions_tok].type != MODS_JSON_ARRAY) {
            ModsLogf(L"  %S: actions must be an array", m->mod_id);
            return -1;
        }
        base_actions = m->json.toks[actions_tok].size;
    }
    extra_actions = lowered_action_count(&m->json, root);
    if (extra_actions < 0) {
        ModsLogf(L"  %S: high-level manifest sections must be arrays", m->mod_id);
        return -1;
    }
    /* capacity is an upper bound for the array; the real action count is
       whatever the lowering writes (set below from write_pos). */
    capacity = base_actions + extra_actions;
    if (capacity == 0) {
        m->actions = NULL;
        m->actions_count = 0;
        return 0;
    }
    m->actions = (ModAction*)ModsArenaAllocZ(arena, capacity * sizeof(ModAction));
    if (!m->actions) return -1;

    write_pos = 0;
    for (i = 0; i < base_actions; i++) {
        int at = ModsJsonArrayAt(&m->json, actions_tok, i);
        int tt, bt;
        if (at < 0 || m->json.toks[at].type != MODS_JSON_OBJECT) {
            ModsLogf(L"  %S action[%d]: not an object", m->mod_id, i);
            return -1;
        }
        tt = ModsJsonObjectFind(&m->json, at, "type");
        if (tt < 0 || m->json.toks[tt].type != MODS_JSON_STRING) {
            ModsLogf(L"  %S action[%d]: missing type", m->mod_id, i);
            return -1;
        }
        m->actions[i].type       = ModsJsonStrdup(arena, &m->json, tt);
        m->actions[i].action_tok = at;
        m->actions[i].index      = write_pos;
        m->actions[i].mod        = m;
        bt = ModsJsonObjectFind(&m->json, at, "manifest_back_reference");
        if (bt >= 0 && m->json.toks[bt].type == MODS_JSON_STRING)
            m->actions[i].back_ref = ModsJsonStrdup(arena, &m->json, bt);
        write_pos++;
    }
    if (lower_manifest_v2(arena, m, root, &write_pos) != 0) return -1;
    /* The lowering is the authority on how many actions exist; the upper bound
       only sized the array. Guard against an under-estimate that would have
       overrun it (a true upper bound never trips this). */
    if (write_pos > capacity) {
        ModsLogf(L"  %S: lowering overran action capacity %d/%d",
                 m->mod_id, write_pos, capacity);
        return -1;
    }
    m->actions_count = write_pos;
    return 0;
}

/* Read the enabled set (via the canonical enabled_set codec) into
   arena-owned mod_id strings, file order preserved. Returns count, 0 when
   absent/empty, -1 when malformed. `mods_root` is ignored - enabled_set
   owns the canonical enabled.json path (all callers pass the same root). */
static int load_enabled_list(ModsArena* arena, const wchar_t* mods_root,
                              const char*** out_list) {
    char ids[ENABLED_ID_MAX][ENABLED_ID_LEN];
    int n, i;
    const char** list;

    (void)mods_root;
    *out_list = NULL;

    n = EnabledSetRead(ids, ENABLED_ID_MAX);
    if (n < 0) {
        ModsLogf(L"  enabled.json: malformed");
        return -1;
    }
    if (n == 0) {
        ModsLogf(L"  enabled.json empty or absent - no mods active");
        return 0;
    }
    list = (const char**)ModsArenaAlloc(arena, n * sizeof(const char*));
    if (!list) return -1;
    for (i = 0; i < n; i++) {
        list[i] = ModsArenaStrdup(arena, ids[i]);
        if (!list[i]) return -1;
    }
    *out_list = list;
    return n;
}

/* Join mods_root + a child directory name into an absolute path.
   Returns 0 on success, -1 if it would overflow `cap`. */
static int join_mod_dir(wchar_t* out, int cap,
                        const wchar_t* root, const wchar_t* child) {
    int dlen = (int)wcslen(root);
    int j = 0;
    if (dlen >= cap - 2) { out[0] = 0; return -1; }
    wcscpy(out, root);
    if (out[dlen - 1] != L'\\') out[dlen++] = L'\\';
    for (; child[j] && dlen + j < cap - 1; j++) out[dlen + j] = child[j];
    out[dlen + j] = 0;
    return 0;
}

/* Skip the "." / ".." pseudo-entries from a FindFirstFileW walk. */
static int is_dot_dir(const wchar_t* name) {
    return name[0] == L'.' &&
           (name[1] == 0 || (name[1] == L'.' && name[2] == 0));
}

/* Count immediate subdirectories of mods_root (each a candidate mod). */
static int count_mod_dirs(const wchar_t* mods_root) {
    wchar_t pattern[MODS_MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    _snwprintf(pattern, MODS_MAX_PATH - 1, L"%s\\*", mods_root);
    pattern[MODS_MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (is_dot_dir(fd.cFileName)) continue;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* The platform's platform mods (mods-tab, wifi-power) live here, apart from the
   feature mods root. Their location is what marks them: always applied, never gated
   by enabled.json. */
#define PLATFORM_ROOT  L"\\flash2\\automation\\platform"

/* Load every mod dir directly under `root` unconditionally, appending to out at
   out->count. Directory-enumeration order is deterministic and identical across the
   gemstone and servicesd apply passes, so per-process registries agree. */
static void load_root_all(ModsArena* arena, const wchar_t* root, ModSet* out) {
    wchar_t pattern[MODS_MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pattern, MODS_MAX_PATH - 1, L"%s\\*", root);
    pattern[MODS_MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t mod_dir[MODS_MAX_PATH];
        Mod* m;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (is_dot_dir(fd.cFileName)) continue;
        if (out->count >= out->cap) break;
        if (join_mod_dir(mod_dir, MODS_MAX_PATH, root, fd.cFileName) < 0) continue;
        m = &out->mods[out->count];
        if (load_one_mod(arena, mod_dir, m) != 0) continue;
        ModsLogf(L"  loaded platform mod %S (%d actions)", m->mod_id, m->actions_count);
        out->count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int ModsManifestLoadAll(ModsArena* arena, const wchar_t* mods_root,
                        ModSet* out) {
    const char** enabled;
    int n_enabled, discovered_plat, i;

    out->mods = NULL;
    out->count = 0;
    out->cap = 0;

    /* enabled.json is the user's feature selection only. Absent or malformed ->
       zero features; platform mods still apply unconditionally. */
    n_enabled = load_enabled_list(arena, mods_root, &enabled);
    if (n_enabled < 0) n_enabled = 0;

    discovered_plat = count_mod_dirs(PLATFORM_ROOT);
    out->cap = discovered_plat + n_enabled;
    if (out->cap == 0) return 0;

    out->mods = (Mod*)ModsArenaAllocZ(arena, out->cap * sizeof(Mod));
    if (!out->mods) { out->cap = 0; return -1; }

    /* Platform mods (platform\), always applied - their location marks them. */
    load_root_all(arena, PLATFORM_ROOT, out);

    /* Feature mods (mods\), in enabled.json order (the user's selection). */
    for (i = 0; i < n_enabled; i++) {
        wchar_t mod_dir[MODS_MAX_PATH], child[MODS_MAX_PATH];
        Mod* m;
        int j;
        if (out->count >= out->cap) break;
        /* enabled mod_id is ASCII -> wchar_t. */
        for (j = 0; enabled[i][j] && j < MODS_MAX_PATH - 1; j++)
            child[j] = (wchar_t)(unsigned char)enabled[i][j];
        child[j] = 0;
        if (join_mod_dir(mod_dir, MODS_MAX_PATH, mods_root, child) < 0) continue;
        m = &out->mods[out->count];
        if (load_one_mod(arena, mod_dir, m) != 0) continue;
        ModsLogf(L"  loaded feature mod %S (%d actions)", m->mod_id, m->actions_count);
        out->count++;
    }

    return 0;
}
