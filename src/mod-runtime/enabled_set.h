#ifndef ENABLED_SET_H
#define ENABLED_SET_H

/* enabled.json is the sole on-disk authority for which mods are active;
   every consumer (boot apply, the Manage UI, reposd's install engine)
   projects from it through these functions and never parses or writes
   the file by hand. Portable C89, compiled into both zuxhook.dll and
   reposd.exe. */

#define ENABLED_ID_MAX  64      /* max mod_ids tracked (matches REPO_MAX_ROWS) */
#define ENABLED_ID_LEN  64      /* max mod_id bytes incl NUL (matches REPO_ID_LEN) */

#ifdef __cplusplus
extern "C" {
#endif

/* Read the enabled set into `ids` (each NUL-terminated, file order
   preserved). Returns the count [0..max], 0 when the file is absent or
   the array is empty, or -1 when the file is present but malformed. */
int EnabledSetRead(char ids[][ENABLED_ID_LEN], int max);

/* Overwrite enabled.json with `ids[0..count)` as
   {"version":1,"enabled":[...]}. Returns 0 on success, -1 on failure. */
int EnabledSetWrite(const char ids[][ENABLED_ID_LEN], int count);

/* 1 if `id` is in `ids[0..count)`, else 0. */
int EnabledSetContains(const char ids[][ENABLED_ID_LEN], int count,
                       const char* id);

/* Read-modify-write add. No-op (returns 0) if already present. Returns
   0 on success, -1 on failure or when the set is full. */
int EnabledSetAdd(const char* id);

/* Read-modify-write remove. No-op (returns 0) if absent. Returns 0 on
   success, -1 on write failure. */
int EnabledSetRemove(const char* id);

#ifdef __cplusplus
}
#endif

#endif
