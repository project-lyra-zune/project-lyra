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
}

int repo_parse_feed(const char *body, RepoRow *rows, int max, int *truncated) {
    ModsArena arena;
    ModsJson mj;
    int mods_tok, count, i, n = 0;

    if (truncated) *truncated = 0;
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
        if (rows[n].id[0]) n++;   /* a row with no id is skipped, not counted */
    }
    ModsArenaFree(&arena);
    return n;
}
