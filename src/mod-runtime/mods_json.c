#include "mods_json.h"

#include <string.h>

/* Parser state. Tokens are appended to a temp array sized 256 initially;
   we grow x2 from the arena as needed. */
typedef struct {
    const char*   src;
    int           pos;
    int           len;
    ModsJsonTok*  toks;
    int           ntoks;
    int           cap;
    ModsArena*    arena;
    int           failed;
    /* Stack of parent-container token indexes, used to bump their .size. */
    int           stack[64];
    int           depth;
} P;

static ModsJsonTok* push_tok(P* p, ModsJsonType type, int start) {
    ModsJsonTok* t;
    if (p->failed) return NULL;
    if (p->ntoks >= p->cap) {
        int new_cap = p->cap * 2;
        ModsJsonTok* nt = (ModsJsonTok*)ModsArenaAlloc(p->arena,
            new_cap * sizeof(ModsJsonTok));
        if (!nt) { p->failed = 1; return NULL; }
        memcpy(nt, p->toks, p->ntoks * sizeof(ModsJsonTok));
        p->toks = nt;
        p->cap  = new_cap;
    }
    t = &p->toks[p->ntoks++];
    t->type  = type;
    t->start = start;
    t->end   = -1;
    t->size  = 0;
    return t;
}

static void bump_parent_size(P* p) {
    if (p->depth > 0) p->toks[p->stack[p->depth - 1]].size++;
}

static int parse_value(P* p);

static void skip_ws(P* p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            p->pos++;
        else
            break;
    }
}

static int parse_string(P* p) {
    int start, tok_idx;
    ModsJsonTok* t;
    if (p->src[p->pos] != '"') { p->failed = 1; return -1; }
    p->pos++;                    /* consume opening quote */
    start = p->pos;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == '\\') {
            if (p->pos + 1 >= p->len) { p->failed = 1; return -1; }
            p->pos += 2;
            continue;
        }
        if (c == '"') {
            tok_idx = p->ntoks;
            t = push_tok(p, MODS_JSON_STRING, start);
            if (!t) return -1;
            t->end = p->pos;
            bump_parent_size(p);
            p->pos++;                /* consume closing quote */
            return tok_idx;
        }
        p->pos++;
    }
    p->failed = 1;
    return -1;
}

static int parse_primitive(P* p) {
    int start = p->pos, tok_idx;
    ModsJsonTok* t;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == ',' || c == ']' || c == '}' || c == ':') break;
        p->pos++;
    }
    if (p->pos == start) { p->failed = 1; return -1; }
    tok_idx = p->ntoks;
    t = push_tok(p, MODS_JSON_PRIMITIVE, start);
    if (!t) return -1;
    t->end = p->pos;
    bump_parent_size(p);
    return tok_idx;
}

/* Resolve token by index against the CURRENT toks buffer. Must be used
   instead of caching a ModsJsonTok* across any inner push_tok call;
   push_tok may double the toks buffer (allocating a fresh buffer and
   copying), which dangles any prior pointer. Writes through such a
   dangling pointer go to the orphaned old buffer and the current
   token's fields stay at their push_tok defaults (end = -1, size = 0).
   The visible symptom: ModsJsonSkip(tok_idx) sees end == -1, the
   subtree-skip loop fails its `start < end` test immediately, and
   returns tok_idx + 1 instead of the index past the subtree,
   landing array walks one element inside the previous subtree. */
#define TOK(P, IDX) (&(P)->toks[(IDX)])

static int parse_object(P* p) {
    int tok_idx;
    if (p->src[p->pos] != '{') { p->failed = 1; return -1; }
    tok_idx = p->ntoks;
    if (!push_tok(p, MODS_JSON_OBJECT, p->pos)) return -1;
    bump_parent_size(p);
    p->pos++;                            /* consume { */
    if (p->depth >= 64) { p->failed = 1; return -1; }
    p->stack[p->depth++] = tok_idx;
    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == '}') {
        p->depth--;
        TOK(p, tok_idx)->end = p->pos + 1;
        p->pos++;
        return tok_idx;
    }
    while (p->pos < p->len) {
        skip_ws(p);
        if (parse_string(p) < 0) return -1;     /* key */
        skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != ':') { p->failed = 1; return -1; }
        p->pos++;
        skip_ws(p);
        if (parse_value(p) < 0) return -1;      /* value */
        skip_ws(p);
        if (p->pos >= p->len) { p->failed = 1; return -1; }
        if (p->src[p->pos] == ',') { p->pos++; continue; }
        if (p->src[p->pos] == '}') {
            p->depth--;
            TOK(p, tok_idx)->end = p->pos + 1;
            p->pos++;
            return tok_idx;
        }
        p->failed = 1;
        return -1;
    }
    p->failed = 1;
    return -1;
}

static int parse_array(P* p) {
    int tok_idx;
    if (p->src[p->pos] != '[') { p->failed = 1; return -1; }
    tok_idx = p->ntoks;
    if (!push_tok(p, MODS_JSON_ARRAY, p->pos)) return -1;
    bump_parent_size(p);
    p->pos++;
    if (p->depth >= 64) { p->failed = 1; return -1; }
    p->stack[p->depth++] = tok_idx;
    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == ']') {
        p->depth--;
        TOK(p, tok_idx)->end = p->pos + 1;
        p->pos++;
        return tok_idx;
    }
    while (p->pos < p->len) {
        skip_ws(p);
        if (parse_value(p) < 0) return -1;
        skip_ws(p);
        if (p->pos >= p->len) { p->failed = 1; return -1; }
        if (p->src[p->pos] == ',') { p->pos++; continue; }
        if (p->src[p->pos] == ']') {
            p->depth--;
            TOK(p, tok_idx)->end = p->pos + 1;
            p->pos++;
            return tok_idx;
        }
        p->failed = 1;
        return -1;
    }
    p->failed = 1;
    return -1;
}

static int parse_value(P* p) {
    char c;
    skip_ws(p);
    if (p->pos >= p->len) { p->failed = 1; return -1; }
    c = p->src[p->pos];
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '"') return parse_string(p);
    return parse_primitive(p);
}

int ModsJsonParse(ModsArena* arena, const char* src, size_t srclen,
                  ModsJson* out) {
    P p;
    p.src   = src;
    p.pos   = 0;
    p.len   = (int)srclen;
    p.cap   = 256;
    p.ntoks = 0;
    p.arena = arena;
    p.failed= 0;
    p.depth = 0;
    p.toks  = (ModsJsonTok*)ModsArenaAlloc(arena, p.cap * sizeof(ModsJsonTok));
    out->src    = src;
    out->srclen = srclen;
    out->toks   = NULL;
    out->ntoks  = 0;
    if (!p.toks) return -1;
    parse_value(&p);
    if (p.failed) return -1;
    out->toks  = p.toks;
    out->ntoks = p.ntoks;
    return 0;
}

ModsJsonType ModsJsonTypeOf(const ModsJson* j, int tok_idx) {
    if (tok_idx < 0 || tok_idx >= j->ntoks) return MODS_JSON_UNDEF;
    return j->toks[tok_idx].type;
}

int ModsJsonSkip(const ModsJson* j, int tok_idx) {
    int end;
    if (tok_idx < 0 || tok_idx >= j->ntoks) return j->ntoks;
    end = j->toks[tok_idx].end;
    /* Skip every token that ends at or before `end`. */
    {
        int i = tok_idx + 1;
        while (i < j->ntoks && j->toks[i].start < end) i++;
        return i;
    }
}

/* Decode a backslash-escape starting at src[*pos]; advance *pos past it
   and append the decoded byte(s) to dst. Returns 0 on success. */
static int decode_escape(const char* src, int* pos, int end, char* dst, int* dlen) {
    char c;
    if (*pos + 1 >= end) return -1;
    c = src[*pos + 1];
    *pos += 2;
    switch (c) {
        case '"':  dst[(*dlen)++] = '"';  return 0;
        case '\\': dst[(*dlen)++] = '\\'; return 0;
        case '/':  dst[(*dlen)++] = '/';  return 0;
        case 'b':  dst[(*dlen)++] = '\b'; return 0;
        case 'f':  dst[(*dlen)++] = '\f'; return 0;
        case 'n':  dst[(*dlen)++] = '\n'; return 0;
        case 'r':  dst[(*dlen)++] = '\r'; return 0;
        case 't':  dst[(*dlen)++] = '\t'; return 0;
        case 'u': {
            /* \uXXXX - BMP only. Map to UTF-8. */
            unsigned cp = 0;
            int i;
            if (*pos + 4 > end) return -1;
            for (i = 0; i < 4; i++) {
                char h = src[*pos + i];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                else return -1;
            }
            *pos += 4;
            if (cp < 0x80) {
                dst[(*dlen)++] = (char)cp;
            } else if (cp < 0x800) {
                dst[(*dlen)++] = (char)(0xC0 | (cp >> 6));
                dst[(*dlen)++] = (char)(0x80 | (cp & 0x3F));
            } else {
                dst[(*dlen)++] = (char)(0xE0 | (cp >> 12));
                dst[(*dlen)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[(*dlen)++] = (char)(0x80 | (cp & 0x3F));
            }
            return 0;
        }
    }
    return -1;
}

int ModsJsonStrEq(const ModsJson* j, int tok_idx, const char* s) {
    const ModsJsonTok* t;
    int sp, slen;
    if (tok_idx < 0 || tok_idx >= j->ntoks) return 0;
    t = &j->toks[tok_idx];
    if (t->type != MODS_JSON_STRING) return 0;
    sp = t->start;
    slen = (int)strlen(s);
    {
        int dpos = 0;
        char buf[256];
        int blen = 0;
        while (sp < t->end) {
            if (j->src[sp] == '\\') {
                /* decode_escape appends up to 3 bytes (3-byte UTF-8). Ensure the
                   room BEFORE it writes; checking after would let a \uXXXX near
                   the buffer end overrun buf. */
                if (blen > (int)sizeof(buf) - 3) return 0;
                if (decode_escape(j->src, &sp, t->end, buf, &blen) < 0) return 0;
            } else {
                if (blen >= (int)sizeof(buf)) return 0;
                buf[blen++] = j->src[sp++];
            }
        }
        if (blen != slen) return 0;
        while (dpos < blen) { if (buf[dpos] != s[dpos]) return 0; dpos++; }
        return 1;
    }
}

char* ModsJsonStrdup(ModsArena* arena, const ModsJson* j, int tok_idx) {
    const ModsJsonTok* t;
    int sp;
    int max_len;
    char* out;
    int dlen = 0;
    if (tok_idx < 0 || tok_idx >= j->ntoks) return NULL;
    t = &j->toks[tok_idx];
    if (t->type != MODS_JSON_STRING) return NULL;
    /* Decoded length is at most (t->end - t->start). */
    max_len = t->end - t->start;
    out = (char*)ModsArenaAlloc(arena, max_len + 1);
    if (!out) return NULL;
    sp = t->start;
    while (sp < t->end) {
        if (j->src[sp] == '\\') {
            if (decode_escape(j->src, &sp, t->end, out, &dlen) < 0) return NULL;
        } else {
            out[dlen++] = j->src[sp++];
        }
    }
    out[dlen] = '\0';
    return out;
}

int ModsJsonInt(const ModsJson* j, int tok_idx, int* out) {
    const ModsJsonTok* t;
    const char* s;
    int sp, end;
    int sign = 1;
    int val = 0;
    if (tok_idx < 0 || tok_idx >= j->ntoks) return -1;
    t = &j->toks[tok_idx];
    if (t->type != MODS_JSON_PRIMITIVE) return -1;
    s   = j->src;
    sp  = t->start;
    end = t->end;
    if (sp < end && s[sp] == '-') { sign = -1; sp++; }
    if (sp + 1 < end && s[sp] == '0' && (s[sp+1] == 'x' || s[sp+1] == 'X')) {
        sp += 2;
        if (sp >= end) return -1;
        while (sp < end) {
            char c = s[sp++];
            val <<= 4;
            if (c >= '0' && c <= '9') val |= c - '0';
            else if (c >= 'a' && c <= 'f') val |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') val |= c - 'A' + 10;
            else return -1;
        }
    } else {
        if (sp >= end) return -1;
        while (sp < end) {
            char c = s[sp++];
            if (c < '0' || c > '9') return -1;
            val = val * 10 + (c - '0');
        }
    }
    *out = sign * val;
    return 0;
}

int ModsJsonBool(const ModsJson* j, int tok_idx, int* out) {
    const ModsJsonTok* t;
    int n;
    if (out) *out = 0;
    if (tok_idx < 0 || tok_idx >= j->ntoks) return -1;
    t = &j->toks[tok_idx];
    if (t->type != MODS_JSON_PRIMITIVE) return -1;
    n = t->end - t->start;
    if (n == 4 && strncmp(j->src + t->start, "true", 4) == 0)  { if (out) *out = 1; return 0; }
    if (n == 5 && strncmp(j->src + t->start, "false", 5) == 0) { if (out) *out = 0; return 0; }
    return -1;
}

int ModsJsonObjectFind(const ModsJson* j, int obj_idx, const char* key) {
    int i;
    const ModsJsonTok* obj;
    if (obj_idx < 0 || obj_idx >= j->ntoks) return -1;
    obj = &j->toks[obj_idx];
    if (obj->type != MODS_JSON_OBJECT) return -1;
    i = obj_idx + 1;
    {
        int remaining = obj->size;
        while (remaining > 0 && i < j->ntoks) {
            /* i points at a key token. */
            if (ModsJsonStrEq(j, i, key)) {
                /* Value is the next token (one past the key). */
                int vi = i + 1;
                if (vi >= j->ntoks) return -1;
                return vi;
            }
            /* Skip key, then skip value subtree. */
            i = ModsJsonSkip(j, i);         /* past key */
            i = ModsJsonSkip(j, i);         /* past value subtree */
            remaining -= 2;
        }
    }
    return -1;
}

int ModsJsonArrayAt(const ModsJson* j, int arr_idx, int idx) {
    const ModsJsonTok* arr;
    int i, k;
    if (arr_idx < 0 || arr_idx >= j->ntoks) return -1;
    arr = &j->toks[arr_idx];
    if (arr->type != MODS_JSON_ARRAY) return -1;
    if (idx < 0 || idx >= arr->size) return -1;
    i = arr_idx + 1;
    for (k = 0; k < idx; k++) {
        i = ModsJsonSkip(j, i);
        if (i >= j->ntoks) return -1;
    }
    return i;
}
