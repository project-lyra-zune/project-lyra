#ifndef ZMOD_EXTRACT_H
#define ZMOD_EXTRACT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ZMOD_STAGE_NONE = 0,
    ZMOD_STAGE_OPEN,        /* rc = unzOpen2_64 gave NULL */
    ZMOD_STAGE_FIRSTFILE,   /* rc = minizip */
    ZMOD_STAGE_GETINFO,     /* rc = minizip */
    ZMOD_STAGE_OPENENTRY,   /* rc = minizip */
    ZMOD_STAGE_CREATEFILE,  /* rc = GetLastError */
    ZMOD_STAGE_READ         /* rc = minizip */
};

typedef struct {
    int  stage;
    long rc;
    char entry[128];
} zmod_error;

/* Unpack a .zmod (a plain ZIP) into dest. A destination held open by a running process
   is renamed <name>.old and the new bytes written to the freed name; the boot .old sweep
   clears it. Entries matching a defer_last name (case-insensitive, whole zip path) are
   written only after every other entry has landed. Returns 1, else 0 with err filled. */
int zmod_extract(const wchar_t* zmod, const wchar_t* dest,
                 const char* const* defer_last, int n_defer, zmod_error* err);

#ifdef __cplusplus
}
#endif

#endif /* ZMOD_EXTRACT_H */
