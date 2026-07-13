#ifndef MODS_ARENA_H
#define MODS_ARENA_H

#include <stddef.h>

/* Per-apply bump allocator. One arena lives for the duration of a
   ModsApplyPhase1 call; everything allocated through it is freed at
   ModsArenaFree time. No fine-grained free. */
typedef struct ModsArena {
    unsigned char* base;
    size_t size;
    size_t used;
    int oom;       /* sticky: once OOM, all subsequent allocs return NULL */
} ModsArena;

#ifdef __cplusplus
extern "C" {
#endif

int   ModsArenaInit(ModsArena* a, size_t bytes);
void  ModsArenaFree(ModsArena* a);
void* ModsArenaAlloc(ModsArena* a, size_t n);   /* 8-byte aligned */
void* ModsArenaAllocZ(ModsArena* a, size_t n);
char* ModsArenaStrdupN(ModsArena* a, const char* s, size_t n);
char* ModsArenaStrdup(ModsArena* a, const char* s);

#ifdef __cplusplus
}
#endif

#endif
