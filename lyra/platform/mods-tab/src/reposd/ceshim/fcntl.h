/* Minimal <fcntl.h> shim for Windows CE (OpenZDK has none). zlib's zutil.c pulls
   in gzguts.h (the gz file layer) when built without Z_SOLO; that header includes
   <fcntl.h>. reposd does not build or use the gz functions, so only the header
   needs to parse. These O_* values (POSIX defaults) are never exercised. */
#ifndef LYRA_CE_FCNTL_H
#define LYRA_CE_FCNTL_H
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_APPEND 0x0008
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_BINARY 0x8000
#endif
