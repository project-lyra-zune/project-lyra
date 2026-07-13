#pragma once

#include "nativeapp_common.h"

// Little-endian read/write for the TCP daemon wire protocol (request args and
// response frames). Wire order is LE; the XUI binary formats use big-endian
// (see zuxhook/mods_endian.h). Keep the two straight.

static __inline u32 read_u32(const unsigned char* p) {
    return  (u32)p[0]        | ((u32)p[1] << 8)
         | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static __inline void write_u32(unsigned char* p, u32 v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static __inline u32 read_u16(const unsigned char* p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static __inline void write_u16(unsigned char* p, u32 v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}
