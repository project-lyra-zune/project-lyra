#include "mods_xus.h"
#include "mods_endian.h"

#include <string.h>

static int entries_reserve(ModsXus* x, ModsArena* arena, int wanted) {
    int new_cap;
    ModsXusEntry* ne;
    if (wanted <= x->cap) return 0;
    new_cap = x->cap ? x->cap : 8;
    while (new_cap < wanted) new_cap *= 2;
    ne = (ModsXusEntry*)ModsArenaAlloc(arena, new_cap * sizeof(ModsXusEntry));
    if (!ne) return -1;
    if (x->count) memcpy(ne, x->entries, x->count * sizeof(ModsXusEntry));
    x->entries = ne;
    x->cap = new_cap;
    return 0;
}

int ModsXusDecode(ModsArena* arena, const unsigned char* data, size_t len,
                  ModsXus* x) {
    int version, total_size, count, pos;
    int hdr_size;
    if (len < 12) return -1;
    if (data[0] != 'X' || data[1] != 'U' || data[2] != 'I' || data[3] != 'S')
        return -1;
    version = (int)U16BE(data + 4);
    /* data+6: reserved u16 (zero) */
    total_size = (int)U16BE(data + 8);
    count      = (int)U16BE(data + 10);
    if ((size_t)total_size != len) return -1;

    x->version       = version;
    x->entries       = NULL;
    x->count         = 0;
    x->cap           = 0;

    if (version == MODS_XUS_V_DENSE) {
        /* data+12: reserved u16. Body starts at +14. Decoded vector
           starts with the empty sentinel at index 0; on-disk records
           are 1-based. */
        hdr_size = MODS_XUS_HDR_DENSE;
        pos = hdr_size;
        if (entries_reserve(x, arena, count) < 0) return -1;
        /* index 0 sentinel */
        x->entries[x->count].id = 0;
        x->entries[x->count].text_be = NULL;
        x->entries[x->count].text_be_len = 0;
        x->count++;
        while (pos < (int)len) {
            int n_chars, text_len;
            if (pos + 2 > (int)len) return -1;
            n_chars = (int)U16BE(data + pos);
            pos += 2;
            text_len = n_chars * 2;
            if (pos + text_len > (int)len) return -1;
            if (entries_reserve(x, arena, x->count + 1) < 0) return -1;
            x->entries[x->count].id = 0;
            x->entries[x->count].text_be = data + pos;
            x->entries[x->count].text_be_len = text_len;
            x->count++;
            pos += text_len;
        }
        if (x->count != count) return -1;
        return 0;
    }

    if (version == MODS_XUS_V_ID_TAGGED) {
        hdr_size = MODS_XUS_HDR_TAGGED;
        pos = hdr_size;
        if (entries_reserve(x, arena, count) < 0) return -1;
        while (pos < (int)len) {
            int n_chars, text_len;
            if (pos + 2 > (int)len) return -1;
            n_chars = (int)U16BE(data + pos);
            pos += 2;
            text_len = n_chars * 2;
            if (pos + text_len + 4 > (int)len) return -1;
            if (entries_reserve(x, arena, x->count + 1) < 0) return -1;
            x->entries[x->count].text_be = data + pos;
            x->entries[x->count].text_be_len = text_len;
            pos += text_len;
            x->entries[x->count].id = (int)U32BE(data + pos);
            pos += 4;
            x->count++;
        }
        if (x->count != count) return -1;
        return 0;
    }

    return -1;
}

int ModsXusEncode(ModsArena* arena, const ModsXus* x,
                  unsigned char** out, size_t* out_len) {
    int i, body_len = 0, total;
    unsigned char* buf;
    int pos;

    if (x->version == MODS_XUS_V_DENSE) {
        if (x->count < 1) return -1;
        if (x->entries[0].text_be_len != 0) return -1;
        for (i = 1; i < x->count; i++)
            body_len += 2 + x->entries[i].text_be_len;
        total = MODS_XUS_HDR_DENSE + body_len;
        if (total > 0xFFFF) return -1;
        buf = (unsigned char*)ModsArenaAlloc(arena, total);
        if (!buf) return -1;
        buf[0] = 'X'; buf[1] = 'U'; buf[2] = 'I'; buf[3] = 'S';
        put_u16be(buf + 4, (unsigned)x->version);
        buf[6] = 0; buf[7] = 0;
        put_u16be(buf + 8, (unsigned)total);
        put_u16be(buf + 10, (unsigned)x->count);
        buf[12] = 0; buf[13] = 0;
        pos = MODS_XUS_HDR_DENSE;
        for (i = 1; i < x->count; i++) {
            int n = x->entries[i].text_be_len;
            put_u16be(buf + pos, (unsigned)(n / 2));
            pos += 2;
            if (n) memcpy(buf + pos, x->entries[i].text_be, n);
            pos += n;
        }
        *out = buf;
        *out_len = total;
        return 0;
    }

    if (x->version == MODS_XUS_V_ID_TAGGED) {
        for (i = 0; i < x->count; i++)
            body_len += 2 + x->entries[i].text_be_len + 4;
        total = MODS_XUS_HDR_TAGGED + body_len;
        if (total > 0xFFFF) return -1;
        buf = (unsigned char*)ModsArenaAlloc(arena, total);
        if (!buf) return -1;
        buf[0] = 'X'; buf[1] = 'U'; buf[2] = 'I'; buf[3] = 'S';
        put_u16be(buf + 4, (unsigned)x->version);
        buf[6] = 0; buf[7] = 0;
        put_u16be(buf + 8, (unsigned)total);
        put_u16be(buf + 10, (unsigned)x->count);
        pos = MODS_XUS_HDR_TAGGED;
        for (i = 0; i < x->count; i++) {
            int n = x->entries[i].text_be_len;
            put_u16be(buf + pos, (unsigned)(n / 2));
            pos += 2;
            if (n) memcpy(buf + pos, x->entries[i].text_be, n);
            pos += n;
            put_u32be(buf + pos, (unsigned)x->entries[i].id);
            pos += 4;
        }
        *out = buf;
        *out_len = total;
        return 0;
    }

    return -1;
}

/* ASCII UTF-8 → UTF-16BE bytes. Rejects any byte ≥ 0x80. */
static int ascii_utf8_to_utf16be(ModsArena* arena, const char* s,
                                  const unsigned char** out, int* out_len) {
    int n = (int)strlen(s);
    int i;
    unsigned char* buf;
    for (i = 0; i < n; i++) {
        if ((unsigned char)s[i] >= 0x80) return -1;
    }
    buf = (unsigned char*)ModsArenaAlloc(arena, n * 2);
    if (!buf && n) return -1;
    for (i = 0; i < n; i++) {
        buf[i * 2]     = 0;
        buf[i * 2 + 1] = (unsigned char)s[i];
    }
    *out = buf;
    *out_len = n * 2;
    return 0;
}

int ModsXusAppendDense(ModsXus* x, ModsArena* arena, const char* utf8_value,
                       int* assigned_index) {
    const unsigned char* be;
    int be_len;
    if (x->version != MODS_XUS_V_DENSE) return -1;
    if (ascii_utf8_to_utf16be(arena, utf8_value, &be, &be_len) < 0) return -1;
    if (entries_reserve(x, arena, x->count + 1) < 0) return -1;
    x->entries[x->count].id = 0;
    x->entries[x->count].text_be = be;
    x->entries[x->count].text_be_len = be_len;
    *assigned_index = x->count;
    x->count++;
    return 0;
}

int ModsXusSetDense(ModsXus* x, ModsArena* arena, int index,
                    const char* utf8_value) {
    const unsigned char* be;
    int be_len;
    if (x->version != MODS_XUS_V_DENSE) return -1;
    if (index < 0 || index >= x->count) return -1;
    if (ascii_utf8_to_utf16be(arena, utf8_value, &be, &be_len) < 0) return -1;
    x->entries[index].text_be = be;
    x->entries[index].text_be_len = be_len;
    return 0;
}
