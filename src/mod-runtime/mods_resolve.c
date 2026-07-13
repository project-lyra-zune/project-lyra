#include "mods_resolve.h"

#include "mods_json.h"
#include "mods_log.h"

#include <string.h>

/* Manifest root object is always token 0 (load_one_mod rejects a non-object
   root before the mod reaches a ModSet). */
#define ROOT 0

/* Does mod `m`'s top-level string array `key` contain the string `val`? */
static int array_has(const Mod* m, const char* key, const char* val) {
    int arr = ModsJsonObjectFind(&m->json, ROOT, key);
    int i, n;
    if (arr < 0 || m->json.toks[arr].type != MODS_JSON_ARRAY) return 0;
    n = m->json.toks[arr].size;
    for (i = 0; i < n; i++) {
        int e = ModsJsonArrayAt(&m->json, arr, i);
        if (e >= 0 && ModsJsonStrEq(&m->json, e, val)) return 1;
    }
    return 0;
}

/* Is `cap` provided by any currently-enabled mod's provides[]? */
static int cap_provided_by_mod(const ModSet* s, const char* cap) {
    int i;
    for (i = 0; i < s->count; i++) {
        if (s->mods[i].disabled) continue;
        if (array_has(&s->mods[i], "provides", cap)) return 1;
    }
    return 0;
}

/* Index of the mod whose mod_id equals the string at token `tok`, or -1. */
static int find_mod_by_tok(const ModSet* s, const ModsJson* j, int tok) {
    int i;
    for (i = 0; i < s->count; i++)
        if (ModsJsonStrEq(j, tok, s->mods[i].mod_id)) return i;
    return -1;
}

/* Arena-owned "<prefix><what>" for a short held-back reason (shown in the
   mod manager). Bounded; over-long inputs are truncated. */
static const char* reasonf(ModsArena* arena, const char* prefix, const char* what) {
    char buf[96];
    int p;
    strncpy(buf, prefix, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    p = (int)strlen(buf);
    if (what && p < (int)sizeof(buf) - 1)
        strncat(buf, what, sizeof(buf) - 1 - (size_t)p);
    return ModsArenaStrdup(arena, buf);
}

/* One resolution sweep. Returns 1 if it disabled anything (caller re-runs to a
   fixpoint so a newly-disabled provider cascades to its consumers). */
static int resolve_pass(ModSet* set, ModsArena* arena,
                        ModsPlatformProvidesFn platform_provides) {
    int changed = 0;
    int i;
    for (i = 0; i < set->count; i++) {
        Mod* m = &set->mods[i];
        const ModsJson* j = &m->json;
        int arr, n, x, e, k;
        if (m->disabled) continue;

        /* conflicts[]: if a named mod is present + enabled, disable the one
           later in load order (deterministic; earlier mod wins). */
        arr = ModsJsonObjectFind(j, ROOT, "conflicts");
        if (arr >= 0 && j->toks[arr].type == MODS_JSON_ARRAY) {
            n = j->toks[arr].size;
            for (x = 0; x < n; x++) {
                e = ModsJsonArrayAt(j, arr, x);
                k = find_mod_by_tok(set, j, e);
                if (k >= 0 && k != i && !set->mods[k].disabled) {
                    int loser = (i > k) ? i : k;
                    set->mods[loser].disabled = 1;
                    set->mods[loser].disabled_reason = reasonf(arena,
                        "conflicts with ",
                        (loser == i) ? set->mods[k].mod_id : m->mod_id);
                    changed = 1;
                    ModsLogf(L"  resolve: %S conflicts with %S -> disable %S",
                             m->mod_id, set->mods[k].mod_id,
                             set->mods[loser].mod_id);
                    if (loser == i) break;
                }
            }
        }
        if (m->disabled) continue;

        /* requires[]: each capability must be platform-provided or provided by
           an enabled mod. */
        arr = ModsJsonObjectFind(j, ROOT, "requires");
        if (arr >= 0 && j->toks[arr].type == MODS_JSON_ARRAY) {
            n = j->toks[arr].size;
            for (x = 0; x < n; x++) {
                char* cap;
                e = ModsJsonArrayAt(j, arr, x);
                cap = ModsJsonStrdup(arena, j, e);
                if (!cap ||
                    (!platform_provides(cap) && !cap_provided_by_mod(set, cap))) {
                    m->disabled = 1;
                    m->disabled_reason = reasonf(arena, "requires ", cap);
                    changed = 1;
                    ModsLogf(L"  resolve: %S requires %S -> unsatisfied, disable",
                             m->mod_id, cap ? cap : "(oom)");
                    break;
                }
            }
        }
        if (m->disabled) continue;

        /* depends_on[]: each named mod must be present + enabled. */
        arr = ModsJsonObjectFind(j, ROOT, "depends_on");
        if (arr >= 0 && j->toks[arr].type == MODS_JSON_ARRAY) {
            n = j->toks[arr].size;
            for (x = 0; x < n; x++) {
                char* dep;
                e = ModsJsonArrayAt(j, arr, x);
                k = find_mod_by_tok(set, j, e);
                if (k < 0 || k == i || set->mods[k].disabled) {
                    dep = ModsJsonStrdup(arena, j, e);
                    m->disabled = 1;
                    m->disabled_reason = reasonf(arena, "needs ", dep);
                    changed = 1;
                    ModsLogf(L"  resolve: %S depends_on %S -> missing, disable",
                             m->mod_id, dep ? dep : "(oom)");
                    break;
                }
            }
        }
    }
    return changed;
}

int ModsCapabilityDemanded(const ModSet* set, const char* cap) {
    int i;
    if (!set || !cap) return 0;
    for (i = 0; i < set->count; i++) {
        if (set->mods[i].disabled) continue;
        if (array_has(&set->mods[i], "requires", cap) ||
            array_has(&set->mods[i], "provides", cap)) return 1;
    }
    return 0;
}

/* Are all of mod `i`'s enabled dependencies already in `placed`? A dependency
   is a depends_on[] target or an enabled mod whose provides[] satisfies one of
   i's mod-level requires[]. (Platform-provided requires impose no mod order.) */
static int deps_satisfied(const ModSet* s, int i, const int* placed,
                          ModsArena* arena,
                          ModsPlatformProvidesFn platform_provides) {
    const Mod* m = &s->mods[i];
    const ModsJson* j = &m->json;
    int arr, n, x, e, k, p;
    arr = ModsJsonObjectFind(j, ROOT, "depends_on");
    if (arr >= 0 && j->toks[arr].type == MODS_JSON_ARRAY) {
        n = j->toks[arr].size;
        for (x = 0; x < n; x++) {
            e = ModsJsonArrayAt(j, arr, x);
            k = find_mod_by_tok(s, j, e);
            if (k >= 0 && k != i && !s->mods[k].disabled && !placed[k]) return 0;
        }
    }
    arr = ModsJsonObjectFind(j, ROOT, "requires");
    if (arr >= 0 && j->toks[arr].type == MODS_JSON_ARRAY) {
        n = j->toks[arr].size;
        for (x = 0; x < n; x++) {
            char* cap;
            e = ModsJsonArrayAt(j, arr, x);
            cap = ModsJsonStrdup(arena, j, e);
            if (!cap || platform_provides(cap)) continue;
            for (p = 0; p < s->count; p++) {
                if (p == i || s->mods[p].disabled) continue;
                if (array_has(&s->mods[p], "provides", cap) && !placed[p]) return 0;
            }
        }
    }
    return 1;
}

/* Reorder set->mods so providers/depends_on targets precede dependents.
   Disabled mods go last. Cycles keep load order. Fixes ModAction.mod
   backpointers after the Mod structs move. */
static void topo_order(ModSet* set, ModsArena* arena,
                       ModsPlatformProvidesFn platform_provides) {
    int n = set->count;
    int* placed;
    int* perm;
    Mod* tmp;
    int np = 0, i, k, progress;

    if (n <= 1) return;
    placed = (int*)ModsArenaAllocZ(arena, (size_t)n * sizeof(int));
    perm   = (int*)ModsArenaAllocZ(arena, (size_t)n * sizeof(int));
    tmp    = (Mod*)ModsArenaAlloc(arena, (size_t)n * sizeof(Mod));
    if (!placed || !perm || !tmp) return;   /* OOM: leave load order */

    do {
        progress = 0;
        for (i = 0; i < n; i++) {
            if (placed[i] || set->mods[i].disabled) continue;
            if (deps_satisfied(set, i, placed, arena, platform_provides)) {
                perm[np++] = i; placed[i] = 1; progress = 1;
            }
        }
    } while (progress);

    for (i = 0; i < n; i++) {           /* cycle: remaining enabled, load order */
        if (!placed[i] && !set->mods[i].disabled) {
            ModsLogf(L"  resolve: dependency cycle at %S - keeping load order",
                     set->mods[i].mod_id);
            perm[np++] = i; placed[i] = 1;
        }
    }
    for (i = 0; i < n; i++)             /* disabled mods last */
        if (!placed[i]) { perm[np++] = i; placed[i] = 1; }

    for (k = 0; k < n; k++) tmp[k] = set->mods[perm[k]];
    for (k = 0; k < n; k++) set->mods[k] = tmp[k];
    for (k = 0; k < n; k++) {           /* re-point actions at moved Mod structs */
        int a;
        for (a = 0; a < set->mods[k].actions_count; a++)
            set->mods[k].actions[a].mod = &set->mods[k];
    }
}

void ModsResolve(ModSet* set, ModsArena* arena,
                 ModsPlatformProvidesFn platform_provides) {
    int i, enabled = 0, disabled = 0;
    if (!set || set->count == 0) return;
    ModsLogf(L"  resolve: %d mod(s) loaded", set->count);
    while (resolve_pass(set, arena, platform_provides)) {
        /* fixpoint: re-run until a sweep disables nothing new */
    }
    topo_order(set, arena, platform_provides);
    for (i = 0; i < set->count; i++) {
        if (set->mods[i].disabled) disabled++;
        else enabled++;
    }
    ModsLogf(L"  resolve: %d enabled / %d disabled; apply order:", enabled, disabled);
    for (i = 0; i < set->count; i++)
        if (!set->mods[i].disabled)
            ModsLogf(L"    %d. %S", i + 1, set->mods[i].mod_id);
}
