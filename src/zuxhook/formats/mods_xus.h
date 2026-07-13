#ifndef MODS_XUS_H
#define MODS_XUS_H

#include <stddef.h>
#include "mods_arena.h"

/* XUS (Xbox UI Strings) codec: a header (magic "XUIS", version, total
   size, count) followed by a table of UTF-16BE string entries.

   Two versions exist in v4.5 firmware:
     v0x0102 - dense indexed table; entries are 1-based (entries[0] is
               the empty sentinel)
     v0x0101 - id-tagged per-scene overrides; each entry carries an
               embedded element id

   All on-disk strings are UTF-16BE; in this codec they're held as raw
   big-endian byte buffers so the encode path is byte-identical to disk
   without endian conversion or wchar_t aliasing.

   For ModsXusAppendDense / ModsXusSetDense, the caller passes ASCII
   UTF-8 input (mods are ASCII-only by manifest convention); the codec
   handles UTF-8 → UTF-16BE conversion. */

#define MODS_XUS_V_DENSE      0x0102
#define MODS_XUS_V_ID_TAGGED  0x0101
#define MODS_XUS_HDR_DENSE    14
#define MODS_XUS_HDR_TAGGED   12

typedef struct {
    int                     id;            /* v0x0101 only: embedded id; ignored for dense */
    const unsigned char*    text_be;       /* UTF-16BE bytes, NOT length-prefixed */
    int                     text_be_len;   /* even; 2 * n_chars */
} ModsXusEntry;

typedef struct {
    int            version;       /* 0x0102 or 0x0101 */
    ModsXusEntry*  entries;
    int            count;         /* live entry count */
    int            cap;           /* allocated capacity */
} ModsXus;

#ifdef __cplusplus
extern "C" {
#endif

/* Decode `data[0..len)`. Returns 0 on success. On success, x->entries
   points at an arena-allocated array. For v0x0102, x->entries[0] is
   always a 0-length sentinel; real strings start at index 1.

   On failure returns -1; x is left in an undefined state. */
int ModsXusDecode(ModsArena* arena, const unsigned char* data, size_t len,
                  ModsXus* x);

/* Encode `x` back to a contiguous buffer in `arena`. Output length goes
   into *out_len. Returns 0 on success. Refuses if a v0x0102 table's
   entries[0] has a non-empty body. */
int ModsXusEncode(ModsArena* arena, const ModsXus* x,
                  unsigned char** out, size_t* out_len);

/* Append a string to a dense table. UTF-8 input; ASCII only.
   `*assigned_index` is set to the 1-based string id of the new entry.
   Returns 0 on success, -1 on bad input/OOM/non-dense. */
int ModsXusAppendDense(ModsXus* x, ModsArena* arena, const char* utf8_value,
                       int* assigned_index);

/* Replace an existing dense-table entry at `index` (1-based). Returns
   0 on success, -1 if index out of range or table isn't dense. */
int ModsXusSetDense(ModsXus* x, ModsArena* arena, int index,
                    const char* utf8_value);

#ifdef __cplusplus
}
#endif

#endif
