#ifndef MODS_CAPABILITY_H
#define MODS_CAPABILITY_H

/* Capability compatibility matching. Two string forms, disambiguated by number
   count so no token is overloaded:
     point (a required revision): "name" (rev 1) or "name@r"
     range (a provided window):   "name" ([1,1]) or "name@lo:hi" (lo=min_compat, hi=cur)
   A provider satisfies a requirement iff the names match and lo <= r <= hi.
   Shared by the boot resolver (mods_resolve.c) and the Browse install gate. */

#define MODS_CAP_NAME_MAX 64

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a point string ("name" or "name@r") into a bounded name and revision (default 1). */
void ModsCapParse(const char* s, char* name_out, int name_cap, int* rev_out);

/* Parse a range string ("name" or "name@lo:hi") into a bounded name and window
   [lo,hi] (default [1,1]). A bare "name@n" (no colon; the validator rejects it) is
   read leniently as [1,n]. */
void ModsCapParseRange(const char* s, char* name_out, int name_cap, int* lo_out, int* hi_out);

/* Names equal, ignoring any @suffix on either side. */
int ModsCapNameEq(const char* a, const char* b);

/* Is required revision r within the window [lo,hi]? */
int ModsCapRangeSatisfies(int lo, int hi, int r);

/* Does provided range `provided` satisfy required point `required`? Same name and
   the required revision within the provided window. */
int ModsCapSatisfies(const char* provided, const char* required);

#ifdef __cplusplus
}
#endif

#endif /* MODS_CAPABILITY_H */
