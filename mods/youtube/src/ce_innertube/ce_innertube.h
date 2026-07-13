/* ce_innertube.h: YouTube internal (Innertube) JSON API client for the Zune HD.
 *
 * A small reusable unit shared by the player (videoId -> audio stream URL) and
 * the music front-end (query -> track list). Built on the standalone ce_https
 * transport (ce-common): each call is one blocking HTTPS request whose JSON body
 * is built here and whose response is extracted here.
 *
 * No OAuth: public playback/search use a baked-in API key per client. Responses
 * are scanned with forward strstr (no general JSON parser) because every field
 * this client needs has a stable literal anchor in the response.
 *
 * Clients are chosen per endpoint from the device-validated 8-client matrix:
 *   /player  -> ANDROID_VR  (only client returning a DIRECT itag url, no cipher)
 *   /search  -> WEB_REMIX   (the music client at music.youtube.com)
 */
#ifndef CE_INNERTUBE_H
#define CE_INNERTUBE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ce_innertube_result {
    CE_IT_OK = 0,
    CE_IT_ERR_ARG,        /* missing/invalid argument */
    CE_IT_ERR_HTTP,       /* ce_https transport failed (see ce_https_result) */
    CE_IT_ERR_STATUS,     /* HTTP status was not 200 */
    CE_IT_ERR_PARSE,      /* a required field was absent from the response */
    CE_IT_ERR_NOMEM
};

/* Resolve a videoId to its itag-140 (AAC-LC, fragmented MP4) direct audio URL via
 * the ANDROID_VR /player endpoint. `url_out` receives a NUL-terminated googlevideo
 * URL (JSON \uXXXX / \/ unescaped); `url_cap` is its capacity (>= 2048 advised). */
enum ce_innertube_result ce_innertube_audio_url(const char *video_id,
                                                char *url_out, size_t url_cap);

/* Search category: maps to a WEB_REMIX search filter `params`. Songs/videos
 * carry a playable videoId; albums/artists/playlists carry a browseId (drill-in). */
enum ce_innertube_category {
    CE_IT_CAT_SONGS     = 0,
    CE_IT_CAT_ALBUMS    = 1,
    CE_IT_CAT_ARTISTS   = 2,
    CE_IT_CAT_PLAYLISTS = 3
};

/* One music search result. All strings NUL-terminated; storage is the caller's
 * array element. `artist` may be empty when the response omits it. `id` is the
 * videoId when `is_video`, else a browseId (album/artist/playlist drill-in). */
struct ce_innertube_track {
    char id[64];       /* videoId (11) / album MPRE (~17) / artist UC (24) / playlist VLPL… (~36-40) */
    int  is_video;
    char title[256];
    char artist[256];
    char album[256];   /* song rows: the subtitle's MUSIC_PAGE_TYPE_ALBUM run */
    int  duration_ms;  /* song rows: the subtitle's "M:SS" run (0 if absent) */
    char thumb[256];   /* largest thumbnail URL from the row (empty if absent) */
};

/* Search YouTube Music (WEB_REMIX) within `category`. Fills up to `cap` results
 * into `tracks`; *count_out receives the number filled.
 *
 * `continuation_in` NULL/"" fetches the first page; a token from a prior call's
 * `continuation_out` fetches the next page (the query/category are ignored then).
 * `continuation_out` (cap `cont_cap`, >= 2048 advised) receives the token for the
 * page after this one, or "" when the results are exhausted. */
enum ce_innertube_result ce_innertube_search(const char *query, int category,
                                             const char *continuation_in,
                                             struct ce_innertube_track *tracks,
                                             int cap, int *count_out,
                                             char *continuation_out, size_t cont_cap);

/* Browse the tracks of a drill-in target (WEB_REMIX /browse). `browse_id` is a
 * browseId from a search/browse row: an album (MPRE…) or playlist (VL…) returns
 * its track list; an artist (UC…) returns its "Top songs" shelf. Every track row
 * is a musicResponsiveListItemRenderer carrying a playable videoId, so the result
 * is filled with song rows (`is_video`=1), the same shape ce_innertube_search
 * produces for the songs category. Continuation paging matches ce_innertube_search:
 * NULL/"" fetches the first page; a prior `continuation_out` token fetches the next
 * (browse_id is then ignored). */
enum ce_innertube_result ce_innertube_browse(const char *browse_id,
                                             const char *continuation_in,
                                             struct ce_innertube_track *tracks,
                                             int cap, int *count_out,
                                             char *continuation_out, size_t cont_cap);

/* Browse an artist channel (UC… browseId) for one of its two drill tabs. A single
 * /browse response carries both, so the raw body is cached for the artist: the first
 * call (either tab) fetches it, the second is served from cache (no second request).
 *   want_albums != 0 → the artist's albums + singles (musicTwoRowItemRenderer cards
 *                      with MPRE browseIds; `is_video`=0, drill into each via browse).
 *   want_albums == 0 → the artist's "Top songs" (musicResponsiveListItemRenderer;
 *                      `is_video`=1, playable). */
enum ce_innertube_result ce_innertube_artist(const char *browse_id, int want_albums,
                                             struct ce_innertube_track *tracks,
                                             int cap, int *count_out);

const char *ce_innertube_result_str(enum ce_innertube_result r);

#ifdef __cplusplus
}
#endif

#endif /* CE_INNERTUBE_H */
