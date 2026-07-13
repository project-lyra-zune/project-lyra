/* repo_ceio.c: a zlib_filefunc64 over the Windows CE file API, replacing
 * minizip's desktop-only iowin32.c. Drives unzip.c with wide paths + 64-bit seek. */
#include <windows.h>
#include "ioapi.h"
#include "repo_ceio.h"

static voidpf ZCALLBACK ce_open64(voidpf opaque, const void *filename, int mode) {
    DWORD access = GENERIC_READ, share = FILE_SHARE_READ, disp = OPEN_EXISTING;
    HANDLE h;
    (void)opaque;
    if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) == ZLIB_FILEFUNC_MODE_READ) {
        access = GENERIC_READ; disp = OPEN_EXISTING;
    } else if (mode & ZLIB_FILEFUNC_MODE_EXISTING) {
        access = GENERIC_READ | GENERIC_WRITE; disp = OPEN_EXISTING;
    } else if (mode & ZLIB_FILEFUNC_MODE_CREATE) {
        access = GENERIC_READ | GENERIC_WRITE; disp = CREATE_ALWAYS;
    }
    if (filename == NULL) return NULL;
    h = CreateFileW((const wchar_t *)filename, access, share, NULL, disp,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    return (h == INVALID_HANDLE_VALUE) ? NULL : (voidpf)h;
}

static uLong ZCALLBACK ce_read(voidpf opaque, voidpf stream, void *buf, uLong size) {
    DWORD got = 0;
    (void)opaque;
    if (!ReadFile((HANDLE)stream, buf, size, &got, NULL)) return 0;
    return (uLong)got;
}

static uLong ZCALLBACK ce_write(voidpf opaque, voidpf stream, const void *buf, uLong size) {
    DWORD put = 0;
    (void)opaque;
    if (!WriteFile((HANDLE)stream, (LPVOID)buf, size, &put, NULL)) return 0;
    return (uLong)put;
}

static ZPOS64_T ZCALLBACK ce_tell64(voidpf opaque, voidpf stream) {
    LONG hi = 0;
    DWORD lo;
    (void)opaque;
    lo = SetFilePointer((HANDLE)stream, 0, &hi, FILE_CURRENT);
    return ((ZPOS64_T)(DWORD)hi << 32) | (ZPOS64_T)lo;
}

static long ZCALLBACK ce_seek64(voidpf opaque, voidpf stream, ZPOS64_T offset, int origin) {
    LONG hi = (LONG)(offset >> 32);
    DWORD lo = (DWORD)(offset & 0xFFFFFFFF);
    DWORD method = FILE_BEGIN;
    DWORD r;
    (void)opaque;
    if (origin == ZLIB_FILEFUNC_SEEK_CUR) method = FILE_CURRENT;
    else if (origin == ZLIB_FILEFUNC_SEEK_END) method = FILE_END;
    r = SetFilePointer((HANDLE)stream, (LONG)lo, &hi, method);
    if (r == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) return -1;
    return 0;
}

static int ZCALLBACK ce_close(voidpf opaque, voidpf stream) {
    (void)opaque;
    return CloseHandle((HANDLE)stream) ? 0 : -1;
}

static int ZCALLBACK ce_error(voidpf opaque, voidpf stream) {
    (void)opaque; (void)stream;
    return 0;
}

void fill_ce_filefunc64W(zlib_filefunc64_def *p) {
    p->zopen64_file = ce_open64;
    p->zread_file   = ce_read;
    p->zwrite_file  = ce_write;
    p->ztell64_file = ce_tell64;
    p->zseek64_file = ce_seek64;
    p->zclose_file  = ce_close;
    p->zerror_file  = ce_error;
    p->opaque       = NULL;
}
