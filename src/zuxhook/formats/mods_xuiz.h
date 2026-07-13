#ifndef MODS_XUIZ_H
#define MODS_XUIZ_H

#include <stddef.h>
#include "mods_arena.h"

/* XUIZ (X-UI Zip) container codec, a named-entry archive of XUI blobs.

   Container layout:
     +0x00..03  magic "XUIZ"
     +0x04..07  u32 BE version (= 1)
     +0x08..0B  u32 BE total file size
     +0x0C..0F  reserved 0
     +0x10..13  u32 BE blob_ptr_field - content_base = 0x16 + blob_ptr_field
     +0x14..15  u16 BE entry count
     +0x16..    entry header table

   Per-entry header (in emission order):
     size            entry 0: u32 BE
                     entry 1: u24 BE
                     entries 2+: u16 BE if size ≤ 0xFFFF, else u32 BE
                                 - u16: preceded by 1-byte 0x00 pad
                                 - u32: the leading 0x00 byte overlaps the
                                        previous entry's UTF-16LE name
                                        terminator (i.e., it shares the
                                        same on-disk byte)
     offset          u32 BE - relative to content blob base
     name_len        u16 LE - UTF-16LE char count (the count, not bytes)
     name            UTF-16LE bytes (name_len * 2)
     (last entry: name's trailing 0x00 high byte is OMITTED; content
      blob starts right after the last char's low byte)

   In-memory entries hold the name as UTF-16LE bytes (NOT length-prefixed,
   no NUL). The content blob is held by pointer (not owned) into the
   source buffer for decoded entries; or by a fresh arena allocation for
   newly-added entries. */

typedef struct {
    const unsigned char* name_le;        /* UTF-16LE bytes; not null-terminated */
    int                  name_chars;     /* count of UTF-16 code units (no NUL) */
    const unsigned char* data;           /* entry contents */
    int                  data_len;
} ModsXuizEntry;

typedef struct {
    ModsXuizEntry*  entries;
    int             count;
    int             cap;
} ModsXuiz;

#ifdef __cplusplus
extern "C" {
#endif

/* Parse `data[0..len)` into x->entries. On success entries point into
   `data` (no allocation). Returns 0 on success, -1 on failure. */
int ModsXuizDecode(ModsArena* arena, const unsigned char* data, size_t len,
                   ModsXuiz* x);

/* Pack `x` into a freshly-allocated buffer in `arena`. Returns 0 on
   success; *out / *out_len point at the result. */
int ModsXuizEncode(ModsArena* arena, const ModsXuiz* x,
                   unsigned char** out, size_t* out_len);

/* Look up an entry by ASCII name (the entry's name is UTF-16LE; this
   helper does the ASCII match). Returns the index or -1 if not found. */
int ModsXuizFind(const ModsXuiz* x, const char* ascii_name);

/* Append a new entry. The caller-provided buffers must outlive the
   ModsXuiz (i.e., they should be arena-allocated). Returns 0 on
   success, -1 on OOM. */
int ModsXuizAppend(ModsXuiz* x, ModsArena* arena,
                   const unsigned char* name_le, int name_chars,
                   const unsigned char* data, int data_len);

/* Replace the data of an existing entry. Buffer must outlive x. */
int ModsXuizReplaceData(ModsXuiz* x, int idx,
                        const unsigned char* data, int data_len);

/* Remove an entry by index. Shifts the tail down. */
int ModsXuizRemove(ModsXuiz* x, int idx);

/* Helper: ASCII → UTF-16LE bytes (no NUL terminator). Rejects non-ASCII. */
int ModsXuizAsciiToLe(ModsArena* arena, const char* ascii,
                      const unsigned char** out_le, int* out_chars);

#ifdef __cplusplus
}
#endif

#endif
