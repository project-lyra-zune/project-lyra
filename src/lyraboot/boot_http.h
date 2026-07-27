#ifndef BOOT_HTTP_H
#define BOOT_HTTP_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BOOT_HTTP_OK = 0,
    BOOT_HTTP_ERR_URL,          /* not an http:// url, or unparseable */
    BOOT_HTTP_ERR_RESOLVE,
    BOOT_HTTP_ERR_CONNECT,
    BOOT_HTTP_ERR_SEND,
    BOOT_HTTP_ERR_RECV,         /* timeout or reset mid-transfer */
    BOOT_HTTP_ERR_RESPONSE,     /* malformed status line or headers */
    BOOT_HTTP_ERR_STATUS,       /* http_status holds the code */
    BOOT_HTTP_ERR_NO_LENGTH,    /* no Content-Length; chunked is not supported */
    BOOT_HTTP_ERR_TOO_LARGE,
    BOOT_HTTP_ERR_SHORT,        /* closed before Content-Length bytes arrived */
    BOOT_HTTP_ERR_FILE
};

typedef void (*boot_http_progress)(unsigned long got, unsigned long total, void* ctx);

/* GET url into dest. Plain HTTP only, no redirects, no chunked decoding. */
int boot_http_download(const char* url, const wchar_t* dest, unsigned long max_bytes,
                       boot_http_progress cb, void* ctx,
                       int* http_status, unsigned long* got_out);

const char* boot_http_result_str(int r);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_HTTP_H */
