#include "mod_fault.h"

#include <stdio.h>
#include <string.h>

#include "mods_arena.h"
#include "mods_json.h"

#ifdef _WIN32
#  define FAULT_SNPRINTF  _snprintf
#else
#  define FAULT_SNPRINTF  snprintf
#endif

#define FAULT_PARSE_ARENA_BYTES 8192

static unsigned long parse_hex(const char* s) {
    unsigned long v = 0;
    if (s == NULL) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        int d;
        if      (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = (v << 4) | (unsigned long)d;
    }
    return v;
}

int ModFaultFormat(const ModFault* f, char* out, size_t cap) {
    int n = FAULT_SNPRINTF(out, cap,
                           "{\"version\":1,\"mod_version\":\"%s\",\"phase\":%d,"
                           "\"action\":%d,\"cap\":\"%s\",\"code\":\"0x%08lX\","
                           "\"disabled\":%d,\"reported\":%d}",
                           f->version, f->phase, f->action, f->cap, f->code,
                           f->disabled, f->reported);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

int ModFaultParse(const char* buf, size_t len, const char* installed_version,
                  ModFault* out) {
    ModsArena arena;
    ModsJson  j;
    int       v, n, rc = 0;

    memset(out, 0, sizeof(*out));
    out->action = -1;
    if (buf == NULL || len == 0) return 0;
    if (ModsArenaInit(&arena, FAULT_PARSE_ARENA_BYTES) < 0) return 0;
    if (ModsJsonParse(&arena, buf, len, &j) < 0) goto done;
    if (ModsJsonTypeOf(&j, 0) != MODS_JSON_OBJECT) goto done;

    v = ModsJsonObjectFind(&j, 0, "mod_version");
    if (v < 0 || ModsJsonTypeOf(&j, v) != MODS_JSON_STRING) goto done;
    {
        char* s = ModsJsonStrdup(&arena, &j, v);
        if (s == NULL) goto done;
        strncpy(out->version, s, MOD_FAULT_VER_LEN - 1);
        out->version[MOD_FAULT_VER_LEN - 1] = 0;
    }
    /* A fault belongs to the version that produced it. */
    if (installed_version != NULL && strcmp(out->version, installed_version) != 0)
        goto done;

    v = ModsJsonObjectFind(&j, 0, "phase");
    if (v >= 0 && ModsJsonInt(&j, v, &n) == 0) out->phase = n;
    v = ModsJsonObjectFind(&j, 0, "action");
    if (v >= 0 && ModsJsonInt(&j, v, &n) == 0) out->action = n;

    v = ModsJsonObjectFind(&j, 0, "cap");
    if (v >= 0 && ModsJsonTypeOf(&j, v) == MODS_JSON_STRING) {
        char* s = ModsJsonStrdup(&arena, &j, v);
        if (s != NULL) {
            strncpy(out->cap, s, MOD_FAULT_CAP_LEN - 1);
            out->cap[MOD_FAULT_CAP_LEN - 1] = 0;
        }
    }
    v = ModsJsonObjectFind(&j, 0, "reported");
    if (v >= 0 && ModsJsonInt(&j, v, &n) == 0 && n > 0) out->reported = 1;

    v = ModsJsonObjectFind(&j, 0, "disabled");
    if (v >= 0 && ModsJsonInt(&j, v, &n) == 0 && n > 0) out->disabled = 1;

    v = ModsJsonObjectFind(&j, 0, "code");
    if (v >= 0 && ModsJsonTypeOf(&j, v) == MODS_JSON_STRING) {
        char* s = ModsJsonStrdup(&arena, &j, v);
        if (s != NULL) out->code = parse_hex(s);
    }
    rc = 1;

done:
    ModsArenaFree(&arena);
    return rc;
}
