/* ce_mp4_defrag.h: fragmented-MP4 -> plain-MP4 de-fragmenter for the Zune.
 * See ce_mp4_defrag.cpp. Lossless AAC remux so ZDKMedia_Queue_PlaySongFromFile
 * can play YouTube's fragmented itag-140 audio. */
#ifndef ZB_CE_MP4_DEFRAG_H
#define ZB_CE_MP4_DEFRAG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CE_MP4_OK = 0,
    CE_MP4_ERR_IO,      /* read/write/open failure */
    CE_MP4_ERR_NOMEM,
    CE_MP4_ERR_PARSE    /* not a recognizable fragmented MP4 */
};

/* Whole-file de-fragment: read the fragmented MP4 at in_path, write a plain MP4
 * to out_path. Thin wrapper over the incremental builder below. */
int ce_mp4_defrag_file(const wchar_t *in_path, const wchar_t *out_path);

/* ── incremental builder (progressive streaming) ───────────────────────────
 *
 * The progressive pipeline can't hold the whole fragmented file: it Range-fetches
 * the small moof boxes first to synthesize the moov (which must be complete and
 * final when PlaySongFromFile opens the file), then streams the large mdat
 * payloads into the growing file while playback runs. This API splits the
 * de-fragmenter's moov synthesis from the mdat concatenation:
 *
 *   ctx = ce_mp4_defrag_begin(init, init_len);   // ftyp + init-moov bytes
 *   for each fragment:
 *       ce_mp4_defrag_feed_moof(ctx, moof, len); // per-sample sizes/durations
 *       ce_mp4_defrag_add_mdat(ctx, payload_len);// size of this fragment's audio
 *   ce_mp4_defrag_finish(ctx, &prefix, &prefix_len, &total_mdat);
 *   // write prefix, then append each fragment's mdat payload (total_mdat bytes)
 *   ce_mp4_defrag_free(ctx);
 *
 * The prefix is ftyp + moov + an 8-byte mdat header claiming total_mdat payload
 * bytes; the caller appends exactly those payload bytes, in fragment order. */
typedef struct ce_mp4_defrag_ctx ce_mp4_defrag_ctx;

/* init = the front of the fragmented file: ftyp + init-moov (a trailing sidx, if
 * present, is ignored). Returns NULL on parse/alloc failure. */
ce_mp4_defrag_ctx *ce_mp4_defrag_begin(const unsigned char *init, size_t init_len);

/* Feed one complete moof box. Returns CE_MP4_OK or an error code. */
int ce_mp4_defrag_feed_moof(ce_mp4_defrag_ctx *c, const unsigned char *moof, size_t moof_len);

/* Record the AAC payload size of the fragment whose moof was just fed. */
void ce_mp4_defrag_add_mdat(ce_mp4_defrag_ctx *c, unsigned long mdat_payload_len);

/* Produce the ftyp+moov+mdat-header prefix (malloc'd into *out_prefix; caller
 * frees with free()). *out_total_mdat is the claimed mdat payload size. */
int ce_mp4_defrag_finish(ce_mp4_defrag_ctx *c, unsigned char **out_prefix,
                         size_t *out_prefix_len, unsigned long *out_total_mdat);

void ce_mp4_defrag_free(ce_mp4_defrag_ctx *c);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CE_MP4_DEFRAG_H */
