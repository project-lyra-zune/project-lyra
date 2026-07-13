/* ce_innertube.c: see ce_innertube.h */

#include "ce_innertube.h"
#include "ce_https.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── client identities (device-validated 8-client matrix) ─────────────────────
 * ANDROID_VR is the only /player client returning a DIRECT itag url with no
 * signatureCipher; WEB/MWEB/TVHTML5 hit poToken, ANDROID/IOS hit attestation.
 * It still bot-gates (LOGIN_REQUIRED) without a session visitorData; see g_visitor. */
#define VR_KEY      "AIzaSyA8eiZmM1FaDVjRy-df2KTyQ_vz_yYM39w"
#define VR_HDRS \
    "User-Agent: com.google.android.apps.youtube.vr.oculus/1.60.19 " \
    "(Linux; U; Android 12; Quest 3) gzip\r\n" \
    "X-YouTube-Client-Name: 28\r\nX-YouTube-Client-Version: 1.60.19"

/* WEB_REMIX = the music.youtube.com client; clientName/version travel in the body. */
#define REMIX_KEY   "AIzaSyC9XL3ZjWddXya6X74dJoCTL-WEYFDNX30"
#define REMIX_HDRS \
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:88.0) " \
    "Gecko/20100101 Firefox/88.0\r\n" \
    "Origin: https://music.youtube.com\r\nReferer: https://music.youtube.com/"

/* WEB_REMIX /search filter `params` per category; restricts results to one tab's
 * type so every musicResponsiveListItemRenderer is of that kind. The byte after
 * "EgWKAQI" selects the type: I=songs, Y=albums, g=artists, o=playlists. */
static const char *category_params(int cat){
    switch(cat){
        case CE_IT_CAT_ALBUMS:    return "EgWKAQIYAWoMEA4QChADEAQQCRAF";
        case CE_IT_CAT_ARTISTS:   return "EgWKAQIgAWoMEA4QChADEAQQCRAF";
        case CE_IT_CAT_PLAYLISTS: return "EgWKAQIoAWoMEA4QChADEAQQCRAF";
        default:                  return "EgWKAQIIAWoMEA4QChADEAQQCRAF"; /* songs */
    }
}

/* itag-140 audio-only is fragmented MP4; the front-end never picks a video itag. */
#define ITAG_AUDIO  140

static int hexd(char c){
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

/* Copy a JSON string value starting at the opening byte after the quote, stopping
 * at the unescaped closing quote, decoding \uXXXX (low byte) and \/ along the way.
 * Returns the number of bytes written (NUL-terminated), 0 on empty/overflow. */
static size_t json_str_copy(const char *src, char *out, size_t cap){
    const char *h = src; size_t o = 0;
    if(!cap) return 0;
    while(*h && *h!='"' && o < cap-1){
        if(h[0]=='\\' && h[1]=='u' &&
           hexd(h[2])>=0 && hexd(h[3])>=0 && hexd(h[4])>=0 && hexd(h[5])>=0){
            int code=(hexd(h[2])<<12)|(hexd(h[3])<<8)|(hexd(h[4])<<4)|hexd(h[5]);
            out[o++]=(char)(code&0x7f); h+=6;
        } else if(h[0]=='\\' && h[1]=='/'){ out[o++]='/'; h+=2; }
        else if(h[0]=='\\' && h[1]){ out[o++]=h[1]; h+=2; }
        else out[o++]=*h++;
    }
    out[o]=0;
    return o;
}

/* Extract the itag-140 direct url from a /player streamingData response. */
static int extract_audio_url(const char *body, char *out, size_t cap){
    const char *p,*u,*h;
    p=strstr(body,"\"itag\": 140"); if(!p) p=strstr(body,"\"itag\":140"); if(!p) return 0;
    u=strstr(p,"\"url\""); if(!u) return 0;
    h=strstr(u,"https"); if(!h) return 0;
    return json_str_copy(h, out, cap) > 0;
}

const char *ce_innertube_result_str(enum ce_innertube_result r){
    switch(r){
        case CE_IT_OK:         return "ok";
        case CE_IT_ERR_ARG:    return "bad argument";
        case CE_IT_ERR_HTTP:   return "transport failed";
        case CE_IT_ERR_STATUS: return "non-200 status";
        case CE_IT_ERR_PARSE:  return "field absent";
        case CE_IT_ERR_NOMEM:  return "out of memory";
    }
    return "unknown";
}

/* Session visitorData, cached for the process. Without it ANDROID_VR /player
 * bot-gates many videoIds with playabilityStatus=LOGIN_REQUIRED (no streamingData);
 * sending it in the client context clears the gate. A gated response still carries
 * responseContext.visitorData, so the first resolve self-bootstraps: try → harvest →
 * retry once with it; later resolves send the cached value on the first call. */
static char g_visitor[640] = {0};   /* visitorData is ~520 bytes (URL-encoded) */

static void harvest_visitor(const char *body){
    const char *p, *c, *q;
    if(g_visitor[0]) return;
    p=strstr(body,"\"visitorData\""); if(!p) return;
    c=strchr(p+13,':'); if(!c) return;
    q=strchr(c,'"'); if(!q) return;          /* opening quote of the value */
    json_str_copy(q+1, g_visitor, sizeof(g_visitor));
}

static void build_player_body(char *body, size_t cap, const char *video_id){
    if(g_visitor[0])
        _snprintf(body,cap,
            "{\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\","
            "\"clientVersion\":\"1.60.19\",\"androidSdkVersion\":32,"
            "\"deviceModel\":\"Quest 3\",\"hl\":\"en\",\"visitorData\":\"%s\"}},"
            "\"videoId\":\"%s\"}", g_visitor, video_id);
    else
        _snprintf(body,cap,
            "{\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\","
            "\"clientVersion\":\"1.60.19\",\"androidSdkVersion\":32,"
            "\"deviceModel\":\"Quest 3\",\"hl\":\"en\"}},\"videoId\":\"%s\"}",
            video_id);
}

static enum ce_innertube_result do_player(const char *video_id,
                                          char *url_out, size_t url_cap){
    char body[1024];   /* must hold the ~520-byte visitorData in the context */
    struct ce_https_response resp;
    enum ce_https_result hr;
    enum ce_innertube_result rc = CE_IT_ERR_PARSE;

    build_player_body(body, sizeof(body), video_id);
    hr=ce_https_request("youtubei.googleapis.com",
                        "/youtubei/v1/player?key=" VR_KEY, "POST",
                        VR_HDRS, body, strlen(body), "application/json", &resp);
    if(hr!=CE_HTTPS_OK) return CE_IT_ERR_HTTP;
    if(resp.status!=200){ ce_https_response_free(&resp); return CE_IT_ERR_STATUS; }

    harvest_visitor(resp.body);
    if(extract_audio_url(resp.body, url_out, url_cap)) rc = CE_IT_OK;
    ce_https_response_free(&resp);
    return rc;
}

enum ce_innertube_result ce_innertube_audio_url(const char *video_id,
                                                char *url_out, size_t url_cap){
    enum ce_innertube_result rc;
    int had_visitor;
    if(!video_id || !*video_id || !url_out || url_cap < 64) return CE_IT_ERR_ARG;

    had_visitor = (g_visitor[0] != 0);
    rc = do_player(video_id, url_out, url_cap);
    /* A bot-gated first attempt (no visitorData) returns no streamingData
     * (CE_IT_ERR_PARSE) but harvested a visitorData; retry once with it. */
    if(rc == CE_IT_ERR_PARSE && !had_visitor && g_visitor[0])
        rc = do_player(video_id, url_out, url_cap);
    return rc;
}

/* Each song row is one musicResponsiveListItemRenderer; within it the title is the
 * first flexColumns[0] text run and the play-button watchEndpoint videoId is the
 * first "videoId". Anchors assume compact JSON, forced via prettyPrint=false.
 * NOT yet device-validated; the front-end phase runs the first live query. */
#define ROW_ANCHOR   "\"musicResponsiveListItemRenderer\""
#define RUNS_ANCHOR  "\"runs\":[{\"text\":\""
#define RUN_ANCHOR   "{\"text\":\""
#define VID_ANCHOR   "\"videoId\":\""
#define BID_ANCHOR   "\"browseId\":\""

/* The subtitle (flexColumns[1]) run that holds the artist differs by type: songs
 * and playlists put the wanted name first (artist / playlist creator); albums lead
 * with the type ("Album • Artist • Year") so the artist is run index 2 (past the
 * "Album"/"Single"/"EP" type and the " • " separator). */
static int subtitle_artist_run(int category){
    return (category == CE_IT_CAT_ALBUMS) ? 2 : 0;
}

/* Each row carries musicThumbnailRenderer.thumbnail.thumbnails[], ordered
 * small→large. Copy the last (largest) url before the array closes so the 128px
 * row art is sourced from the highest-res available. */
#define THUMBS_ANCHOR  "\"thumbnails\":[{\"url\":\""
static void extract_largest_thumb(const char *row, const char *row_end,
                                  char *out, size_t cap){
    const char *arr, *arr_end, *u, *last = 0;
    out[0] = 0;
    arr = strstr(row, THUMBS_ANCHOR);
    if(!arr || arr >= row_end) return;
    arr_end = strchr(arr, ']'); if(!arr_end || arr_end > row_end) arr_end = row_end;
    u = arr + strlen("\"thumbnails\":[");
    for(;;){
        const char *f = strstr(u, "\"url\":\"");
        if(!f || f >= arr_end) break;
        last = f + strlen("\"url\":\"");
        u = last;
    }
    if(last) json_str_copy(last, out, cap);

    /* googleusercontent (album/artist/playlist art) takes free-form size params
     * after '='; pin them to a 128px JPEG; the list art box is 128x128, so this
     * is the smallest no-upscale size, and forcing JPEG (-rj) keeps the bytes
     * imaging.dll-decodable (defends against a -rw WebP variant). ytimg song art is
     * already a .jpg path with no params; leave it. */
    if(out[0] && strstr(out, "googleusercontent.com")){
        char *eq = strchr(out, '=');
        size_t base = eq ? (size_t)(eq - out) : strlen(out);
        if(base + 18 < cap){
            memcpy(out + base, "=w128-h128-l90-rj", 18); /* includes NUL */
        }
    }
}

/* Pull the next-page continuation token from a search/continuation response.
 * Newer responses carry continuationCommand.token; older nextContinuationData. */
static void extract_continuation(const char *body, char *out, size_t cap){
    const char *c, *t;
    if(!out || !cap) return;
    out[0]=0;
    c=strstr(body,"\"continuationCommand\"");
    if(c){ t=strstr(c,"\"token\":\""); if(t){ json_str_copy(t+9, out, cap); if(out[0]) return; } }
    c=strstr(body,"\"nextContinuationData\"");
    if(c){ t=strstr(c,"\"continuation\":\""); if(t) json_str_copy(t+16, out, cap); }
}

/* "M:SS" or "H:MM:SS" -> milliseconds. */
static int parse_length_ms(const char *s){
    int parts[3], n=0, v=0, any=0, secs=0;
    parts[0]=parts[1]=parts[2]=0;
    for(; *s && *s!='"'; s++){
        if(*s>='0'&&*s<='9'){ v=v*10+(*s-'0'); any=1; }
        else if(*s==':'){ if(n<2) parts[n++]=v; v=0; }
        else break;
    }
    if(n<3) parts[n]=v;
    if(n==2) secs=parts[0]*3600+parts[1]*60+parts[2];
    else if(n==1) secs=parts[0]*60+parts[1];
    else secs=parts[0];
    return any ? secs*1000 : 0;
}

/* Within [start,end): the run text whose run carries `pagetype` is the last
 * "text":" before that pageType literal (text precedes navigationEndpoint per run). */
static void byline_name_for_pagetype(const char *start, const char *end,
                                     const char *pagetype, char *out, size_t cap){
    const char *pt, *p, *f, *last=0;
    if(!out || !cap) return;
    out[0]=0;
    pt=strstr(start, pagetype); if(!pt || pt>=end) return;
    p=start;
    while((f=strstr(p,"\"text\":\""))!=0 && f<pt){ last=f; p=f+8; }
    if(last) json_str_copy(last+8, out, cap);   /* skip the 8-char "text":" prefix */
}

/* Within [start,end): the first run whose value is a bare "M:SS" timestamp. */
static int subtitle_duration_ms(const char *start, const char *end){
    const char *p=start, *f;
    while((f=strstr(p,"\"text\":\""))!=0 && f<end){
        const char *v=f+8, *c=v; int colon=0, ok=1;
        if(*v>='0'&&*v<='9'){
            for(; *c && *c!='"'; c++){
                if(*c==':') colon=1;
                else if(*c<'0'||*c>'9'){ ok=0; break; }
            }
            if(ok && colon && *c=='"') return parse_length_ms(v);
        }
        p=f+8;
    }
    return 0;
}

/* Parse musicResponsiveListItemRenderer rows out of a /search or /browse response
 * body into `tracks` (up to `cap`); returns the count. Shared by search and browse:
 * a search response's rows are the category's results, a browse response's rows are
 * the drill-in target's tracks; both are the same renderer with the same anchors.
 * `category` selects the browseId prefix (MPRE/VL/UC) for non-video rows and the
 * subtitle artist run; browse passes CE_IT_CAT_SONGS so every row keeps its videoId. */
static int parse_track_rows(const char *body, size_t body_len, int category,
                            struct ce_innertube_track *tracks, int cap){
    const char *p = body, *end = body + body_len;
    int n = 0;
    while(n<cap){
        const char *row=strstr(p,ROW_ANCHOR), *row_end, *t, *v, *b, *a;
        if(!row) break;
        p=row+1;
        row_end=strstr(p,ROW_ANCHOR); if(!row_end) row_end=end;

        t=strstr(row,RUNS_ANCHOR); if(!t || t>=row_end) continue;

        /* id: a playable videoId for songs/videos, else a browseId to drill in. A row
         * holds several browseIds (the entity's own + its artist's UC channel); pick the
         * one whose prefix matches this category (album=MPRE, playlist=VL, artist=UC),
         * so we don't grab the shared artist channel (which collides across an artist's
         * albums). Fall back to the first browseId if no prefix matches. */
        v=strstr(row,VID_ANCHOR);
        if(v && v<row_end){
            tracks[n].is_video=1;
            json_str_copy(v+strlen(VID_ANCHOR), tracks[n].id, sizeof(tracks[n].id));
        } else {
            const char *want = (category==CE_IT_CAT_ALBUMS)    ? "MPRE" :
                               (category==CE_IT_CAT_PLAYLISTS) ? "VL"   :
                               (category==CE_IT_CAT_ARTISTS)   ? "UC"   : (const char*)0;
            const char *scan = row, *chosen = 0;
            while(want){
                const char *val;
                b = strstr(scan, BID_ANCHOR); if(!b || b>=row_end) break;
                val = b + strlen(BID_ANCHOR);
                if(strncmp(val, want, strlen(want))==0){ chosen = val; break; }
                scan = val;
            }
            if(!chosen){
                b=strstr(row,BID_ANCHOR); if(!b || b>=row_end) continue;
                chosen = b + strlen(BID_ANCHOR);
            }
            tracks[n].is_video=0;
            json_str_copy(chosen, tracks[n].id, sizeof(tracks[n].id));
        }

        json_str_copy(t+strlen(RUNS_ANCHOR), tracks[n].title, sizeof(tracks[n].title));
        tracks[n].artist[0]=0;
        a=strstr(t+strlen(RUNS_ANCHOR),RUNS_ANCHOR);   /* flexColumns[1] runs = subtitle */
        if(a && a<row_end){
            const char *r=a+strlen(RUNS_ANCHOR); int ri=subtitle_artist_run(category), ok=1, k;
            for(k=0;k<ri;k++){
                const char *nx=strstr(r,RUN_ANCHOR);
                if(!nx || nx>=row_end){ ok=0; break; }
                r=nx+strlen(RUN_ANCHOR);
            }
            if(ok) json_str_copy(r,tracks[n].artist,sizeof(tracks[n].artist));
        }

        /* song subtitle is "Artist • Album • Duration" with pageType tags; pull the
         * album + duration for now-playing straight from the row (no /next needed). */
        tracks[n].album[0]=0; tracks[n].duration_ms=0;
        if(a && a<row_end){
            byline_name_for_pagetype(a, row_end, "MUSIC_PAGE_TYPE_ALBUM",
                                     tracks[n].album, sizeof(tracks[n].album));
            tracks[n].duration_ms = subtitle_duration_ms(a, row_end);
        }

        extract_largest_thumb(row, row_end, tracks[n].thumb, sizeof(tracks[n].thumb));

        if(tracks[n].id[0]) n++;
    }
    return n;
}

/* Parse musicTwoRowItemRenderer cards (the artist page's carousels) into `tracks`,
 * keeping only album/single cards, those whose card browseId is an MPRE. Video
 * (videoId), playlist (VL) and related-artist (UC) cards are skipped, so the result
 * is the artist's discography as browseId drill-in rows (is_video=0). The card's
 * first browseId is its title's browseEndpoint (the album); title/subtitle/thumb
 * come from the card's own runs. */
#define CARD_ANCHOR    "\"musicTwoRowItemRenderer\""
#define CARD_TITLE     "\"title\":{\"runs\":[{\"text\":\""
#define CARD_SUBTITLE  "\"subtitle\":{\"runs\":[{\"text\":\""
static int parse_card_rows(const char *body, size_t body_len,
                           struct ce_innertube_track *tracks, int cap){
    const char *p=body, *end=body+body_len;
    int n=0;
    while(n<cap){
        const char *card=strstr(p,CARD_ANCHOR), *card_end, *t, *b, *s;
        if(!card) break;
        p=card+1;
        card_end=strstr(p,CARD_ANCHOR); if(!card_end) card_end=end;

        b=strstr(card,BID_ANCHOR);
        if(!b || b>=card_end) continue;
        b += strlen(BID_ANCHOR);
        if(strncmp(b,"MPRE",4)!=0) continue;        /* keep albums/singles only */
        tracks[n].is_video=0;
        json_str_copy(b, tracks[n].id, sizeof(tracks[n].id));

        tracks[n].title[0]=0;
        t=strstr(card,CARD_TITLE);
        if(t && t<card_end) json_str_copy(t+strlen(CARD_TITLE), tracks[n].title, sizeof(tracks[n].title));

        tracks[n].artist[0]=0;                        /* subtitle (e.g. "Album • 2025") */
        s=strstr(card,CARD_SUBTITLE);
        if(s && s<card_end) json_str_copy(s+strlen(CARD_SUBTITLE), tracks[n].artist, sizeof(tracks[n].artist));

        tracks[n].album[0]=0; tracks[n].duration_ms=0;
        extract_largest_thumb(card, card_end, tracks[n].thumb, sizeof(tracks[n].thumb));

        if(tracks[n].id[0]) n++;
    }
    return n;
}

enum ce_innertube_result ce_innertube_search(const char *query, int category,
                                             const char *continuation_in,
                                             struct ce_innertube_track *tracks,
                                             int cap, int *count_out,
                                             char *continuation_out, size_t cont_cap){
    char body[4096], qesc[256];
    struct ce_https_response resp;
    enum ce_https_result hr;
    int n=0, is_cont = (continuation_in && *continuation_in);

    if(count_out) *count_out=0;
    if(continuation_out && cont_cap) continuation_out[0]=0;
    if(!tracks || cap<=0) return CE_IT_ERR_ARG;
    if(!is_cont && (!query || !*query)) return CE_IT_ERR_ARG;

    if(is_cont){
        /* next page: the continuation token replaces query/params (it encodes both). */
        _snprintf(body,sizeof(body),
            "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
            "\"clientVersion\":\"1.20250310.01.00\"}},"
            "\"continuation\":\"%s\"}",
            continuation_in);
    } else {
        const char *s=query; size_t o=0;
        while(*s && o<sizeof(qesc)-2){
            if(*s=='"'||*s=='\\'){ qesc[o++]='\\'; qesc[o++]=*s++; }
            else if((unsigned char)*s < 0x20){ s++; }   /* drop control chars */
            else qesc[o++]=*s++;
        }
        qesc[o]=0;
        _snprintf(body,sizeof(body),
            "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
            "\"clientVersion\":\"1.20250310.01.00\"}},"
            "\"query\":\"%s\",\"params\":\"%s\"}",
            qesc, category_params(category));
    }

    hr=ce_https_request("music.youtube.com",
                        "/youtubei/v1/search?prettyPrint=false&key=" REMIX_KEY,
                        "POST", REMIX_HDRS, body, strlen(body),
                        "application/json", &resp);
    if(hr!=CE_HTTPS_OK) return CE_IT_ERR_HTTP;
    if(resp.status!=200){ ce_https_response_free(&resp); return CE_IT_ERR_STATUS; }

    n = parse_track_rows(resp.body, resp.body_len, category, tracks, cap);

    extract_continuation(resp.body, continuation_out, cont_cap);
    ce_https_response_free(&resp);
    if(count_out) *count_out=n;
    return CE_IT_OK;
}

/* Browse a drill-in target's track list. The /browse endpoint takes a browseId (or
 * a continuation token), and its musicResponsiveListItemRenderer rows are parsed the
 * same way the songs search tab is; every row is a playable track. See the header. */
enum ce_innertube_result ce_innertube_browse(const char *browse_id,
                                             const char *continuation_in,
                                             struct ce_innertube_track *tracks,
                                             int cap, int *count_out,
                                             char *continuation_out, size_t cont_cap){
    char body[4096];
    struct ce_https_response resp;
    enum ce_https_result hr;
    int n=0, is_cont = (continuation_in && *continuation_in);

    if(count_out) *count_out=0;
    if(continuation_out && cont_cap) continuation_out[0]=0;
    if(!tracks || cap<=0) return CE_IT_ERR_ARG;
    if(!is_cont && (!browse_id || !*browse_id)) return CE_IT_ERR_ARG;

    if(is_cont)
        _snprintf(body,sizeof(body),
            "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
            "\"clientVersion\":\"1.20250310.01.00\"}},\"continuation\":\"%s\"}",
            continuation_in);
    else
        _snprintf(body,sizeof(body),
            "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
            "\"clientVersion\":\"1.20250310.01.00\"}},\"browseId\":\"%s\"}",
            browse_id);

    hr=ce_https_request("music.youtube.com",
                        "/youtubei/v1/browse?prettyPrint=false&key=" REMIX_KEY,
                        "POST", REMIX_HDRS, body, strlen(body),
                        "application/json", &resp);
    if(hr!=CE_HTTPS_OK) return CE_IT_ERR_HTTP;
    if(resp.status!=200){ ce_https_response_free(&resp); return CE_IT_ERR_STATUS; }

    n = parse_track_rows(resp.body, resp.body_len, CE_IT_CAT_SONGS, tracks, cap);

    extract_continuation(resp.body, continuation_out, cont_cap);
    ce_https_response_free(&resp);
    if(count_out) *count_out=n;
    return CE_IT_OK;
}

/* The current artist's raw /browse body, cached so the Albums and Songs tabs share
 * one network fetch (the page carries both). Keyed by browseId; replaced when a
 * different artist is opened. */
static char  *g_artist_body = NULL;
static size_t g_artist_body_len = 0;
static char   g_artist_id[64] = {0};

enum ce_innertube_result ce_innertube_artist(const char *browse_id, int want_albums,
                                             struct ce_innertube_track *tracks,
                                             int cap, int *count_out){
    int n;
    if(count_out) *count_out=0;
    if(!tracks || cap<=0 || !browse_id || !*browse_id) return CE_IT_ERR_ARG;

    if(g_artist_body==NULL || strcmp(g_artist_id, browse_id)!=0){
        char body[4096];
        struct ce_https_response resp;
        enum ce_https_result hr;
        char *copy; size_t blen;
        _snprintf(body,sizeof(body),
            "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
            "\"clientVersion\":\"1.20250310.01.00\"}},\"browseId\":\"%s\"}", browse_id);
        hr=ce_https_request("music.youtube.com",
                            "/youtubei/v1/browse?prettyPrint=false&key=" REMIX_KEY,
                            "POST", REMIX_HDRS, body, strlen(body), "application/json", &resp);
        if(hr!=CE_HTTPS_OK) return CE_IT_ERR_HTTP;
        if(resp.status!=200){ ce_https_response_free(&resp); return CE_IT_ERR_STATUS; }
        blen=resp.body_len;
        copy=(char*)malloc(blen+1);
        if(!copy){ ce_https_response_free(&resp); return CE_IT_ERR_NOMEM; }
        memcpy(copy, resp.body, blen); copy[blen]=0;
        ce_https_response_free(&resp);
        if(g_artist_body) free(g_artist_body);
        g_artist_body=copy; g_artist_body_len=blen;
        _snprintf(g_artist_id,sizeof(g_artist_id),"%s",browse_id);
    }

    n = want_albums ? parse_card_rows(g_artist_body, g_artist_body_len, tracks, cap)
                    : parse_track_rows(g_artist_body, g_artist_body_len, CE_IT_CAT_SONGS, tracks, cap);
    if(count_out) *count_out=n;
    return CE_IT_OK;
}
