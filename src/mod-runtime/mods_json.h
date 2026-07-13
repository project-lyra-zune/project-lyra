#ifndef MODS_JSON_H
#define MODS_JSON_H

#include <stddef.h>
#include "mods_arena.h"

/* Minimal JSON parser. Token-based (jsmn-style): every token records its
   type, byte range in the source, and number of immediate children
   (objects' children count k+v pairs as 2*N; arrays just count elements).

   Restrictions:
     - Source must be valid UTF-8. ASCII-only strings simplify everything
       and are sufficient for mod manifests.
     - Escape sequences supported: \" \\ \/ \n \r \t \uXXXX (BMP only).
     - No surrogate-pair decoding for \u escapes beyond BMP.
     - Numbers are surfaced as untyped tokens; consumers parse with
       ModsJsonInt on demand. */

typedef enum {
    MODS_JSON_UNDEF = 0,
    MODS_JSON_OBJECT,
    MODS_JSON_ARRAY,
    MODS_JSON_STRING,
    MODS_JSON_PRIMITIVE   /* number, true, false, null */
} ModsJsonType;

typedef struct {
    ModsJsonType type;
    int start;     /* byte offset of first content char */
    int end;       /* byte offset one past last content char */
    int size;      /* count of immediate children (kv pairs counted 2x) */
} ModsJsonTok;

typedef struct {
    const char*       src;   /* not owned; must outlive token array uses */
    size_t            srclen;
    ModsJsonTok*      toks;
    int               ntoks;
} ModsJson;

#ifdef __cplusplus
extern "C" {
#endif

/* Parse `src` (length `srclen`, UTF-8) into a token array allocated in
   `arena`. Returns 0 on success, -1 on failure. `*out` is populated on
   both paths but ntoks==0 on failure. */
int ModsJsonParse(ModsArena* arena, const char* src, size_t srclen,
                  ModsJson* out);

/* Within object token `obj_idx`, find the value token index for the
   given key. Returns -1 if not found. `obj_idx` must point at an
   OBJECT token. */
int ModsJsonObjectFind(const ModsJson* j, int obj_idx, const char* key);

/* Iterate over an array. `arr_idx` must point at an ARRAY token. Returns
   the token index of the i-th element (i ∈ [0, size)), or -1. */
int ModsJsonArrayAt(const ModsJson* j, int arr_idx, int i);

/* Compare a string token's content to a NUL-terminated key. Handles
   simple escape sequences. */
int ModsJsonStrEq(const ModsJson* j, int tok_idx, const char* s);

/* Allocate a NUL-terminated copy of the string token's decoded value
   in `arena`. Returns NULL on bad input or OOM. */
char* ModsJsonStrdup(ModsArena* arena, const ModsJson* j, int tok_idx);

/* Decode a primitive token as a signed integer. Supports decimal,
   "0xNN" hex, and negative values. On failure leaves *out unchanged
   and returns -1. */
int ModsJsonInt(const ModsJson* j, int tok_idx, int* out);

/* Decode a primitive true/false token as a boolean (*out = 0/1). Returns 0 on
   success, -1 if the token isn't a boolean primitive. The single source for
   reading a JSON bool; JSON true/false is a PRIMITIVE token, not a STRING, so
   ModsJsonStrEq never matches it. */
int ModsJsonBool(const ModsJson* j, int tok_idx, int* out);

/* Type accessor (UNDEF for out-of-range). */
ModsJsonType ModsJsonTypeOf(const ModsJson* j, int tok_idx);

/* Step past one full token (including its descendants). Returns the
   index immediately following the subtree rooted at `tok_idx`. Used
   to walk object key-value pairs. */
int ModsJsonSkip(const ModsJson* j, int tok_idx);

#ifdef __cplusplus
}
#endif

#endif
