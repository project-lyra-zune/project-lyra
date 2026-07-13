#include "mods_xur_builder.h"
#include "mods_endian.h"

#include <string.h>
#include <stdio.h>

typedef struct {
    ModsArena* arena;
    unsigned char* p;
    int len;
    int cap;
} XurBuf;

typedef struct {
    const char* ascii;
    const wchar_t* wide;
} XurStr;

static int xb_need(XurBuf* b, int add) {
    if (b->len + add <= b->cap) return 0;
    {
        int new_cap = b->cap ? b->cap * 2 : 256;
        unsigned char* np;
        while (new_cap < b->len + add) new_cap *= 2;
        np = (unsigned char*)ModsArenaAlloc(b->arena, new_cap);
        if (!np) return -1;
        if (b->len) memcpy(np, b->p, b->len);
        b->p = np;
        b->cap = new_cap;
    }
    return 0;
}

static int xb_u8(XurBuf* b, unsigned int v) {
    if (xb_need(b, 1) != 0) return -1;
    b->p[b->len++] = (unsigned char)(v & 0xff);
    return 0;
}

static int xb_bytes(XurBuf* b, const unsigned char* p, int n) {
    if (n < 0 || xb_need(b, n) != 0) return -1;
    if (n) memcpy(b->p + b->len, p, n);
    b->len += n;
    return 0;
}

static int xb_be16(XurBuf* b, unsigned int v) {
    if (xb_need(b, 2) != 0) return -1;
    put_u16be(b->p + b->len, v);
    b->len += 2;
    return 0;
}

static int xb_be32(XurBuf* b, unsigned long v) {
    if (xb_need(b, 4) != 0) return -1;
    put_u32be(b->p + b->len, (unsigned)v);
    b->len += 4;
    return 0;
}

static int xb_magic(XurBuf* b, const char* s) {
    return xb_bytes(b, (const unsigned char*)s, 4);
}

static int ascii_wlen(const char* s) {
    int n = 0;
    if (!s) return -1;
    while (s[n]) {
        if ((unsigned char)s[n] >= 0x80) return -1;
        n++;
    }
    return n;
}

static int wide_wlen_ascii(const wchar_t* s) {
    int n = 0;
    if (!s) return -1;
    while (s[n]) {
        if (s[n] > 0x7f) return -1;
        n++;
    }
    return n;
}

static int str_wlen(const XurStr* s) {
    if (s->ascii) return ascii_wlen(s->ascii);
    return wide_wlen_ascii(s->wide);
}

static int xb_utf16be_str(XurBuf* b, const XurStr* s) {
    int i;
    if (s->ascii) {
        for (i = 0; s->ascii[i]; i++) {
            if ((unsigned char)s->ascii[i] >= 0x80) return -1;
            if (xb_be16(b, (unsigned char)s->ascii[i]) != 0) return -1;
        }
        return 0;
    }
    for (i = 0; s->wide[i]; i++) {
        if (s->wide[i] > 0x7f) return -1;
        if (xb_be16(b, (unsigned int)s->wide[i]) != 0) return -1;
    }
    return 0;
}

static int build_strn(ModsArena* arena, const XurStr* strings, int count,
                      XurBuf* out) {
    int i;
    out->arena = arena; out->p = NULL; out->len = 0; out->cap = 0;
    for (i = 0; i < count; i++) {
        int n = str_wlen(&strings[i]);
        if (n <= 0 || n > 0x7fff) return -1;
        if (xb_be16(out, n) != 0) return -1;
        if (xb_utf16be_str(out, &strings[i]) != 0) return -1;
    }
    return 0;
}

static int write_object_header(XurBuf* b, int class_idx, int flags) {
    return xb_be16(b, class_idx) || xb_u8(b, flags) ? -1 : 0;
}

static int write_width_height_props(XurBuf* b) {
    if (xb_be16(b, 2) != 0) return -1;      /* properties count */
    if (xb_u8(b, 0x0c) != 0) return -1;     /* XuiElement: 2 props, 4 masks */
    if (xb_u8(b, 0x00) || xb_u8(b, 0x00) || xb_u8(b, 0x00) ||
        xb_u8(b, 0x06)) return -1;          /* Width, Height */
    if (xb_be32(b, 0x41800000ul) || xb_be32(b, 0x41800000ul)) return -1;
    if (xb_u8(b, 0x00) != 0) return -1;     /* XuiCanvas leaf descriptor (no own props) */
    return 0;
}

static int write_scene_props(XurBuf* b, int id_idx) {
    if (xb_be16(b, 3) != 0) return -1;
    if (xb_u8(b, 0x14) != 0) return -1;
    if (xb_u8(b, 0x00) || xb_u8(b, 0x00) || xb_u8(b, 0x00) ||
        xb_u8(b, 0x07)) return -1;
    if (xb_be16(b, id_idx) || xb_be32(b, 0x41800000ul) ||
        xb_be32(b, 0x41800000ul)) return -1;
    if (xb_u8(b, 0x00) || xb_u8(b, 0x00)) return -1; /* XuiControl, XuiScene */
    return 0;
}

static int write_icon_control_props(XurBuf* b, int id_idx, int class_idx) {
    if (xb_be16(b, 7) != 0) return -1;
    if (xb_u8(b, 0x2c) != 0) return -1;
    if (xb_u8(b, 0x00) || xb_u8(b, 0x40) || xb_u8(b, 0x30) ||
        xb_u8(b, 0x07)) return -1;
    if (xb_be16(b, id_idx) || xb_be32(b, 0x41800000ul) ||
        xb_be32(b, 0x41800000ul)) return -1;
    if (xb_u8(b, 0x00) || xb_u8(b, 0x01) || xb_be32(b, 3)) return -1;
    if (xb_u8(b, 0x02) || xb_u8(b, 0x00) || xb_u8(b, 0x01) ||
        xb_be16(b, class_idx)) return -1;
    return 0;
}

static int write_frame_control_props(XurBuf* b, int id_idx, int visual_idx) {
    if (xb_be16(b, 7) != 0) return -1;
    if (xb_u8(b, 0x2c) != 0) return -1;
    if (xb_u8(b, 0x00) || xb_u8(b, 0x00) || xb_u8(b, 0x30) ||
        xb_u8(b, 0x87)) return -1;
    if (xb_be16(b, id_idx) || xb_be32(b, 0x41800000ul) ||
        xb_be32(b, 0x41800000ul)) return -1;
    if (xb_u8(b, 0x00) || xb_u8(b, 0x01) || xb_u8(b, 0x00)) return -1; /* Hittable=false, DisableFocus=true, Show=false */
    if (xb_u8(b, 0x02) || xb_u8(b, 0x00) || xb_u8(b, 0x02) ||
        xb_be16(b, visual_idx)) return -1;
    return 0;
}

static int build_fragment_data(XurBuf* out, const int* frame_id_idx,
                               const int* frame_vis_idx, int nframes) {
    int k;
    out->len = 0;
    if (write_object_header(out, 1, 0x03) != 0) return -1;
    if (write_width_height_props(out) != 0) return -1;
    if (xb_be32(out, 1) != 0) return -1;                 /* root child count */

    if (write_object_header(out, 2, 0x03) != 0) return -1;
    if (write_scene_props(out, 3) != 0) return -1;
    if (xb_be32(out, 1) != 0) return -1;

    if (write_object_header(out, 4, 0x03) != 0) return -1;
    if (write_icon_control_props(out, 5, 6) != 0) return -1;
    if (xb_be32(out, (unsigned long)nframes) != 0) return -1;   /* icon child count = N frames */

    for (k = 0; k < nframes; k++) {
        if (write_object_header(out, 4, 0x01) != 0) return -1;
        if (write_frame_control_props(out, frame_id_idx[k], frame_vis_idx[k]) != 0) return -1;
    }
    return 0;
}

static int write_visual_props(XurBuf* b, int id_idx) {
    if (xb_be16(b, 3) != 0) return -1;
    if (xb_u8(b, 0x14) != 0) return -1;
    if (xb_u8(b, 0x00) || xb_u8(b, 0x00) || xb_u8(b, 0x00) ||
        xb_u8(b, 0x07)) return -1;
    if (xb_be16(b, id_idx) || xb_be32(b, 0x41800000ul) ||
        xb_be32(b, 0x41800000ul)) return -1;
    if (xb_u8(b, 0x00) != 0) return -1; /* XuiVisual */
    return 0;
}

static int write_image_props(XurBuf* b, int id_idx, int image_idx) {
    if (xb_be16(b, 7) != 0) return -1;
    if (xb_u8(b, 0x24) != 0) return -1;
    if (xb_u8(b, 0x00) || xb_u8(b, 0x00) || xb_u8(b, 0x02) ||
        xb_u8(b, 0x0f)) return -1;
    if (xb_be16(b, id_idx) || xb_be32(b, 0x41800000ul) ||
        xb_be32(b, 0x41800000ul)) return -1;
    if (xb_be32(b, 0) || xb_be32(b, 15)) return -1; /* Position vector idx, Anchor */
    if (xb_u8(b, 0x09) || xb_u8(b, 0x05)) return -1; /* XuiImage: SizeMode, ImagePath */
    if (xb_be32(b, 4) || xb_be16(b, image_idx)) return -1;
    return 0;
}

static int build_visual_data(ModsArena* arena, XurBuf* out) {
    (void)arena;
    out->len = 0;
    if (write_object_header(out, 1, 0x03) != 0) return -1;
    if (write_width_height_props(out) != 0) return -1;
    if (xb_be32(out, 1) != 0) return -1;

    if (write_object_header(out, 2, 0x03) != 0) return -1;
    if (write_visual_props(out, 3) != 0) return -1;
    if (xb_be32(out, 1) != 0) return -1;

    if (write_object_header(out, 4, 0x01) != 0) return -1;
    if (write_image_props(out, 5, 6) != 0) return -1;
    return 0;
}

static int build_vect_zero(ModsArena* arena, XurBuf* out) {
    out->arena = arena; out->p = NULL; out->len = 0; out->cap = 0;
    return xb_be32(out, 0) || xb_be32(out, 0) || xb_be32(out, 0) ? -1 : 0;
}

static int wrap_xur(ModsArena* arena,
                    const XurBuf* strn,
                    const XurBuf* vect,
                    const XurBuf* data,
                    const unsigned char** out,
                    int* out_len) {
    XurBuf b;
    int sections = vect ? 3 : 2;
    int table_len = sections * 12;
    int off = 20 + table_len;
    int file_size = off + strn->len + (vect ? vect->len : 0) + data->len;
    b.arena = arena; b.p = NULL; b.len = 0; b.cap = 0;
    if (xb_magic(&b, "XUIB") || xb_be32(&b, 5) || xb_be32(&b, 0) ||
        xb_be16(&b, 0x000c) || xb_be32(&b, (unsigned long)file_size) ||
        xb_be16(&b, (unsigned int)sections)) return -1;
    if (xb_magic(&b, "STRN") || xb_be32(&b, (unsigned long)off) ||
        xb_be32(&b, (unsigned long)strn->len)) return -1;
    off += strn->len;
    if (vect) {
        if (xb_magic(&b, "VECT") || xb_be32(&b, (unsigned long)off) ||
            xb_be32(&b, (unsigned long)vect->len)) return -1;
        off += vect->len;
    }
    if (xb_magic(&b, "DATA") || xb_be32(&b, (unsigned long)off) ||
        xb_be32(&b, (unsigned long)data->len)) return -1;
    if (xb_bytes(&b, strn->p, strn->len) != 0) return -1;
    if (vect && xb_bytes(&b, vect->p, vect->len) != 0) return -1;
    if (xb_bytes(&b, data->p, data->len) != 0) return -1;
    if (b.len != file_size) return -1;
    *out = b.p;
    *out_len = b.len;
    return 0;
}

int ModsBuildStatusIconFragmentXur(ModsArena* arena,
                                   const char* slot,
                                   const char* const* visual_ids,
                                   int nstates,
                                   const unsigned char** out,
                                   int* out_len) {
    char   modicon[96];
    char   frame_names[MOD_ICON_MAX_FRAMES][16];
    XurStr strings[6 + 2 * MOD_ICON_MAX_FRAMES];
    int    frame_id_idx[MOD_ICON_MAX_FRAMES];
    int    frame_vis_idx[MOD_ICON_MAX_FRAMES];
    XurBuf strn, data;
    int    nstr = 6, nframes = 0, k;
    if (!arena || !slot || !visual_ids || !out || !out_len) return -1;
    if (nstates < 1 || nstates > MOD_ICON_MAX_FRAMES) return -1;
    _snprintf(modicon, sizeof(modicon) - 1, "modicon_%s", slot);
    modicon[sizeof(modicon) - 1] = 0;
    strings[0].ascii = "XuiCanvas";     strings[0].wide = NULL;
    strings[1].ascii = "XuiScene";      strings[1].wide = NULL;
    strings[2].ascii = "scene";         strings[2].wide = NULL;
    strings[3].ascii = "XuiControl";    strings[3].wide = NULL;
    strings[4].ascii = modicon;         strings[4].wide = NULL;
    strings[5].ascii = "ModStatusIcon"; strings[5].wide = NULL;
    /* One frame element per non-NULL state, named "frame<k>" so the icon host
       shows frame<state>. NULL states (e.g. "off") get no element → hidden. */
    for (k = 0; k < nstates; k++) {
        if (!visual_ids[k]) continue;
        _snprintf(frame_names[nframes], sizeof(frame_names[0]) - 1, "frame%d", k);
        frame_names[nframes][sizeof(frame_names[0]) - 1] = 0;
        strings[nstr].ascii = frame_names[nframes]; strings[nstr].wide = NULL;
        frame_id_idx[nframes] = nstr + 1;           /* STRN indices are 1-based */
        nstr++;
        strings[nstr].ascii = visual_ids[k];        strings[nstr].wide = NULL;
        frame_vis_idx[nframes] = nstr + 1;
        nstr++;
        nframes++;
    }
    if (nframes < 1) return -1;   /* need at least one visible state */
    if (build_strn(arena, strings, nstr, &strn) != 0) return -1;
    data.arena = arena; data.p = NULL; data.len = 0; data.cap = 0;
    if (build_fragment_data(&data, frame_id_idx, frame_vis_idx, nframes) != 0) return -1;
    return wrap_xur(arena, &strn, NULL, &data, out, out_len);
}

int ModsBuildStatusIconVisualXur(ModsArena* arena,
                                 const char* visual_id,
                                 const wchar_t* image_url,
                                 const unsigned char** out,
                                 int* out_len) {
    XurStr strings[6];
    XurBuf strn, vect, data;
    if (!arena || !visual_id || !image_url || !out || !out_len) return -1;
    strings[0].ascii = "XuiCanvas"; strings[0].wide = NULL;
    strings[1].ascii = "XuiVisual"; strings[1].wide = NULL;
    strings[2].ascii = visual_id;   strings[2].wide = NULL;
    strings[3].ascii = "XuiImage";  strings[3].wide = NULL;
    strings[4].ascii = "icon";      strings[4].wide = NULL;
    strings[5].ascii = NULL;        strings[5].wide = image_url;
    if (build_strn(arena, strings, 6, &strn) != 0) return -1;
    if (build_vect_zero(arena, &vect) != 0) return -1;
    data.arena = arena; data.p = NULL; data.len = 0; data.cap = 0;
    if (build_visual_data(arena, &data) != 0) return -1;
    return wrap_xur(arena, &strn, &vect, &data, out, out_len);
}
