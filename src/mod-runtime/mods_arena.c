#include "mods_arena.h"

#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define MODS_ALLOC(n)  LocalAlloc(LPTR, (n))
#  define MODS_FREE(p)   LocalFree((p))
#else
#  include <stdlib.h>
#  define MODS_ALLOC(n)  calloc(1, (n))
#  define MODS_FREE(p)   free((p))
#endif

#define ALIGN_UP(n, a) (((n) + ((a) - 1)) & ~((size_t)((a) - 1)))

int ModsArenaInit(ModsArena* a, size_t bytes) {
    a->base = (unsigned char*)MODS_ALLOC(bytes);
    a->size = a->base ? bytes : 0;
    a->used = 0;
    a->oom  = a->base ? 0 : 1;
    return a->base ? 0 : -1;
}

void ModsArenaFree(ModsArena* a) {
    if (a->base) MODS_FREE(a->base);
    a->base = NULL;
    a->size = 0;
    a->used = 0;
}

void* ModsArenaAlloc(ModsArena* a, size_t n) {
    size_t off;
    if (a->oom || a->base == NULL) return NULL;
    off = ALIGN_UP(a->used, 8);
    if (off + n > a->size) { a->oom = 1; return NULL; }
    a->used = off + n;
    return a->base + off;
}

void* ModsArenaAllocZ(ModsArena* a, size_t n) {
    void* p = ModsArenaAlloc(a, n);
    if (p) memset(p, 0, n);
    return p;
}

char* ModsArenaStrdupN(ModsArena* a, const char* s, size_t n) {
    char* p = (char*)ModsArenaAlloc(a, n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char* ModsArenaStrdup(ModsArena* a, const char* s) {
    return ModsArenaStrdupN(a, s, s ? strlen(s) : 0);
}
