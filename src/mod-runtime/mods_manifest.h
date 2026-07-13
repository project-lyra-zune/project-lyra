#ifndef MODS_MANIFEST_H
#define MODS_MANIFEST_H

#include <windows.h>
#include "mods_arena.h"
#include "mods_json.h"

/* Manifest + per-mod state.

   A Mod owns its arena-allocated JSON source + token array. Actions
   reference the JSON by token index; capability handlers read args
   lazily via the ModAction* + helpers below.

   Back-references are scalar DWORDs. Width chosen to hold any kernel
   VA (0xC0000000..0xE0000000 range) or runtime-allocated heap address
   returned by kcall (NvOsAlloc results); narrower types would clip
   the high bit and break VA arithmetic. xus_add_string still returns
   small index values; they fit fine. */

#define MODS_MAX_PATH 260

typedef struct {
    const char*  name;        /* arena-owned NUL-terminated */
    DWORD        value;
} ModsBackRef;

typedef struct {
    ModsBackRef*  refs;
    int           count;
    int           cap;
} ModsBackRefScope;

struct Mod;

typedef struct ModAction {
    const char*       type;          /* arena copy of action.type */
    const char*       back_ref;      /* NULL or arena-owned key */
    int               action_tok;    /* JSON token index of the action object */
    int               index;         /* 0-based action index within mod */
    struct Mod*       mod;           /* backpointer */
    struct ModActionArg* args;       /* synthetic args; NULL for JSON actions */
    int               args_count;
} ModAction;

typedef enum {
    MOD_ACTION_ARG_STRING = 1,
    MOD_ACTION_ARG_BOOL   = 2,
    MOD_ACTION_ARG_U32    = 3,
    MOD_ACTION_ARG_BYTES  = 4
} ModActionArgKind;

typedef struct ModActionArg {
    const char* key;
    ModActionArgKind kind;
    const char* string_value;
    int bool_value;
    DWORD u32_value;
    const unsigned char* bytes_value;
    int bytes_len;
} ModActionArg;

typedef struct Mod {
    const char*        mod_id;       /* arena-owned */
    const char*        name;
    const char*        version;
    wchar_t            source_dir[MODS_MAX_PATH];   /* absolute path, no trailing \ */
    ModsJson           json;         /* tokens + src pointer */
    const char*        json_src;     /* arena-owned UTF-8 source */
    ModAction*         actions;
    int                actions_count;
    ModsBackRefScope   scope;
    int                disabled;     /* set by ModsResolve if a relation is unmet */
    const char*        disabled_reason; /* arena-owned short reason when disabled, else NULL */
} Mod;

typedef struct ModSet {
    Mod*   mods;
    int    count;
    int    cap;
} ModSet;

#ifdef __cplusplus
extern "C" {
#endif

/* Read enabled.json + load each enabled mod's manifest into `out`.
   Returns 0 on success (even if `out->count == 0`). Returns -1 on
   catastrophic failure (mods dir missing, OOM). Per-mod load failures
   are logged and the mod is skipped; the overall call still succeeds.

   `mods_root` is e.g. L"\\flash2\\automation\\mods". */
int ModsManifestLoadAll(ModsArena* arena, const wchar_t* mods_root,
                        ModSet* out);

/* Which phase owns a capability type: 1 = compositor (Phase 1, file I/O: gem/xus
   composition, registry, blob/daemon), 2 = servicesd/gemstone (Phase 2, XUI
   surfaces + kerncore-backed kernel caps), MODS_CAP_PHASE_NONE = not a runtime
   capability. This is the single source of truth both phase dispatchers consult:
   each handles only its own phase's caps and defers/skips the other, so a cap's
   phase is declared exactly once (no per-phase skip-lists to drift out of sync). */
#define MODS_CAP_PHASE_NONE  (-1)
int ModsCapabilityPhase(const char* type);

/* Read an arg by key as a NUL-terminated arena-allocated string.
   Resolves "@/path" blob refs against the mod's source_dir, returning
   the joined UTF-16 absolute path in `out_path_w` (caller-supplied
   buffer of `path_cap` wchar_t). For non-blob string values, copies
   the decoded UTF-8 into `out_utf8` (NULL if not needed) AND sets
   *out_path_w[0] = 0 unless it's a blob. Returns:
     0 = scalar string copied to *out_utf8
     1 = blob path resolved to *out_path_w
    -1 = missing key, OOM, or wrong type. */
int ModActionGetString(const ModAction* a, const char* key,
                       ModsArena* arena,
                       const char** out_utf8,
                       wchar_t* out_path_w, int path_cap);

/* Read an arg as a signed int. Resolves "$name" against the mod's
   back-ref scope. Present value -> returns 0 with *out set. Missing key ->
   returns 0 with *out = default_value. Returns -1 with *out = default_value
   on type mismatch / unresolved back-ref.

   Note: for VA / DWORD values, prefer ModActionGetU32; it widens to
   DWORD, parses "0xHEX" string values, and supports "$name + 0xN" /
   "$name - 0xN" arithmetic. Use ModActionGetInt only for small signed
   indices (e.g. xus_add_string). */
int ModActionGetInt(const ModAction* a, const char* key,
                    int default_value, int* out);

/* Read an arg as a bool. Accepts only JSON `true`/`false`. Missing key ->
   returns 0 with *out = default_value. A present-but-malformed value (a number,
   string, object, …) returns -1 with *out = default_value; fail fast, no
   silent coerce. */
int ModActionGetBool(const ModAction* a, const char* key,
                     int default_value, int* out);

/* Read an arg as a DWORD. Accepts:
     - primitive int (decimal or 0xHEX)
     - "0xHEX" string
     - "$name" back-ref
     - "$name + 0xN" / "$name + N" / "$name - 0xN" / "$name - N"
       (arithmetic on a $name back-ref; whitespace tolerant)
   If missing, returns 0 (no error). Returns -1 on type mismatch /
   unresolved ref / parse error. On success, *out is set. */
int ModActionGetU32(const ModAction* a, const char* key, DWORD* out);

/* Same as ModActionGetU32 but if the key is missing returns -2 so the
   caller can distinguish "absent" from "present-but-error". */
int ModActionGetU32Required(const ModAction* a, const char* key,
                             DWORD* out);

/* Read an arena-owned generated blob from a synthetic action. JSON-authored
   actions cannot carry raw bytes, so this returns -1 for non-synthetic args. */
int ModActionGetBytes(const ModAction* a, const char* key,
                      const unsigned char** out, int* out_len);

/* Resolve any JSON token (typically nested inside an action arg array
   element) to a DWORD with the same semantics as ModActionGetU32:
   primitive int, "0xHEX" string, "$name" back-ref, "$name +/- N"
   arithmetic. Returns 0 on success, -1 on type / parse error. */
int ModResolveTokU32(const Mod* m, int tok_idx, DWORD* out);

/* Add or overwrite a back-ref in the mod's scope. */
int ModScopeSet(Mod* m, ModsArena* arena, const char* name, DWORD value);

#ifdef __cplusplus
}
#endif

#endif
