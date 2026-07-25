#ifndef FAILED_SET_H
#define FAILED_SET_H

#include <stddef.h>
#include "enabled_set.h"   /* ENABLED_ID_LEN */

/* Mods that failed to apply in Phase 1, for the rest of this boot only.

   Phase 1 runs in the compositor and Phase 2 in gemstone, so the set crosses
   processes through a file, the same way back-refs do. It is boot-scoped:
   Phase 1 truncates it before applying, and nothing reads it afterwards.

   A mod whose capability FAULTS is disabled outright and never reaches Phase 2
   at all. This covers the softer case, an action that returns failure without
   raising: the mod is half-applied, so it must not go on to receive
   patch_bytes or load_module in the same boot.

   failed_set.c is free of windows.h and holds the matching; failed_set_file.c
   holds the flash I/O. */

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when `buf` (newline-separated ids) holds `mod_id` as a whole line. Matching
   whole lines matters: a prefix match would skip "night" for "night-mode". */
int  FailedSetBufContains(const char* buf, const char* mod_id);

void FailedSetClear(void);
void FailedSetAdd(const char* mod_id);
int  FailedSetContains(const char* mod_id);

#ifdef __cplusplus
}
#endif

#endif
