/* repo_ceio.h: CE replacement for minizip's fill_win32_filefunc64W. */
#ifndef REPO_CEIO_H
#define REPO_CEIO_H

#include "ioapi.h"

#ifdef __cplusplus
extern "C" {
#endif

void fill_ce_filefunc64W(zlib_filefunc64_def *p);

#ifdef __cplusplus
}
#endif

#endif /* REPO_CEIO_H */
