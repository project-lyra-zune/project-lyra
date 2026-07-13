/* yt_search_ipc.h: shared-section IPC between youtube.dll (gemstone UI) and
 * ytsearchd.exe (the out-of-process search daemon). Mirrors the zune-cast
 * daemon↔UI pattern (named CreateFileMappingW section + named wake events):
 *
 *   UI → daemon:  write query into the section, bump req_seq, SetEvent(WAKE).
 *   daemon → UI:  fill rows + count, set done_seq=req_seq, ping the UI notify.
 *
 * CE6 has no OpenFileMapping, so both sides CreateFileMappingW by the same name to
 * attach to the one section. Names carry a layout version so a stale section from
 * an older layout can't be mapped. Single producer per direction; the seq fields
 * are the change signal (UI bumps req_seq, daemon echoes it into done_seq). */
#ifndef YT_SEARCH_IPC_H
#define YT_SEARCH_IPC_H

#define YT_SEARCH_SECTION_NAME  L"zune-yt-search-v6"
#define YT_SEARCH_WAKE_EVENT    L"zune-yt-search-wake-v1"   /* UI SetEvents → daemon wakes */
#define YT_SEARCH_DONE_EVENT    L"zune-yt-search-done-v1"   /* daemon SetEvents → UI wakes (also pinged via notify) */

/* Row thumbnail cache: ytsearchd writes <id>.jpg here, the UI reads the same path.
 * Under the mod's own dir so it is scoped to the install and cleared on update
 * (regenerable, so not declared persistent in the manifest). */
#define YT_ART_DIR              L"\\flash2\\automation\\mods\\youtube\\yt-art"

#define YT_SEARCH_MAX_ROWS   24
#define YT_SEARCH_QUERY_LEN  128   /* wchar units */
#define YT_SEARCH_TITLE_LEN  120   /* wchar units */
#define YT_SEARCH_ARTIST_LEN 96    /* wchar units */
#define YT_SEARCH_ID_LEN     64    /* ASCII bytes: videoId (11) / album MPRE / artist UC / playlist VLPL… (~36-40) + NUL */
#define YT_SEARCH_CONT_LEN   3072  /* ASCII bytes: continuation token (in/out) */

typedef struct {
    char    id[YT_SEARCH_ID_LEN];          /* ASCII videoId (is_video) or browseId */
    long    is_video;                      /* 1 = playable videoId; 0 = browseId drill-in */
    wchar_t title[YT_SEARCH_TITLE_LEN];
    wchar_t artist[YT_SEARCH_ARTIST_LEN];
    wchar_t album[YT_SEARCH_ARTIST_LEN];   /* song rows only (from the subtitle) */
    long    duration_ms;                   /* song rows only (parsed from the subtitle) */
} YtSearchRow;

typedef struct {
    unsigned long version;                 /* layout version */
    volatile long req_seq;                 /* bumped by the UI on each request */
    volatile long done_seq;                /* set by the daemon = the req_seq it just answered */
    long          category;                /* requested category (0..3); echoed back */
    long          mode;                    /* 0=new search (replace), 1=continuation (append), 2=browse */
    long          status;                  /* 0=ok, else ce_innertube_result code */
    long          count;                   /* rows filled this page (0..YT_SEARCH_MAX_ROWS) */
    wchar_t       query[YT_SEARCH_QUERY_LEN];
    char          browse_id[YT_SEARCH_ID_LEN];      /* mode 2: the album/playlist/artist browseId to drill into */
    char          continuation[YT_SEARCH_CONT_LEN]; /* in: token for mode 1; out: next-page token */
    YtSearchRow   rows[YT_SEARCH_MAX_ROWS];
} YtSearchBlock;

#define YT_SEARCH_VERSION  6u

#endif /* YT_SEARCH_IPC_H */
