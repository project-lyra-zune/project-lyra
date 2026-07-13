#include "mods_xuiz.h"
#include "mods_endian.h"

#include <string.h>

static int reserve(ModsXuiz* x, ModsArena* arena, int wanted) {
    int new_cap;
    ModsXuizEntry* ne;
    if (wanted <= x->cap) return 0;
    new_cap = x->cap ? x->cap : 16;
    while (new_cap < wanted) new_cap *= 2;
    ne = (ModsXuizEntry*)ModsArenaAlloc(arena, new_cap * sizeof(ModsXuizEntry));
    if (!ne) return -1;
    if (x->count) memcpy(ne, x->entries, x->count * sizeof(ModsXuizEntry));
    x->entries = ne;
    x->cap = new_cap;
    return 0;
}

int ModsXuizDecode(ModsArena* arena, const unsigned char* data, size_t len,
                   ModsXuiz* x) {
    unsigned total_size, blob_ptr_field;
    int content_base, entry_count;
    int p, entry_idx;
    int prev_offset;

    x->entries = NULL; x->count = 0; x->cap = 0;
    if (len < 0x16) return -1;
    if (data[0] != 'X' || data[1] != 'U' || data[2] != 'I' || data[3] != 'Z')
        return -1;
    /* version at +4 is u32 BE = 1; not strictly validated. */
    total_size = U32BE(data + 8);
    if ((size_t)total_size != len) return -1;
    blob_ptr_field = U32BE(data + 0x10);
    entry_count    = (int)U16BE(data + 0x14);
    content_base   = 0x16 + (int)blob_ptr_field;
    if (content_base >= (int)len) return -1;
    if (reserve(x, arena, entry_count) < 0) return -1;

    p = 0x16;
    entry_idx = 0;
    prev_offset = -1;
    while (x->count < entry_count && p < (int)len - 8) {
        int size_bytes, size_val;
        int offset_p, name_len_p, name_off;
        unsigned offset_val;
        int name_chars;
        int name_byte_len;
        const unsigned char* name_ptr;
        const unsigned char* content_ptr;
        unsigned char* synthetic_name;

        if (entry_idx == 0) {
            size_bytes = 4;
            size_val   = (int)U32BE(data + p);
        } else if (entry_idx == 1) {
            size_bytes = 3;
            size_val   = (int)U24BE(data + p);
        } else {
            /* Disambiguate u16 vs u32 by validating each candidate's
               offset+size fits within the file. */
            int u16_size = -1, u16_off = -1, u16_valid = 0;
            int u32_size = -1, u32_off = -1, u32_valid = 0;

            if (data[p] == 0 && p + 1 + 2 + 4 <= (int)len) {
                u16_size = (int)U16BE(data + p + 1);
                u16_off  = (int)U32BE(data + p + 3);
                if (content_base + u16_off + u16_size <= (int)len &&
                    u16_off >= prev_offset)
                    u16_valid = 1;
            }
            if (p >= 1 && data[p - 1] == 0 && p + 3 + 4 <= (int)len) {
                u32_size = (int)U32BE(data + p - 1);
                u32_off  = (int)U32BE(data + p + 3);
                if (u32_size > 0xFFFF &&
                    content_base + u32_off + u32_size <= (int)len &&
                    u32_off >= prev_offset)
                    u32_valid = 1;
            }

            if (u32_valid && !u16_valid) {
                size_bytes = 4;
                size_val   = u32_size;
                p -= 1;                          /* consume prev name terminator */
            } else if (u16_valid) {
                size_bytes = 2;
                size_val   = u16_size;
                p += 1;                          /* consume pad byte */
            } else {
                return -1;
            }
        }

        offset_p   = p + size_bytes;
        name_len_p = offset_p + 4;
        if (name_len_p + 2 > (int)len) return -1;
        offset_val = U32BE(data + offset_p);
        name_chars = (int)U16LE(data + name_len_p);
        if (name_chars < 1 || name_chars > 200) return -1;
        name_off = name_len_p + 2;
        name_byte_len = name_chars * 2;
        name_ptr = data + name_off;

        /* Last entry quirk: trailing 0x00 high byte elided when the
           name's last char ends right before content_base. */
        if (name_off + name_byte_len > content_base &&
            name_off < content_base) {
            int truncated = content_base - name_off;
            synthetic_name = (unsigned char*)ModsArenaAlloc(arena, truncated + 1);
            if (!synthetic_name) return -1;
            memcpy(synthetic_name, data + name_off, truncated);
            synthetic_name[truncated] = 0;
            name_ptr = synthetic_name;
            name_byte_len = truncated + 1;
        }

        if ((int)offset_val <= prev_offset && entry_idx > 0) return -1;
        if (content_base + (int)offset_val + size_val > (int)len) return -1;

        content_ptr = data + content_base + offset_val;

        if (reserve(x, arena, x->count + 1) < 0) return -1;
        x->entries[x->count].name_le    = name_ptr;
        x->entries[x->count].name_chars = name_byte_len / 2;
        x->entries[x->count].data       = content_ptr;
        x->entries[x->count].data_len   = size_val;
        x->count++;

        prev_offset = (int)offset_val;
        entry_idx++;
        p = name_off + name_chars * 2;
    }

    if (x->count != entry_count) return -1;
    return 0;
}

static int size_encoding_bytes(int entry_idx, int size) {
    if (entry_idx == 0) return 4;
    if (entry_idx == 1) return 3;
    if (size <= 0xFFFF) return 2;
    return 4;
}

/* True if the entry's name's last UTF-16LE code unit's high byte (the
   actual final byte of name bytes when stored) is 0. ASCII chars always
   have a 0 high byte; we assume ASCII for all entries. */
static int name_ends_with_zero(const ModsXuizEntry* e) {
    if (e->name_chars <= 0) return 1;
    return e->name_le[e->name_chars * 2 - 1] == 0;
}

int ModsXuizEncode(ModsArena* arena, const ModsXuiz* x,
                   unsigned char** out, size_t* out_len) {
    int entry_table_len = 0;
    int content_base, blob_ptr_field;
    int i, total_size;
    unsigned char* buf;
    int pos, content_pos;
    unsigned cur_offset;

    if (x->count == 0) return -1;

    /* Phase 1: compute entry table length. */
    for (i = 0; i < x->count; i++) {
        int sz_bytes = size_encoding_bytes(i, x->entries[i].data_len);
        int name_byte_len = x->entries[i].name_chars * 2;
        int is_last = (i == x->count - 1);
        int entry_len;
        if (is_last && name_ends_with_zero(&x->entries[i]))
            name_byte_len -= 1;
        entry_len = 4 /*offset*/ + 2 /*name_len*/ + name_byte_len;
        if (i >= 2 && sz_bytes == 4)
            entry_len += 3;      /* u32 with overlap → only 3 bytes contributed */
        else if (i >= 2 && sz_bytes == 2)
            entry_len += 1 + 2;  /* pad + u16 */
        else
            entry_len += sz_bytes;
        entry_table_len += entry_len;
    }

    content_base   = 0x16 + entry_table_len;
    blob_ptr_field = entry_table_len;

    total_size = content_base;
    for (i = 0; i < x->count; i++) total_size += x->entries[i].data_len;

    buf = (unsigned char*)ModsArenaAlloc(arena, total_size);
    if (!buf) return -1;

    /* Container header */
    buf[0] = 'X'; buf[1] = 'U'; buf[2] = 'I'; buf[3] = 'Z';
    put_u32be(buf + 4, 1);
    put_u32be(buf + 8, (unsigned)total_size);
    buf[0x0C] = 0; buf[0x0D] = 0; buf[0x0E] = 0; buf[0x0F] = 0;
    put_u32be(buf + 0x10, (unsigned)blob_ptr_field);
    put_u16be(buf + 0x14, (unsigned)x->count);

    /* Phase 2: emit entry headers with computed offsets. */
    pos = 0x16;
    cur_offset = 0;
    for (i = 0; i < x->count; i++) {
        int sz_bytes = size_encoding_bytes(i, x->entries[i].data_len);
        int is_last = (i == x->count - 1);
        int name_byte_len = x->entries[i].name_chars * 2;
        int prev_term_zero = (i == 0) ? 1 : name_ends_with_zero(&x->entries[i - 1]);

        if (is_last && name_ends_with_zero(&x->entries[i]))
            name_byte_len -= 1;

        if (i >= 2 && sz_bytes == 4) {
            if (!prev_term_zero) return -1;
            /* Overlap: low 3 bytes of size. Caller wrote the high byte
               as the previous name's terminator into buf[pos - 1] = 0
               (which is exactly what the previous name's last byte
               already is, since we asserted prev_term_zero). */
            buf[pos]     = (unsigned char)((x->entries[i].data_len >> 16) & 0xFF);
            buf[pos + 1] = (unsigned char)((x->entries[i].data_len >>  8) & 0xFF);
            buf[pos + 2] = (unsigned char)( x->entries[i].data_len        & 0xFF);
            pos += 3;
        } else if (i >= 2 && sz_bytes == 2) {
            buf[pos++] = 0;                        /* pad */
            put_u16be(buf + pos, (unsigned)x->entries[i].data_len);
            pos += 2;
        } else if (sz_bytes == 4) {
            put_u32be(buf + pos, (unsigned)x->entries[i].data_len);
            pos += 4;
        } else if (sz_bytes == 3) {
            buf[pos]     = (unsigned char)((x->entries[i].data_len >> 16) & 0xFF);
            buf[pos + 1] = (unsigned char)((x->entries[i].data_len >>  8) & 0xFF);
            buf[pos + 2] = (unsigned char)( x->entries[i].data_len        & 0xFF);
            pos += 3;
        } else if (sz_bytes == 2) {
            put_u16be(buf + pos, (unsigned)x->entries[i].data_len);
            pos += 2;
        }

        put_u32be(buf + pos, cur_offset);
        pos += 4;
        put_u16le(buf + pos, (unsigned)x->entries[i].name_chars);
        pos += 2;
        if (name_byte_len > 0)
            memcpy(buf + pos, x->entries[i].name_le, name_byte_len);
        pos += name_byte_len;

        cur_offset += x->entries[i].data_len;
    }

    if (pos != content_base) return -1;

    /* Phase 3: emit content blob */
    content_pos = content_base;
    for (i = 0; i < x->count; i++) {
        if (x->entries[i].data_len > 0)
            memcpy(buf + content_pos, x->entries[i].data, x->entries[i].data_len);
        content_pos += x->entries[i].data_len;
    }

    *out = buf;
    *out_len = total_size;
    return 0;
}

int ModsXuizFind(const ModsXuiz* x, const char* ascii_name) {
    int n = (int)strlen(ascii_name);
    int i, j;
    for (i = 0; i < x->count; i++) {
        if (x->entries[i].name_chars != n) continue;
        for (j = 0; j < n; j++) {
            if (x->entries[i].name_le[j * 2]     != (unsigned char)ascii_name[j] ||
                x->entries[i].name_le[j * 2 + 1] != 0)
                break;
        }
        if (j == n) return i;
    }
    return -1;
}

int ModsXuizAppend(ModsXuiz* x, ModsArena* arena,
                   const unsigned char* name_le, int name_chars,
                   const unsigned char* data, int data_len) {
    if (reserve(x, arena, x->count + 1) < 0) return -1;
    x->entries[x->count].name_le    = name_le;
    x->entries[x->count].name_chars = name_chars;
    x->entries[x->count].data       = data;
    x->entries[x->count].data_len   = data_len;
    x->count++;
    return 0;
}

int ModsXuizReplaceData(ModsXuiz* x, int idx,
                        const unsigned char* data, int data_len) {
    if (idx < 0 || idx >= x->count) return -1;
    x->entries[idx].data     = data;
    x->entries[idx].data_len = data_len;
    return 0;
}

int ModsXuizRemove(ModsXuiz* x, int idx) {
    int i;
    if (idx < 0 || idx >= x->count) return -1;
    for (i = idx; i < x->count - 1; i++) x->entries[i] = x->entries[i + 1];
    x->count--;
    return 0;
}

int ModsXuizAsciiToLe(ModsArena* arena, const char* ascii,
                      const unsigned char** out_le, int* out_chars) {
    int n = (int)strlen(ascii);
    int i;
    unsigned char* buf;
    for (i = 0; i < n; i++) {
        if ((unsigned char)ascii[i] >= 0x80) return -1;
    }
    buf = (unsigned char*)ModsArenaAlloc(arena, n * 2);
    if (!buf && n) return -1;
    for (i = 0; i < n; i++) {
        buf[i * 2]     = (unsigned char)ascii[i];
        buf[i * 2 + 1] = 0;
    }
    *out_le    = buf;
    *out_chars = n;
    return 0;
}
