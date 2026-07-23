#include "repo_feed.h"
#include <string.h>
#include "mods_json.h"

/* The feed is parsed with the shared ModsJson tokenizer (the same parser zuxhook
   uses for manifests), not a bespoke string scan. A 64-mod feed is ~1500 tokens;
   this arena covers the tokens plus parse slack. */
#define FEED_ARENA_BYTES (256 * 1024)

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Decode a JSON string token into a fixed ASCII field, resolving escapes; a
   \uXXXX >= 0x80 becomes '?' (the feed is ensure_ascii, so id/version/sha256/url are
   plain ASCII). A string token's [start,end) is the content between the quotes. */
static void tok_str_a(const ModsJson* j, int tok, char* out, int cap) {
    const char* v; const char* e; int o = 0;
    out[0] = 0;
    if (tok < 0 || j->toks[tok].type != MODS_JSON_STRING) return;
    v = j->src + j->toks[tok].start;
    e = j->src + j->toks[tok].end;
    while (v < e && o < cap - 1) {
        if (*v == '\\' && v + 1 < e) {
            v++;
            if (*v == 'n') out[o++] = '\n';
            else if (*v == 't') out[o++] = '\t';
            else if (*v == 'u') {
                int c = 0, k;
                for (k = 0; k < 4 && v + 1 < e; k++) { v++; c = c * 16 + hexval(*v); }
                out[o++] = (c < 0x80) ? (char)c : '?';
            } else out[o++] = *v;   /* \" \\ \/ */
            v++;
        } else {
            out[o++] = *v++;
        }
    }
    out[o] = 0;
}

/* As tok_str_a, but into a wide field; \uXXXX decodes to the wchar directly. */
static void tok_str_w(const ModsJson* j, int tok, wchar_t* out, int cap) {
    const char* v; const char* e; int o = 0;
    out[0] = 0;
    if (tok < 0 || j->toks[tok].type != MODS_JSON_STRING) return;
    v = j->src + j->toks[tok].start;
    e = j->src + j->toks[tok].end;
    while (v < e && o < cap - 1) {
        if (*v == '\\' && v + 1 < e) {
            v++;
            if (*v == 'n') out[o++] = L'\n';
            else if (*v == 't') out[o++] = L'\t';
            else if (*v == 'u') {
                int c = 0, k;
                for (k = 0; k < 4 && v + 1 < e; k++) { v++; c = c * 16 + hexval(*v); }
                out[o++] = (wchar_t)c;
            } else out[o++] = (wchar_t)(unsigned char)*v;
            v++;
        } else {
            out[o++] = (wchar_t)(unsigned char)*v++;
        }
    }
    out[o] = 0;
}

/* Read the string array `key` of `obj` into a bounded [max][REPO_CAP_LEN] table; returns
   the count. Empty entries are skipped. Absent/non-array key yields 0. */
static int parse_cap_array(const ModsJson* j, int obj, const char* key,
                           char out[][REPO_CAP_LEN], int max) {
    int arr = ModsJsonObjectFind(j, obj, key);
    int n = 0, cnt, k;
    if (arr < 0 || ModsJsonTypeOf(j, arr) != MODS_JSON_ARRAY) return 0;
    cnt = j->toks[arr].size;
    for (k = 0; k < cnt && n < max; k++) {
        int el = ModsJsonArrayAt(j, arr, k);
        if (el < 0 || j->toks[el].type != MODS_JSON_STRING) continue;
        tok_str_a(j, el, out[n], REPO_CAP_LEN);
        if (out[n][0]) n++;
    }
    return n;
}

static void fill_row(const ModsJson* j, int obj, RepoRow* r) {
    int b = 0, sz = 0;
    memset(r, 0, sizeof(*r));
    tok_str_a(j, ModsJsonObjectFind(j, obj, "mod_id"),      r->id,          REPO_ID_LEN);
    tok_str_w(j, ModsJsonObjectFind(j, obj, "name"),        r->name,        REPO_NAME_LEN);
    tok_str_a(j, ModsJsonObjectFind(j, obj, "version"),     r->version,     REPO_VERSION_LEN);
    tok_str_w(j, ModsJsonObjectFind(j, obj, "author"),      r->author,      REPO_AUTHOR_LEN);
    tok_str_w(j, ModsJsonObjectFind(j, obj, "category"),    r->category,    REPO_CATEGORY_LEN);
    tok_str_w(j, ModsJsonObjectFind(j, obj, "description"), r->description, REPO_DESC_LEN);
    tok_str_w(j, ModsJsonObjectFind(j, obj, "changelog"),   r->changelog,   REPO_CHANGELOG_LEN);
    tok_str_a(j, ModsJsonObjectFind(j, obj, "sha256"),      r->sha256,      REPO_SHA_LEN);
    tok_str_a(j, ModsJsonObjectFind(j, obj, "url"),         r->url,         REPO_URL_LEN);
    r->experimental = (ModsJsonBool(j, ModsJsonObjectFind(j, obj, "experimental"), &b) == 0 && b) ? 1 : 0;
    r->size = (ModsJsonInt(j, ModsJsonObjectFind(j, obj, "size"), &sz) == 0 && sz > 0) ? (unsigned long)sz : 0;
    /* Capability footprint (uses) + provided capabilities (absent keys leave the memset
       defaults). */
    r->uses_count = parse_cap_array(j, obj, "uses", r->uses_caps, REPO_MAX_USES);
    r->provides_count = parse_cap_array(j, obj, "provides", r->provides_caps, REPO_MAX_PROVIDES);
}

int repo_parse_feed(const char *body, RepoRow *rows, int max, int *truncated,
                    char plat_prov[][REPO_CAP_LEN], int plat_prov_max, int *plat_prov_count) {
    ModsArena arena;
    ModsJson mj;
    int mods_tok, count, i, n = 0;

    if (truncated) *truncated = 0;
    if (plat_prov_count) *plat_prov_count = 0;
    if (!body || !rows || max <= 0) return 0;
    if (ModsArenaInit(&arena, FEED_ARENA_BYTES) < 0) return 0;

    if (ModsJsonParse(&arena, body, strlen(body), &mj) < 0 ||
            mj.ntoks == 0 || mj.toks[0].type != MODS_JSON_OBJECT) {
        ModsArenaFree(&arena);
        return 0;
    }
    mods_tok = ModsJsonObjectFind(&mj, 0, "mods");
    if (mods_tok < 0 || ModsJsonTypeOf(&mj, mods_tok) != MODS_JSON_ARRAY) {
        ModsArenaFree(&arena);
        return 0;
    }
    count = mj.toks[mods_tok].size;
    for (i = 0; i < count; i++) {
        int obj = ModsJsonArrayAt(&mj, mods_tok, i);
        if (obj < 0 || ModsJsonTypeOf(&mj, obj) != MODS_JSON_OBJECT) continue;
        if (n >= max) { if (truncated) *truncated = 1; break; }
        fill_row(&mj, obj, &rows[n]);
        if (!rows[n].id[0]) continue;   /* a row with no id is skipped, not counted */
        /* The platform row carries the advertised capability set; hoist it block-wide. */
        if (plat_prov && plat_prov_count && strcmp(rows[n].id, LYRA_PLATFORM_ID) == 0)
            *plat_prov_count = parse_cap_array(&mj, obj, "provides", plat_prov, plat_prov_max);
        n++;
    }
    ModsArenaFree(&arena);
    return n;
}
