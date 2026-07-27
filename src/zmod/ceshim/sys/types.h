/* Minimal <sys/types.h> shim for Windows CE (OpenZDK has no sys/ headers).
   zlib's zconf.h includes it "for off_t"; supply just that so the vendored zlib
   compiles unpatched WITHOUT Z_SOLO; Z_SOLO would drop zlib's default malloc
   allocator, which minizip's inflate (zalloc=0) depends on. */
#ifndef LYRA_CE_SYS_TYPES_H
#define LYRA_CE_SYS_TYPES_H
typedef long off_t;
#endif
