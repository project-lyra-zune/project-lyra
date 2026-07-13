/* ce_mp4_defrag.cpp: fragmented-MP4 (DASH audio) -> plain MP4 de-fragmenter.
 *
 * YouTube serves audio (itag 140 AAC-LC) as fragmented MP4 (moof/mdat), which the
 * Zune HD's media parser cannot read. This rewrites it losslessly into a plain
 * MP4 (single moov+mdat) the Zune plays via ZDKMedia_Queue_PlaySongFromFile:
 * concatenate the moof/trun AAC samples, synthesize a normal stbl (sizes from the
 * truns, uniform durations), copy the stsd (AAC esds) verbatim from the init moov.
 * The AAC bitstream is untouched; no transcode.
 *
 * The moov synthesis is exposed incrementally (begin/feed_moof/add_mdat/finish)
 * so the progressive pipeline can build the moov from Range-fetched moof boxes
 * before the mdat payloads have streamed; ce_mp4_defrag_file is the whole-file
 * wrapper. Port of the validated host prototype tools/ytmusic_defrag.py. */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "ce_mp4_defrag.h"

/* ── big-endian readers ────────────────────────────────────────────────── */
static unsigned rd32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

/* Iterate boxes in [s,e): returns next box (type4cc at *typ, [bs,be)) or 0 at end. */
static int next_box(const unsigned char *d, size_t s, size_t e,
                    size_t *bs, size_t *be, const char **typ) {
    if (s + 8 > e) return 0;
    unsigned sz = rd32(d + s);
    if (sz < 8 || s + sz > e) return 0;
    *bs = s; *be = s + sz; *typ = (const char *)(d + s + 4);
    return 1;
}

/* Find first child box of type `fourcc` within container payload [cs+8, ce). */
static int child(const unsigned char *d, size_t cs, size_t ce, const char *fourcc,
                 size_t *bs, size_t *be) {
    size_t s = cs + 8, xs, xe; const char *t;
    while (next_box(d, s, ce, &xs, &xe, &t)) {
        if (memcmp(t, fourcc, 4) == 0) { *bs = xs; *be = xe; return 1; }
        s = xe;
    }
    return 0;
}

/* ── growable output buffer + box backpatching ─────────────────────────── */
typedef struct { unsigned char *p; size_t len, cap; } Buf;

static int bput(Buf *b, const void *d, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = (b->len + n) * 2 + 4096;
        unsigned char *np = (unsigned char *)realloc(b->p, nc);
        if (!np) return 0;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, d, n); b->len += n;
    return 1;
}
static int bu32(Buf *b, unsigned v) { unsigned char t[4] = {(unsigned char)(v>>24),(unsigned char)(v>>16),(unsigned char)(v>>8),(unsigned char)v}; return bput(b, t, 4); }
static size_t bopen(Buf *b, const char *fourcc) { size_t at = b->len; bu32(b, 0); bput(b, fourcc, 4); return at; }
static void bclose(Buf *b, size_t at) { unsigned sz = (unsigned)(b->len - at); b->p[at]=(unsigned char)(sz>>24); b->p[at+1]=(unsigned char)(sz>>16); b->p[at+2]=(unsigned char)(sz>>8); b->p[at+3]=(unsigned char)sz; }
static size_t bfull(Buf *b, const char *fourcc, unsigned verflags) { size_t at = bopen(b, fourcc); bu32(b, verflags); return at; }

/* ── incremental builder ───────────────────────────────────────────────── */

struct ce_mp4_defrag_ctx {
    unsigned char *init;        /* copy of ftyp + init-moov bytes */
    size_t         init_len;
    /* box offsets within `init` */
    size_t ftyp_s, ftyp_e, moov_s, moov_e;
    size_t mvhd_s, mvhd_e, trak_s, trak_e, tkhd_s, tkhd_e;
    size_t mdia_s, mdia_e, mdhd_s, mdhd_e, hdlr_s, hdlr_e;
    size_t minf_s, minf_e, stbl_s, stbl_e, stsd_s, stsd_e;
    size_t smhd_s, smhd_e, dinf_s, dinf_e;
    unsigned timescale, movie_ts;
    /* per-sample table accumulated from the fed moofs */
    unsigned *sizes;
    size_t    n, cap;
    unsigned long long total_dur;
    unsigned long      total_mdat;
};

ce_mp4_defrag_ctx *ce_mp4_defrag_begin(const unsigned char *init, size_t init_len) {
    ce_mp4_defrag_ctx *c = (ce_mp4_defrag_ctx *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->init = (unsigned char *)malloc(init_len ? init_len : 1);
    if (!c->init) { free(c); return NULL; }
    memcpy(c->init, init, init_len);
    c->init_len = init_len;
    {
        const unsigned char *d = c->init;
        size_t s = 0, bs, be; const char *t;
        int have_ftyp = 0, have_moov = 0;
        while (next_box(d, s, init_len, &bs, &be, &t)) {
            if (!memcmp(t, "ftyp", 4)) { c->ftyp_s = bs; c->ftyp_e = be; have_ftyp = 1; }
            else if (!memcmp(t, "moov", 4)) { c->moov_s = bs; c->moov_e = be; have_moov = 1; }
            s = be;
        }
        if (!have_ftyp || !have_moov) { ce_mp4_defrag_free(c); return NULL; }

        if (!child(d, c->moov_s, c->moov_e, "mvhd", &c->mvhd_s, &c->mvhd_e) ||
            !child(d, c->moov_s, c->moov_e, "trak", &c->trak_s, &c->trak_e) ||
            !child(d, c->trak_s, c->trak_e, "tkhd", &c->tkhd_s, &c->tkhd_e) ||
            !child(d, c->trak_s, c->trak_e, "mdia", &c->mdia_s, &c->mdia_e) ||
            !child(d, c->mdia_s, c->mdia_e, "mdhd", &c->mdhd_s, &c->mdhd_e) ||
            !child(d, c->mdia_s, c->mdia_e, "hdlr", &c->hdlr_s, &c->hdlr_e) ||
            !child(d, c->mdia_s, c->mdia_e, "minf", &c->minf_s, &c->minf_e) ||
            !child(d, c->minf_s, c->minf_e, "stbl", &c->stbl_s, &c->stbl_e) ||
            !child(d, c->stbl_s, c->stbl_e, "stsd", &c->stsd_s, &c->stsd_e)) {
            ce_mp4_defrag_free(c); return NULL;
        }
        if (!child(d, c->minf_s, c->minf_e, "smhd", &c->smhd_s, &c->smhd_e)) { c->smhd_s = c->smhd_e = 0; }
        if (!child(d, c->minf_s, c->minf_e, "dinf", &c->dinf_s, &c->dinf_e)) { c->dinf_s = c->dinf_e = 0; }

        c->timescale = (d[c->mdhd_s+8]==0) ? rd32(d+c->mdhd_s+8+12) : rd32(d+c->mdhd_s+8+20);
        c->movie_ts  = (d[c->mvhd_s+8]==0) ? rd32(d+c->mvhd_s+8+12) : 1000;
        if (c->timescale == 0) c->timescale = 44100;
    }
    return c;
}

int ce_mp4_defrag_feed_moof(ce_mp4_defrag_ctx *c, const unsigned char *moof, size_t moof_len) {
    size_t traf_s, traf_e, tfhd_s, tfhd_e;
    unsigned tf, def_dur = 0, def_size = 0;
    size_t p, ts, xs, xe; const char *tt;
    if (!c || !moof || moof_len < 8) return CE_MP4_ERR_PARSE;
    if (!child(moof, 0, moof_len, "traf", &traf_s, &traf_e) ||
        !child(moof, traf_s, traf_e, "tfhd", &tfhd_s, &tfhd_e)) {
        return CE_MP4_ERR_PARSE;
    }
    tf = rd32(moof + tfhd_s + 8) & 0xffffff;
    p = tfhd_s + 12 + 4;                       /* skip ver/flags + track_ID */
    if (tf & 0x000001) p += 8;
    if (tf & 0x000002) p += 4;
    if (tf & 0x000008) { def_dur = rd32(moof + p); p += 4; }
    if (tf & 0x000010) { def_size = rd32(moof + p); p += 4; }

    ts = traf_s + 8;
    while (next_box(moof, ts, traf_e, &xs, &xe, &tt)) {
        if (!memcmp(tt, "trun", 4)) {
            unsigned fl = rd32(moof + xs + 8) & 0xffffff;
            unsigned cnt = rd32(moof + xs + 12);
            size_t q = xs + 16;
            unsigned i;
            if (fl & 0x000001) q += 4;          /* data-offset */
            if (fl & 0x000004) q += 4;          /* first-sample-flags */
            for (i = 0; i < cnt; i++) {
                unsigned sd = def_dur, ss = def_size;
                if (fl & 0x000100) { sd = rd32(moof + q); q += 4; }
                if (fl & 0x000200) { ss = rd32(moof + q); q += 4; }
                if (fl & 0x000400) q += 4;
                if (fl & 0x000800) q += 4;
                if (c->n == c->cap) {
                    size_t ncap = c->cap ? c->cap * 2 : 4096;
                    unsigned *ns = (unsigned *)realloc(c->sizes, ncap * sizeof(unsigned));
                    if (!ns) return CE_MP4_ERR_NOMEM;
                    c->sizes = ns; c->cap = ncap;
                }
                c->sizes[c->n++] = ss;
                c->total_dur += (sd ? sd : 1024);
            }
        }
        ts = xe;
    }
    return CE_MP4_OK;
}

void ce_mp4_defrag_add_mdat(ce_mp4_defrag_ctx *c, unsigned long mdat_payload_len) {
    if (c) c->total_mdat += mdat_payload_len;
}

int ce_mp4_defrag_finish(ce_mp4_defrag_ctx *c, unsigned char **out_prefix,
                         size_t *out_prefix_len, unsigned long *out_total_mdat) {
    const unsigned char *d;
    Buf out; size_t moov;
    unsigned seg_movie;
    if (!c || c->n == 0) return CE_MP4_ERR_PARSE;
    d = c->init;
    memset(&out, 0, sizeof(out));

    if (!bput(&out, d + c->ftyp_s, c->ftyp_e - c->ftyp_s)) { free(out.p); return CE_MP4_ERR_NOMEM; }
    seg_movie = (unsigned)((c->total_dur * c->movie_ts) / c->timescale);

    moov = bopen(&out, "moov");
    {
        size_t at = out.len; bput(&out, d + c->mvhd_s, c->mvhd_e - c->mvhd_s);
        if (d[c->mvhd_s+8]==0) { unsigned v=seg_movie; out.p[at+8+16]=(unsigned char)(v>>24); out.p[at+8+17]=(unsigned char)(v>>16); out.p[at+8+18]=(unsigned char)(v>>8); out.p[at+8+19]=(unsigned char)v; }

        size_t trak = bopen(&out, "trak");
        {
            size_t a2 = out.len; bput(&out, d + c->tkhd_s, c->tkhd_e - c->tkhd_s);
            if (d[c->tkhd_s+8]==0) { unsigned v=seg_movie; out.p[a2+8+20]=(unsigned char)(v>>24); out.p[a2+8+21]=(unsigned char)(v>>16); out.p[a2+8+22]=(unsigned char)(v>>8); out.p[a2+8+23]=(unsigned char)v; }

            size_t edts = bopen(&out, "edts");
            { size_t elst = bfull(&out, "elst", 0); bu32(&out, 1); bu32(&out, seg_movie); bu32(&out, 0); bu32(&out, 0x00010000); bclose(&out, elst); }
            bclose(&out, edts);

            size_t mdia = bopen(&out, "mdia");
            {
                size_t a3 = out.len; bput(&out, d + c->mdhd_s, c->mdhd_e - c->mdhd_s);
                if (d[c->mdhd_s+8]==0) { unsigned v=(unsigned)c->total_dur; out.p[a3+8+16]=(unsigned char)(v>>24); out.p[a3+8+17]=(unsigned char)(v>>16); out.p[a3+8+18]=(unsigned char)(v>>8); out.p[a3+8+19]=(unsigned char)v; }
                bput(&out, d + c->hdlr_s, c->hdlr_e - c->hdlr_s);

                size_t minf = bopen(&out, "minf");
                {
                    if (c->smhd_s) bput(&out, d + c->smhd_s, c->smhd_e - c->smhd_s);
                    if (c->dinf_s) bput(&out, d + c->dinf_s, c->dinf_e - c->dinf_s);

                    size_t stbl = bopen(&out, "stbl");
                    {
                        size_t i, x;
                        bput(&out, d + c->stsd_s, c->stsd_e - c->stsd_s);    /* stsd: AAC esds */
                        x = bfull(&out, "stts", 0); bu32(&out, 1); bu32(&out, (unsigned)c->n); bu32(&out, (unsigned)(c->total_dur/c->n)); bclose(&out, x);
                        x = bfull(&out, "stsc", 0); bu32(&out, 1); bu32(&out, 1); bu32(&out, (unsigned)c->n); bu32(&out, 1); bclose(&out, x);
                        x = bfull(&out, "stsz", 0); bu32(&out, 0); bu32(&out, (unsigned)c->n);
                        for (i = 0; i < c->n; i++) bu32(&out, c->sizes[i]);
                        bclose(&out, x);
                        x = bfull(&out, "stco", 0); bu32(&out, 1); bu32(&out, 0); bclose(&out, x);
                    }
                    bclose(&out, stbl);
                }
                bclose(&out, minf);
            }
            bclose(&out, mdia);
        }
        bclose(&out, trak);
    }
    bclose(&out, moov);

    /* chunk offset = ftyp + moov + mdat header(8); patch the stco value */
    {
        unsigned chunk_off = (unsigned)(out.len + 8);
        out.p[out.len-4]=(unsigned char)(chunk_off>>24); out.p[out.len-3]=(unsigned char)(chunk_off>>16);
        out.p[out.len-2]=(unsigned char)(chunk_off>>8); out.p[out.len-1]=(unsigned char)chunk_off;
    }

    /* mdat header only; the caller appends the payload bytes */
    if (!bu32(&out, (unsigned)(8 + c->total_mdat)) || !bput(&out, "mdat", 4)) { free(out.p); return CE_MP4_ERR_NOMEM; }

    if (out_prefix)     *out_prefix = out.p;
    if (out_prefix_len) *out_prefix_len = out.len;
    if (out_total_mdat) *out_total_mdat = c->total_mdat;
    return CE_MP4_OK;
}

void ce_mp4_defrag_free(ce_mp4_defrag_ctx *c) {
    if (!c) return;
    free(c->init);
    free(c->sizes);
    free(c);
}

/* ── whole-file wrapper over the incremental builder ───────────────────── */

int ce_mp4_defrag_file(const wchar_t *in_path, const wchar_t *out_path) {
    int rv = CE_MP4_ERR_IO;
    unsigned char *d = NULL; DWORD isize = 0;
    ce_mp4_defrag_ctx *ctx = NULL;
    unsigned char *prefix = NULL; size_t prefix_len = 0; unsigned long total_mdat = 0;
    size_t first_moof = 0;

    {
        HANDLE h = CreateFileW(in_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        DWORD got = 0;
        if (h == INVALID_HANDLE_VALUE) return CE_MP4_ERR_IO;
        isize = GetFileSize(h, NULL);
        d = (unsigned char *)malloc(isize ? isize : 1);
        if (!d) { CloseHandle(h); return CE_MP4_ERR_NOMEM; }
        if (!ReadFile(h, d, isize, &got, NULL) || got != isize) { CloseHandle(h); free(d); return CE_MP4_ERR_IO; }
        CloseHandle(h);
    }

    /* init segment = everything up to the first moof */
    {
        size_t s = 0, bs, be; const char *t;
        while (next_box(d, s, isize, &bs, &be, &t)) {
            if (!memcmp(t, "moof", 4)) { first_moof = bs; break; }
            s = be;
        }
        if (first_moof == 0) { free(d); return CE_MP4_ERR_PARSE; }
    }

    ctx = ce_mp4_defrag_begin(d, first_moof);
    if (!ctx) { free(d); return CE_MP4_ERR_PARSE; }

    /* feed every moof + its following mdat payload size */
    {
        size_t s = 0, bs, be; const char *t;
        while (next_box(d, s, isize, &bs, &be, &t)) {
            if (!memcmp(t, "moof", 4)) {
                int fr = ce_mp4_defrag_feed_moof(ctx, d + bs, be - bs);
                if (fr != CE_MP4_OK) { rv = fr; goto done; }
            } else if (!memcmp(t, "mdat", 4)) {
                ce_mp4_defrag_add_mdat(ctx, (unsigned long)((be - bs) - 8));
            }
            s = be;
        }
    }

    rv = ce_mp4_defrag_finish(ctx, &prefix, &prefix_len, &total_mdat);
    if (rv != CE_MP4_OK) goto done;

    {
        HANDLE h = CreateFileW(out_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD wrote = 0;
        if (h == INVALID_HANDLE_VALUE) { rv = CE_MP4_ERR_IO; goto done; }
        if (!WriteFile(h, prefix, (DWORD)prefix_len, &wrote, NULL) || wrote != prefix_len) { CloseHandle(h); rv = CE_MP4_ERR_IO; goto done; }
        /* append each fragment's mdat payload, in file order */
        {
            size_t s = 0, bs, be; const char *t;
            while (next_box(d, s, isize, &bs, &be, &t)) {
                if (!memcmp(t, "mdat", 4)) {
                    DWORD w = 0; DWORD plen = (DWORD)((be - bs) - 8);
                    if (!WriteFile(h, d + bs + 8, plen, &w, NULL) || w != plen) { CloseHandle(h); rv = CE_MP4_ERR_IO; goto done; }
                }
                s = be;
            }
        }
        CloseHandle(h);
    }
    rv = CE_MP4_OK;

done:
    free(d);
    free(prefix);
    ce_mp4_defrag_free(ctx);
    return rv;
}
