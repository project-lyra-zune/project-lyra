#include "boot_state.h"

#include <stdio.h>
#include <string.h>

#include "mods_arena.h"
#include "mods_json.h"

#ifdef _WIN32
#  define BOOT_SNPRINTF  _snprintf
#else
#  define BOOT_SNPRINTF  snprintf
#endif

/* One 256-token array (4 KB) covers the marker with no doubling. */
#define BOOT_PARSE_ARENA_BYTES  8192

static const struct { BootLevel level; const char* name; } LEVEL_NAMES[] = {
    { BOOT_LEVEL_NORMAL, "normal" },
    { BOOT_LEVEL_SAFE,   "safe"   },
    { BOOT_LEVEL_BARE,   "bare"   }
};
#define NLEVELS ((int)(sizeof(LEVEL_NAMES) / sizeof(LEVEL_NAMES[0])))

/* NORMAL spends two so one power-off inside the commit window does not cost
   the user their mods. */
static int retry_budget(BootLevel level) {
    switch (level) {
        case BOOT_LEVEL_NORMAL: return 2;
        case BOOT_LEVEL_SAFE:   return 1;
        default:                return 0;
    }
}

BootLevel BootLevelDemote(BootLevel current, int failures) {
    if (current >= BOOT_LEVEL_BARE)        return BOOT_LEVEL_BARE;
    if (failures < retry_budget(current))  return current;
    return (BootLevel)(current + 1);
}

const char* BootLevelName(BootLevel level) {
    int i;
    for (i = 0; i < NLEVELS; i++)
        if (LEVEL_NAMES[i].level == level) return LEVEL_NAMES[i].name;
    return "unknown";
}

static int level_from_token(const ModsJson* j, int tok, BootLevel* out) {
    int i;
    for (i = 0; i < NLEVELS; i++) {
        if (ModsJsonStrEq(j, tok, LEVEL_NAMES[i].name)) {
            *out = LEVEL_NAMES[i].level;
            return 0;
        }
    }
    return -1;
}

void BootStateParse(const char* buf, size_t len, BootState* out) {
    ModsArena arena;
    ModsJson  j;
    int v, n;

    memset(out, 0, sizeof(*out));
    out->level = BOOT_LEVEL_SAFE;

    if (buf == NULL || len == 0) return;
    if (ModsArenaInit(&arena, BOOT_PARSE_ARENA_BYTES) < 0) return;
    if (ModsJsonParse(&arena, buf, len, &j) < 0) goto done;
    if (ModsJsonTypeOf(&j, 0) != MODS_JSON_OBJECT) goto done;

    v = ModsJsonObjectFind(&j, 0, "version");
    if (v < 0 || ModsJsonInt(&j, v, &n) < 0 || n != 1) goto done;

    v = ModsJsonObjectFind(&j, 0, "level");
    if (v < 0 || level_from_token(&j, v, &out->level) < 0) {
        out->level = BOOT_LEVEL_SAFE;
        goto done;
    }

    v = ModsJsonObjectFind(&j, 0, "failures");
    if (v < 0 || ModsJsonInt(&j, v, &n) < 0 || n < 0) {
        out->level = BOOT_LEVEL_SAFE;
        goto done;
    }
    out->failures = n;

    v = ModsJsonObjectFind(&j, 0, "shell_started");
    if (v >= 0 && ModsJsonInt(&j, v, &n) == 0 && n > 0) out->shell_started = 1;

    v = ModsJsonObjectFind(&j, 0, "last_phase");
    if (v >= 0 && ModsJsonInt(&j, v, &n) == 0 && n >= 0) out->last_phase = n;

    v = ModsJsonObjectFind(&j, 0, "last_mod");
    if (v >= 0 && ModsJsonTypeOf(&j, v) == MODS_JSON_STRING) {
        char* s = ModsJsonStrdup(&arena, &j, v);
        if (s != NULL) {
            strncpy(out->last_mod, s, BOOT_MOD_ID_LEN - 1);
            out->last_mod[BOOT_MOD_ID_LEN - 1] = 0;
        }
    }

done:
    ModsArenaFree(&arena);
}

int BootStateFormat(const BootState* st, char* out, size_t cap) {
    int n = BOOT_SNPRINTF(out, cap,
                          "{\"version\":1,\"level\":\"%s\",\"failures\":%d,"
                          "\"shell_started\":%d,"
                          "\"last_mod\":\"%s\",\"last_phase\":%d}",
                          BootLevelName(st->level), st->failures,
                          st->shell_started, st->last_mod, st->last_phase);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}
