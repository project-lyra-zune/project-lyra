/* ytsearchd.exe: the zune-yt search daemon (mirrors zune-cast's castd.exe).
 *
 * Spawned at boot by the youtube mod's `daemons` capability (CreateProcessW, no
 * args). Its own process + Winsock, so the blocking HTTPS search never touches
 * the gemstone UI thread. IPC = a named shared section (yt_search_ipc.h):
 *
 *   loop: WaitForSingleObject(WAKE)               // UI bumped req_seq + SetEvent
 *         read block->query                       // the typed query (wide)
 *         ce_innertube_search(query) -> rows      // blocking HTTPS, off the UI
 *         fill block->rows/count/status
 *         block->done_seq = block->req_seq        // mark this request answered
 *         SetEvent(DONE)                          // gemstone's pump wait wakes (UI thread)
 *
 * The UI side (youtube.dll) adds DONE to gemstone's MsgWaitForMultipleObjectsEx
 * set; on signal it re-snapshots the section + LIST_INVALIDATE on the UI thread;
 * the same out-of-process→UI delivery the modkit/zune-cast use, no native marshal. */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "ce_innertube.h"
#include "ce_https.h"
#include "yt_search_ipc.h"

static void L(const char* s){
    HANDLE f=CreateFileW(L"\\flash2\\automation\\ytsearchd.log",GENERIC_WRITE,
                         FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE)return; SetFilePointer(f,0,NULL,FILE_END);
    DWORD n; WriteFile(f,s,(DWORD)strlen(s),&n,NULL); WriteFile(f,"\r\n",2,&n,NULL); CloseHandle(f);
}
static void Lx(const char* t,long v){ char b[160]; _snprintf(b,sizeof(b),"%s=%ld",t,v); L(b); }

/* narrow a wide query to ASCII for the JSON body (English queries; drop >0x7f). */
static void wide_to_ascii(const wchar_t* w, char* out, int cap){
    int o=0; for(int i=0; w[i] && o<cap-1; i++){ if((unsigned)w[i]<0x80) out[o++]=(char)w[i]; }
    out[o]=0;
}
/* widen an ASCII string into a wide field (NUL-terminated, capped). */
static void ascii_to_wide(const char* s, wchar_t* out, int cap){
    int o=0; for(int i=0; s[i] && o<cap-1; i++) out[o++]=(wchar_t)(unsigned char)s[i];
    out[o]=0;
}

/* Build the per-row art cache path `YT_ART_DIR\<id>.jpg`. The id
 * (videoId/browseId) is filesystem-safe ASCII; the UI side derives the same name. */
static void art_path_w(const char* id, wchar_t* out, int cap){
    static const wchar_t* DIR = YT_ART_DIR L"\\";
    int o=0, i;
    for(i=0; DIR[i] && o<cap-6; i++) out[o++]=DIR[i];
    for(i=0; id[i] && o<cap-6; i++) out[o++]=(wchar_t)(unsigned char)id[i];
    out[o++]=L'.'; out[o++]=L'j'; out[o++]=L'p'; out[o++]=L'g'; out[o]=0;
}

/* Background pass after a search answers: download each row's thumbnail JPEG to its
 * cache file (atomic temp+rename) and re-signal DONE so the UI re-pulls and the art
 * fills in. Bails the moment a newer request arrives (req_seq moved). The bytes are
 * served as-is; YouTube thumbs are JPEG (ytimg .jpg / lh3 -rj), imaging.dll decodes. */
static void fetch_art_pass(YtSearchBlock* blk, long seq,
                           struct ce_innertube_track* tr, int n, HANDLE done){
    static const wchar_t* TMP = YT_ART_DIR L"\\_dl.tmp";
    int wrote=0, i;
    CreateDirectoryW(YT_ART_DIR, NULL);
    for(i=0;i<n;i++){
        wchar_t out[160]; int st=0; unsigned long got=0; enum ce_https_result hr;
        /* Abandon only for a NEW user-initiated request (new search or tab switch =
         * mode 0). A same-query continuation (mode 1) is background prefetch; let the
         * visible page's art finish first; that pending request just waits its turn. */
        if(blk->req_seq != seq && blk->mode == 0) return;
        if(!tr[i].id[0] || !tr[i].thumb[0]) continue;
        art_path_w(tr[i].id, out, 160);
        if(GetFileAttributesW(out) != 0xFFFFFFFF) continue;   /* already cached */
        DeleteFileW(TMP);
        hr = ce_https_download_url(tr[i].thumb, NULL, TMP, 0, &st, &got);
        if(hr==CE_HTTPS_OK && st==200 && got>0 && MoveFileW(TMP, out)){
            wrote++; SetEvent(done);                          /* re-pull so this row's art pops in */
        } else {
            DeleteFileW(TMP);
        }
    }
    (void)wrote;
}

static YtSearchBlock* map_block(void){
    HANDLE h=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(YtSearchBlock),YT_SEARCH_SECTION_NAME);
    if(!h) return NULL;
    YtSearchBlock* b=(YtSearchBlock*)MapViewOfFile(h,FILE_MAP_ALL_ACCESS,0,0,0);
    return b;
}

int WINAPI wWinMain(HINSTANCE a,HINSTANCE b,LPWSTR c,int d){
    (void)a;(void)b;(void)c;(void)d;
    L("=== ytsearchd start ===");
    /* Priority is set per request below (foreground vs background). Idle default is
     * LOWEST so the daemon never contends while parked on the wake event. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    { WSADATA w; WSAStartup(MAKEWORD(2,2),&w); }  // own Winsock (separate process)

    YtSearchBlock* blk = map_block();
    if(!blk){ L("map_block failed"); return -1; }
    if(blk->version==0) blk->version=YT_SEARCH_VERSION;

    HANDLE wake = CreateEventW(NULL, FALSE, FALSE, YT_SEARCH_WAKE_EVENT);   // auto-reset
    HANDLE done = CreateEventW(NULL, FALSE, FALSE, YT_SEARCH_DONE_EVENT);   // auto-reset
    if(!wake || !done){ L("event create failed"); return -2; }

    struct ce_innertube_track tracks[YT_SEARCH_MAX_ROWS];
    for(;;){
        WaitForSingleObject(wake, INFINITE);
        long seq = blk->req_seq;
        long cat = blk->category;
        long mode = blk->mode;
        wchar_t q[YT_SEARCH_QUERY_LEN]; wcsncpy(q, blk->query, YT_SEARCH_QUERY_LEN-1); q[YT_SEARCH_QUERY_LEN-1]=0;
        char qa[YT_SEARCH_QUERY_LEN]; wide_to_ascii(q, qa, sizeof(qa));
        char bid[YT_SEARCH_ID_LEN]; strncpy(bid, blk->browse_id, YT_SEARCH_ID_LEN-1); bid[YT_SEARCH_ID_LEN-1]=0;
        static char cont[YT_SEARCH_CONT_LEN];
        cont[0]=0;
        if(mode==1){ strncpy(cont, blk->continuation, YT_SEARCH_CONT_LEN-1); cont[YT_SEARCH_CONT_LEN-1]=0; }

        int n=0;
        enum ce_innertube_result rc;
        DWORD t0 = GetTickCount();
        if(mode==3){
            /* artist drill: category 0 = albums, 1 = songs. One /browse body, cached
             * in ce_innertube, serves both tabs (the second is parse-only, no fetch). */
            int want_albums = (cat==0);
            { char m[200]; _snprintf(m,sizeof(m),"artist req_seq=%ld want=%s browseId=\"%s\"",seq,want_albums?"albums":"songs",bid); L(m); }
            rc = ce_innertube_artist(bid, want_albums, tracks, YT_SEARCH_MAX_ROWS, &n);
        } else if(mode==2){
            /* drill-in: list an album/playlist's tracks via /browse */
            { char m[200]; _snprintf(m,sizeof(m),"browse req_seq=%ld browseId=\"%s\"",seq,bid); L(m); }
            rc = ce_innertube_browse(bid, NULL, tracks, YT_SEARCH_MAX_ROWS, &n,
                                     blk->continuation, YT_SEARCH_CONT_LEN);
        } else {
            { char m[200]; _snprintf(m,sizeof(m),"search req_seq=%ld cat=%ld mode=%ld query=\"%s\"",seq,cat,mode,qa); L(m); }
            rc = ce_innertube_search(qa, (int)cat, (mode==1?cont:NULL),
                                     tracks, YT_SEARCH_MAX_ROWS, &n,
                                     blk->continuation, YT_SEARCH_CONT_LEN);
        }
        Lx("  request ms", (long)(GetTickCount()-t0));
        { int ct=0,tl=0,rc2=0; ce_https_last_timing(&ct,&tl,&rc2);
          char m[160]; _snprintf(m,sizeof(m),"  net connect=%d tls=%d recv=%d",ct,tl,rc2); L(m); }
        if(n>YT_SEARCH_MAX_ROWS) n=YT_SEARCH_MAX_ROWS;
        for(int i=0;i<n;i++){
            strncpy(blk->rows[i].id, tracks[i].id, YT_SEARCH_ID_LEN-1);
            blk->rows[i].id[YT_SEARCH_ID_LEN-1]=0;
            blk->rows[i].is_video = tracks[i].is_video;
            ascii_to_wide(tracks[i].title,  blk->rows[i].title,  YT_SEARCH_TITLE_LEN);
            ascii_to_wide(tracks[i].artist, blk->rows[i].artist, YT_SEARCH_ARTIST_LEN);
            ascii_to_wide(tracks[i].album,  blk->rows[i].album,  YT_SEARCH_ARTIST_LEN);
            blk->rows[i].duration_ms = tracks[i].duration_ms;
        }
        blk->status = (long)rc;
        blk->count  = n;
        blk->category = cat;          // echo the answered category
        blk->done_seq = seq;          // mark answered before waking the UI
        Lx("  done count", n);
        SetEvent(done);               // gemstone pump wait wakes on the UI thread

        // Background-fetch this page's thumbnails; re-signals DONE as files land so
        // the UI re-pulls and art appears. Bails if a newer request arrives.
        fetch_art_pass(blk, seq, tracks, n, done);
    }
    /* not reached */
}
