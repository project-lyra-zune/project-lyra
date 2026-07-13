/* Minimal <io.h> shim for CE: zlib gzguts.h includes it, but reposd never
   builds or calls the gz file layer, so it only needs to parse. */
#ifndef LYRA_CE_IO_H
#define LYRA_CE_IO_H
#endif
