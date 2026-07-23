#include "mods_capability.h"
#include <string.h>

static int parse_uint(const char** pp) {
    int v = 0, have = 0;
    const char* p = *pp;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; have = 1; }
    *pp = p;
    return have ? v : -1;
}

static void copy_name(const char* s, char* name_out, int name_cap) {
    int o = 0;
    if (name_out && name_cap > 0) name_out[0] = 0;
    if (!s || !name_out) return;
    while (s[o] && s[o] != '@' && o < name_cap - 1) { name_out[o] = s[o]; o++; }
    name_out[o] = 0;
}

void ModsCapParse(const char* s, char* name_out, int name_cap, int* rev_out) {
    const char* at;
    if (rev_out) *rev_out = 1;
    copy_name(s, name_out, name_cap);
    if (!s) return;
    at = strchr(s, '@');
    if (at && rev_out) {
        const char* p = at + 1;
        int r = parse_uint(&p);
        *rev_out = (r >= 1) ? r : 1;
    }
}

void ModsCapParseRange(const char* s, char* name_out, int name_cap, int* lo_out, int* hi_out) {
    const char* at;
    if (lo_out) *lo_out = 1;
    if (hi_out) *hi_out = 1;
    copy_name(s, name_out, name_cap);
    if (!s) return;
    at = strchr(s, '@');
    if (!at) return;
    {
        const char* p = at + 1;
        int a = parse_uint(&p);
        if (a < 1) return;                 /* "name@" garbage: keep [1,1] */
        if (*p == ':') {
            int b;
            p++;
            b = parse_uint(&p);
            if (b < a) b = a;
            if (lo_out) *lo_out = a;
            if (hi_out) *hi_out = b;
        } else {
            if (hi_out) *hi_out = a;        /* lenient "name@n" -> [1,n] */
        }
    }
}

int ModsCapNameEq(const char* a, const char* b) {
    char na[MODS_CAP_NAME_MAX], nb[MODS_CAP_NAME_MAX];
    ModsCapParse(a, na, sizeof(na), 0);
    ModsCapParse(b, nb, sizeof(nb), 0);
    return strcmp(na, nb) == 0;
}

int ModsCapRangeSatisfies(int lo, int hi, int r) {
    return r >= lo && r <= hi;
}

int ModsCapSatisfies(const char* provided, const char* required) {
    char np[MODS_CAP_NAME_MAX], nr[MODS_CAP_NAME_MAX];
    int lo = 1, hi = 1, r = 1;
    ModsCapParseRange(provided, np, sizeof(np), &lo, &hi);
    ModsCapParse(required, nr, sizeof(nr), &r);
    return strcmp(np, nr) == 0 && ModsCapRangeSatisfies(lo, hi, r);
}
