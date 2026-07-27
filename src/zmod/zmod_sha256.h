#ifndef ZMOD_SHA256_H
#define ZMOD_SHA256_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned long state[8];
    unsigned long len_lo, len_hi;   /* message length in bytes */
    unsigned char buf[64];
    unsigned int  buf_len;
} zmod_sha256;

void zmod_sha256_init(zmod_sha256* c);
void zmod_sha256_update(zmod_sha256* c, const void* data, unsigned long n);
void zmod_sha256_final(zmod_sha256* c, unsigned char out[32]);

/* Lowercase hex into hex_out (65 bytes). 0 if the file cannot be opened. */
int zmod_sha256_file(const wchar_t* path, char* hex_out);

int zmod_sha256_hex_equal(const char* a, const char* b);

#ifdef __cplusplus
}
#endif

#endif /* ZMOD_SHA256_H */
