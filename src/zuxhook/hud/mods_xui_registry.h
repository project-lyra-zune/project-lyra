#ifndef MODS_XUI_REGISTRY_H
#define MODS_XUI_REGISTRY_H

#include <windows.h>

/* xuidll runtime class registry (v4.5). In xuidll.dll's writable data:
   27 buckets of 4-byte head pointers, each chaining 20-byte entries:
     +0x00 hash  +0x04 name_ptr(wchar_t*)  +0x08 info_ptr  +0x0c next  +0x10 back
   xuidll is at the same image base (0x41800000) in gemstone AND
   compositor, so this base and walk are valid in both processes; the
   gemstone Phase 2 pass and the compositor class-reg pass share this one
   copy. */
#define XUIDLL_REGISTRY_BASE      0x41874060u
#define XUIDLL_REGISTRY_BUCKETS   27

typedef struct {
    DWORD hash;
    DWORD name_ptr;
    DWORD info_ptr;
    DWORD next;
    DWORD back;
} RegistryEntry;

/* SEH-wrapped class-registry walk. If `find` is non-NULL, returns 1 on the
   first name match (case-insensitive), 0 if absent. Always sets *out_total
   (when non-NULL) to the number of entries walked. Returns -1 on access
   violation (xuidll not loaded / registry not yet present). */

#ifdef __cplusplus
extern "C" {
#endif

int walk_registry(const wchar_t* find, int* out_total);

#ifdef __cplusplus
}
#endif

#endif /* MODS_XUI_REGISTRY_H */
