#ifndef MODS_ENDIAN_H
#define MODS_ENDIAN_H

/* Byte-order read/write helpers for the XUI binary formats (XUS/XUIZ/XUR).
   The XUI containers are big-endian; a few XUIZ length fields are little-endian.
   Read macros take a const unsigned char*; writers take a mutable one. */

#define U16BE(p) (((unsigned)(p)[0] << 8) | (unsigned)(p)[1])
#define U24BE(p) (((unsigned)(p)[0] << 16) | ((unsigned)(p)[1] << 8) | (unsigned)(p)[2])
#define U32BE(p) (((unsigned)(p)[0] << 24) | ((unsigned)(p)[1] << 16) | \
                  ((unsigned)(p)[2] <<  8) |  (unsigned)(p)[3])
#define U16LE(p) (((unsigned)(p)[1] << 8) | (unsigned)(p)[0])

static __inline void put_u16be(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static __inline void put_u32be(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >>  8);
    p[3] = (unsigned char)v;
}

static __inline void put_u16le(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

#endif
