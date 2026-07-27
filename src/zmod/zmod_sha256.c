/* FIPS 180-4 SHA-256. */
#include <windows.h>
#include <string.h>
#include "zmod_sha256.h"

#define ROR(x, n)     (((x) >> (n)) | ((x) << (32 - (n))))
#define BSIG0(x)      (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define BSIG1(x)      (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SSIG0(x)      (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define SSIG1(x)      (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))
#define CH(x, y, z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

static const unsigned long K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static void compress(unsigned long state[8], const unsigned char blk[64]) {
    unsigned long w[64], a, b, c, d, e, f, g, h, t1, t2;
    int t;

    for (t = 0; t < 16; t++)
        w[t] = ((unsigned long)blk[t * 4] << 24) | ((unsigned long)blk[t * 4 + 1] << 16) |
               ((unsigned long)blk[t * 4 + 2] << 8) | (unsigned long)blk[t * 4 + 3];
    for (t = 16; t < 64; t++)
        w[t] = (SSIG1(w[t - 2]) + w[t - 7] + SSIG0(w[t - 15]) + w[t - 16]) & 0xFFFFFFFFUL;

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (t = 0; t < 64; t++) {
        t1 = (h + BSIG1(e) + CH(e, f, g) + K[t] + w[t]) & 0xFFFFFFFFUL;
        t2 = (BSIG0(a) + MAJ(a, b, c)) & 0xFFFFFFFFUL;
        h = g; g = f; f = e;
        e = (d + t1) & 0xFFFFFFFFUL;
        d = c; c = b; b = a;
        a = (t1 + t2) & 0xFFFFFFFFUL;
    }

    state[0] = (state[0] + a) & 0xFFFFFFFFUL;
    state[1] = (state[1] + b) & 0xFFFFFFFFUL;
    state[2] = (state[2] + c) & 0xFFFFFFFFUL;
    state[3] = (state[3] + d) & 0xFFFFFFFFUL;
    state[4] = (state[4] + e) & 0xFFFFFFFFUL;
    state[5] = (state[5] + f) & 0xFFFFFFFFUL;
    state[6] = (state[6] + g) & 0xFFFFFFFFUL;
    state[7] = (state[7] + h) & 0xFFFFFFFFUL;
}

void zmod_sha256_init(zmod_sha256* c) {
    c->state[0] = 0x6a09e667UL; c->state[1] = 0xbb67ae85UL;
    c->state[2] = 0x3c6ef372UL; c->state[3] = 0xa54ff53aUL;
    c->state[4] = 0x510e527fUL; c->state[5] = 0x9b05688cUL;
    c->state[6] = 0x1f83d9abUL; c->state[7] = 0x5be0cd19UL;
    c->len_lo = 0; c->len_hi = 0; c->buf_len = 0;
}

void zmod_sha256_update(zmod_sha256* c, const void* data, unsigned long n) {
    const unsigned char* p = (const unsigned char*)data;
    unsigned long prev = c->len_lo;

    c->len_lo = (c->len_lo + n) & 0xFFFFFFFFUL;
    if (c->len_lo < prev) c->len_hi++;

    if (c->buf_len) {
        unsigned int want = 64 - c->buf_len;
        unsigned int take = (n < want) ? (unsigned int)n : want;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += take; p += take; n -= take;
        if (c->buf_len < 64) return;
        compress(c->state, c->buf);
        c->buf_len = 0;
    }
    while (n >= 64) { compress(c->state, p); p += 64; n -= 64; }
    if (n) { memcpy(c->buf, p, (unsigned int)n); c->buf_len = (unsigned int)n; }
}

void zmod_sha256_final(zmod_sha256* c, unsigned char out[32]) {
    unsigned long bits_hi = (c->len_hi << 3) | (c->len_lo >> 29);
    unsigned long bits_lo = (c->len_lo << 3) & 0xFFFFFFFFUL;
    unsigned char tail[8];
    static const unsigned char pad[64] = { 0x80 };
    unsigned int padlen = (c->buf_len < 56) ? (56 - c->buf_len) : (120 - c->buf_len);
    int i;

    tail[0] = (unsigned char)(bits_hi >> 24); tail[1] = (unsigned char)(bits_hi >> 16);
    tail[2] = (unsigned char)(bits_hi >> 8);  tail[3] = (unsigned char)bits_hi;
    tail[4] = (unsigned char)(bits_lo >> 24); tail[5] = (unsigned char)(bits_lo >> 16);
    tail[6] = (unsigned char)(bits_lo >> 8);  tail[7] = (unsigned char)bits_lo;

    zmod_sha256_update(c, pad, padlen);
    zmod_sha256_update(c, tail, 8);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(c->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->state[i];
    }
}

int zmod_sha256_file(const wchar_t* path, char* hex_out) {
    static const char HX[] = "0123456789abcdef";
    static BYTE buf[16384];
    unsigned char dig[32];
    zmod_sha256 c;
    DWORD n;
    int i;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    zmod_sha256_init(&c);
    while (ReadFile(h, buf, sizeof(buf), &n, NULL) && n > 0) zmod_sha256_update(&c, buf, n);
    CloseHandle(h);
    zmod_sha256_final(&c, dig);

    for (i = 0; i < 32; i++) {
        hex_out[i * 2]     = HX[dig[i] >> 4];
        hex_out[i * 2 + 1] = HX[dig[i] & 0xf];
    }
    hex_out[64] = 0;
    return 1;
}

int zmod_sha256_hex_equal(const char* a, const char* b) {
    int i;
    for (i = 0; i < 64; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x || !y) return 0;
    }
    return a[64] == 0 && b[64] == 0;
}
