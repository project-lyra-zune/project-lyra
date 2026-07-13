/* youtube_gem.cpp: self-contained YouTube feature-mod DLL (gemstone).
 *
 * Loaded into the gemstone shell process by the modkit `load_module` action
 * (manifest init export: YtInstall). YtInstall registers two NOVEL XUI scene
 * classes directly via xuidll!XuiRegisterClass; no class blobs, no zuxhook
 * changes (the system-mod path). The class handlers, factory, ctor, and vtable
 * all live here in our DLL; the engine only ever sees absolute VAs, so it does
 * not care that they point into a loaded feature DLL rather than gemstone XIP.
 *
 *   GemYtHub                 : landing (search / browse buttons)
 *   GemYtResultsContentScene : results list; row tap streams ytm://<videoId>
 *
 * Class-registration ABI mirrors tools/modkit/classblob.py + firmware_v45_abi.py
 * (the values the system-mod blob path bakes), reimplemented in C:
 *   descriptor[11]: [1]=name, [2]=GemBaseScene parent name, [5]=dtor,
 *                   [6]=factory, [7]=finalizer; call reg(&desc[1], &desc[0]).
 *   factory(ctx,out): alloc(size) -> ctor (vtable@+0, scene_handle@+4) -> *out.
 *   vtable[4]: [0]=parent_vtable, [1]=OnMessage, [2]=OnInit, [3]=OnDestroy.
 *
 * The DLL must NEVER be unloaded; the engine holds raw pointers into its
 * .text (factory/vtable/handlers) for the gemstone process lifetime.
 */

#include <windows.h>
#include <stdio.h>

#include "yt_search_ipc.h"

/* ── gemstone v4.5 absolute VAs (image base 0x00010000) ─────────────────── */
#define ALLOCATOR            0x00084ee4   /* coredll ord1095 ("operator new") */
#define PARENT_VTABLE_BASE   0x0002aca0   /* GemBaseScene base vtable */
#define CLASS_DESTROY_SHARED 0x0008678c   /* canonical OnDestroy */
#define DESC_DESTRUCTOR      0x0002acf4   /* descriptor +0x14 */
#define DESC_FINALIZER       0x0002a7ac   /* descriptor +0x1c */
#define GEMBASESCENE_NAME    0x00011be0   /* L"GemBaseScene" in gemstone RO */
#define GEMLIBRARYBASESCENE_NAME 0x00011b6c   /* L"GemLibraryBaseScene", content-swap helper parent */

typedef int (*SetLabelTextFn)(void*, const wchar_t*);
#define SET_LABEL_TEXT  ((SetLabelTextFn)0x00038434)   /* (elem, wstring) → msg 0x7e0 */

typedef HRESULT (*XuiElementGetDescendantByIdFn)(void*, const wchar_t*, void**, int);
#define XUI_GET_DESC_BY_ID  ((XuiElementGetDescendantByIdFn)0x0006afec)

typedef HRESULT (*MessageHandlerFn)(void*, void*);
#define XUISCENE_ON_MESSAGE  ((MessageHandlerFn)0x000653f4)

typedef HRESULT (*XuiSceneCreateFn)(const wchar_t*, const wchar_t*, void*, void**);
#define XUI_SCENE_CREATE  ((XuiSceneCreateFn)0x418358d0)   /* xuidll, XIP */

typedef int (*SceneNavigateFn)(void*, void*);
#define SCENE_NAVIGATE  ((SceneNavigateFn)0x0001e5d8)
#define CURRENT_CTX_GLOBAL  ((void**)0x00097300)

typedef HRESULT (*RawRowLabelFn)(DWORD, DWORD, const wchar_t*);
#define RAW_ROW_LABEL  ((RawRowLabelFn)0x00083914)

/* Image-source row assign, the sibling of RAW_ROW_LABEL used by the get-image
 * pull (sub 0x3e9). The native file:// path (GemLibraryListContentScene get-image
 * 0x320dc → 0x28754 type 0xfc) calls it as (out_8, out_c, scheme, &out_8, &out_c, 0). */
typedef HRESULT (*ImgRowAssignFn)(DWORD, DWORD, const wchar_t*, DWORD*, DWORD*, int);
#define IMG_ROW_ASSIGN  ((ImgRowAssignFn)0x00083a14)

typedef int (*ListGetSelectedIdxFn)(void*, int*);
#define GET_SELECTED_IDX  ((ListGetSelectedIdxFn)0x0003195c)

typedef int (*ListInvalidateFn)(void*, int, int);
#define LIST_INVALIDATE  ((ListInvalidateFn)0x00058890)

typedef int (*ListGetRowCountFn)(void*);
#define LIST_GET_ROW_COUNT  ((ListGetRowCountFn)0x0004b058)

typedef int (*SetShowFn)(void*, int);
#define SET_SHOW  ((SetShowFn)0x00058860)

/* ── message constants ──────────────────────────────────────────────────── */
#define MSG_INIT_BIND        0x13
#define MSG_DATA_SOURCE      0xe
#define SUB_DS_SET_SEL       0x01
#define SUB_DS_GET_ITEM      0x3e8
#define SUB_DS_GET_IMAGE     0x3e9
#define SUB_DS_COUNT         0x3eb
#define MSG_NAV_SOURCE_QUERY 0x18000022

struct DataSourceSubStruct {
    DWORD sub_code;
    DWORD target_elem;
    DWORD size_hint;
    DWORD output_area;
};

/* ── out-of-process search IPC (yt_search_ipc.h) ────────────────────────────
 * ytsearchd.exe runs the blocking HTTPS search in its own process and writes the
 * result rows into the named shared section; the UI side reads them. Completion
 * is delivered on the UI thread by joining the daemon's auto-reset DONE event to
 * gemstone's message-pump wait set (the MsgWaitForMultipleObjectsEx IAT hook
 * below), the same out-of-process→UI pathway the modkit/zune-cast notify uses,
 * specialised to one producer + one consumer (no MsgQueue fan-out needed). */
#define MSGWAIT_IAT_GEMSTONE  0x00096244u   /* coredll ord871 import slot, gemstone image */
#define MWMO_WAITALL          0x00000001u

typedef DWORD (WINAPI *MsgWaitFn)(DWORD, const HANDLE*, DWORD, DWORD, DWORD);
static MsgWaitFn      g_orig_wait = NULL;   /* real MsgWaitForMultipleObjectsEx */
static YtSearchBlock* g_search    = NULL;   /* shared section (daemon writes, we read) */
static HANDLE         g_wake      = NULL;   /* we SetEvent → daemon wakes */
static HANDLE         g_done      = NULL;   /* daemon SetEvents → our pump wakes */

/* Per-category result cache. Each tab keeps its own loaded rows, displayed count,
 * and continuation token, so switching back to a category shows its results
 * instantly (no re-fetch) and one category's fetch can't clobber another's. All
 * touched only on the UI/pump thread, so no locking. */
#define YT_CATEGORY_COUNT 4
#define YT_CAT_MAX 96   /* per-category row cap (accumulated across continuations) */
typedef struct {
    YtSearchRow  rows[YT_CAT_MAX];
    volatile LONG count;       /* rows loaded into the buffer */
    volatile LONG displayed;   /* rows currently shown in the list */
    volatile LONG fetched;     /* 0 = never fetched; 1 = first page answered (data or empty) */
    char          cont[YT_SEARCH_CONT_LEN];  /* next-page token ("" = exhausted) */
} YtCatBuf;
static YtCatBuf      g_cat[YT_CATEGORY_COUNT];
static volatile LONG g_active_category   = 0;    /* tab currently shown */
static volatile LONG g_fetch_inflight    = 0;    /* a daemon request is outstanding */
static volatile LONG g_pending_category  = -1;   /* a first-page fetch deferred behind the inflight one */
static volatile LONG g_query_gen         = 0;    /* bumped on each new query */
static volatile LONG g_inflight_gen      = 0;    /* the query-gen of the in-flight request */
static const wchar_t* const YT_CATEGORIES[] = { L"songs", L"albums", L"artists", L"playlists" };

static void yt_request_category(int cat);   /* defined with the search-IPC plumbing */
static void yt_pump(void);                   /* issue the next queued/prefetch request */

struct GemYtResultsInstance;
static GemYtResultsInstance* g_active_results = NULL;   /* active results/album scene, for invalidation */

static void yt_search_submit(const wchar_t* query);     /* defined after nav_to_scene_by_name */

/* Drill-in (browse) state. Tapping an album/playlist/artist row issues a /browse via
 * the daemon (mode 2) into g_browse and navigates to the album scene, which renders
 * g_browse with the same list machinery the search results use (the scene's result_buf
 * points at g_browse instead of a search category cache). One drill in flight at a time;
 * a deeper drill reuses the same buffer (the back stack lives in the engine's nav). */
static YtCatBuf g_browse;                                  /* drill-in track buffer */
static char     g_pending_browse[YT_SEARCH_ID_LEN];        /* browseId queued for the pump ("" = none) */
static wchar_t  g_browse_title[YT_SEARCH_TITLE_LEN];       /* tapped entity title, album-scene header */
static wchar_t  g_browse_subtitle[YT_SEARCH_ARTIST_LEN];   /* tapped entity subtitle, album-scene artist line */
static void yt_browse_submit(const char* browse_id, const wchar_t* title, const wchar_t* subtitle);
static void yt_row_activate(const YtSearchRow* r);         /* play (song) / album drill / artist drill */

/* Artist drill (UC… browseId) → a two-tab twist (Albums, Songs). Both tabs come from
 * one /browse (the daemon caches the body), so each tab has its own UI buffer; tab 0 =
 * albums, tab 1 = songs. Mirrors g_cat but for the active artist. */
#define YT_ARTIST_TABS 2
static YtCatBuf g_artist[YT_ARTIST_TABS];                  /* [0]=albums, [1]=songs */
static char     g_artist_browse_id[YT_SEARCH_ID_LEN];      /* the artist being shown */
static wchar_t  g_artist_name[YT_SEARCH_TITLE_LEN];        /* artist name, host header */
static volatile LONG g_pending_artist = -1;                /* artist tab queued for the pump (-1 = none) */
static void yt_artist_submit(const char* browse_id, const wchar_t* name);
static void yt_request_artist(int tab);                    /* fetch a tab's data (or show cached) */

/* ── one-shot install logging (Phase2Worker thread, not the pump) ────────── */
static void L(const char* s) {
    HANDLE f = CreateFileW(L"\\flash2\\automation\\youtube.log", GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    DWORD n; WriteFile(f, s, (DWORD)strlen(s), &n, NULL); WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}
static void Lx(const char* t, HRESULT hr) { char b[160]; _snprintf(b, sizeof(b), "%s hr=0x%08x", t, hr); L(b); }

/* ════════════════════════════════ GemYtHub ════════════════════════════════ */

struct GemYtHubInstance {
    DWORD vtable;            /* +0x00 */
    DWORD scene_handle;      /* +0x04 */
    DWORD breadcrumb_elem;   /* +0x08: XuiScene base reads */
    DWORD reserved_0c;       /* +0x0c..+0x24: base writes */
    DWORD reserved_10;
    DWORD reserved_14;
    DWORD nav_source_elem;   /* +0x18: tapped button; engine reads via 0x18000022 */
    DWORD reserved_1c;
    DWORD reserved_20;
    DWORD reserved_24;
    DWORD search_btn;        /* +0x28 */
    DWORD browse_btn;        /* +0x2c */
    DWORD reserved_30;
    DWORD reserved_34;
    DWORD reserved_38;
    DWORD reserved_3c;
};

static int nav_to_scene_by_name(const wchar_t* name) {
    void* h = NULL;
    HRESULT hr = XUI_SCENE_CREATE(L"gem://", name, NULL, &h);
    if (FAILED(hr) || h == NULL) return -1;
    return SCENE_NAVIGATE(*CURRENT_CTX_GLOBAL, h);
}

/* Hand a committed query to the search daemon and open the results scene. The
 * daemon answers asynchronously; its DONE event drives the list refresh (see the
 * MsgWait hook). The results scene shows its no-items state until rows arrive.
 * The .xur suffix is required; XuiSceneCreate builds the URI as gem://<name>. */
static void yt_search_submit(const wchar_t* query) {
    if (!g_search || !g_wake || !query) return;
    int i = 0;
    for (; query[i] && i < YT_SEARCH_QUERY_LEN - 1; i++) g_search->query[i] = query[i];
    g_search->query[i] = 0;
    /* new query → drop every category's cache so each tab re-fetches; bump the
     * generation so any fetch still in flight from the previous query is ignored
     * when it lands (the daemon can't be cancelled mid-request). */
    for (int k = 0; k < YT_CATEGORY_COUNT; k++) {
        g_cat[k].count = 0; g_cat[k].displayed = 0; g_cat[k].fetched = 0; g_cat[k].cont[0] = 0;
    }
    g_pending_category = -1;
    g_query_gen++;
    nav_to_scene_by_name(L"YtSearch.xur");
}

/* Native on-screen keyboard summon; device-captured (bp gemstone 0x72ea4 on a
 * marketplace-search-bar tap): sub_0x72ea4(0,1,8,&args), args={placeholderPtr,
 * titlePtr, 0x100=maxlen}. The requester is implicit (the current scene), so the
 * typed-text messages route back to whichever scene calls this. */
typedef int (*KbdSummonFn)(int,int,int,void*);
#define KBD_SUMMON  ((KbdSummonFn)0x00072ea4)
typedef int  (*KbdGetTextFn)(void* buf, int maxlen, int z);   /* op6: pull typed text */
#define KBD_GETTEXT ((KbdGetTextFn)0x00072ba4)
typedef void (*KbdCloseFn)(void);                             /* op9: dismiss keyboard */
#define KBD_CLOSE   ((KbdCloseFn)0x00072c20)
/* keyboard result, delivered to the active scene element's OnMessage (RE 2026-06-19):
 *   msg 0x25, payload[0]: 0xef=commit, 0xf0=cancel, 0xee=text-changed (live)
 * On commit the RECEIVER pulls the text (op6) and dismisses (op9) itself. */
#define MSG_KBD_CTRL   0x25
/* Engine "detached / dismissed" message: sent to a hosted keyboard when it closes
 * (payload[0]==1) and to an outgoing content scene before it is destroyed. */
#define MSG_DETACHED   0x18000007
#define KBD_COMMIT     0xef
#define KBD_CANCEL     0xf0
static const wchar_t KBD_SUBMIT[]      = L"search";          /* enter/submit button label */
static const wchar_t KBD_PLACEHOLDER[] = L"search youtube";  /* field placeholder */
static void summon_keyboard(void){
    DWORD args[3];
    args[0]=(DWORD)KBD_PLACEHOLDER; args[1]=(DWORD)KBD_SUBMIT; args[2]=0x100;
    __try { KBD_SUMMON(0,1,8,args); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static HRESULT GemYtHub_OnInit(GemYtHubInstance* self) {
    if (!self) return -1;
    self->breadcrumb_elem = 0; self->search_btn = 0; self->browse_btn = 0;
    self->nav_source_elem = 0;
    void* h = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &h, 0);
    self->breadcrumb_elem = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"searchButton", &h, 0);
    self->search_btn = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"browseButton", &h, 0);
    self->browse_btn = (DWORD)h;
    return 0;
}

static HRESULT GemYtHub_OnMessage(GemYtHubInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    /* keyboard result channel, delivered to the active scene's OnMessage. On commit
     * we PULL the typed text (op6) and DISMISS (op9); the keyboard stays up until we do. */
    if (msg_id == MSG_KBD_CTRL) {
        DWORD ksub = 0;
        __try { DWORD* p=(DWORD*)m[4]; if(p) ksub=p[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ksub == KBD_COMMIT) {
            wchar_t q[0x104]; q[0]=0;
            __try { KBD_GETTEXT(q, 0x104, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { q[0]=0; }
            __try { KBD_CLOSE(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            /* seed the breadcrumb-morph source (engine reads it back via 0x18000022) */
            self->nav_source_elem = self->search_btn;
            if (q[0]) yt_search_submit(q);
            __try { m[2]=1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (ksub == KBD_CANCEL) {
            __try { KBD_CLOSE(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            __try { m[2]=1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
    }
    if (msg_id == MSG_DETACHED) {
        DWORD ph=0;
        __try { DWORD* p=(DWORD*)m[4]; if(p) ph=p[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ph == 1) { __try { KBD_CLOSE(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    }

    DWORD sub_code = 0, target = 0;
    DataSourceSubStruct* sub = NULL;
    if (msg_id == MSG_DATA_SOURCE) {
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (msg_id == MSG_NAV_SOURCE_QUERY) {
        __try {
            DWORD* sub_payload = (DWORD*)m[4];
            if (sub_payload) sub_payload[0] = self->nav_source_elem;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    if (msg_id == MSG_DATA_SOURCE && sub && sub_code == SUB_DS_SET_SEL) {
        if (target && target == self->search_btn) {
            /* summon the native keyboard; the typed-text messages route back here */
            summon_keyboard();
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        /* browse button not yet implemented; fall through to base. */
    }

    HRESULT hr = 0;
    __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}

/* ═══════════════════════ GemYtResultsContentScene ═════════════════════════ */

/* Shared by the search results content scene and the drill-in album scene. The album
 * scene leaves more_elem/moregroup_elem NULL (no pagination) and points result_buf at
 * g_browse; the content scene points result_buf at the active search category cache. */
struct GemYtResultsInstance {
    DWORD vtable;            /* +0x00 */
    DWORD scene_handle;      /* +0x04 */
    DWORD breadcrumb_elem;   /* +0x08 */
    DWORD reserved_0c[3];    /* +0x0c..+0x14 */
    DWORD nav_source_elem;   /* +0x18: tapped row; engine reads via 0x18000022 for the morph */
    DWORD reserved_1c[3];    /* +0x1c..+0x24 */
    DWORD list_element;      /* +0x28 */
    DWORD noItems_element;   /* +0x2c */
    DWORD more_elem;         /* +0x30: "more" ExpandButton */
    DWORD moregroup_elem;    /* +0x34: moreloadingGroup container */
    DWORD loading_elem;      /* +0x38: "Loading..." animated label */
    DWORD result_buf;        /* +0x3c: YtCatBuf* the list renders (search cache or g_browse) */
    DWORD reserved_40[4];    /* +0x40..+0x4c: pad to 0x50 */
};

typedef int (*PlaySongFromUrlFn)(const wchar_t*, const wchar_t*);
typedef int (*QGetActiveIdxFn)(int*);
typedef int (*QGetSongAtFn)(int, void**);
/* Internal song-property setter, the one PlaySongFromURL itself uses to stamp the
 * title (prop 0x20001) and url (0x2000a) on its freshly-built song. zdksystem is XIP
 * (fixed VA 0x41970000), so this absolute address is callable once it's loaded. */
typedef int (*ZdkSetPropFn)(void*, DWORD, DWORD);
#define ZDK_SETPROP ((ZdkSetPropFn)0x4198474c)   /* string props (title/artist/album/url) */
#define ZDK_SETINT  ((ZdkSetPropFn)0x4198464c)   /* int props: what PlaySongFromFile uses for 0x10003 duration */
static PlaySongFromUrlFn g_play = NULL;
static HMODULE g_zdk = NULL;

/* Streamed-rate copy of the daemon-cached thumbnail into the content store. */
static void yt_copy_file(const wchar_t* src, const wchar_t* dst) {
    HANDLE in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE) return;
    HANDLE out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (out != INVALID_HANDLE_VALUE) {
        char buf[2048]; DWORD got = 0, put = 0;
        while (ReadFile(in, buf, sizeof(buf), &got, NULL) && got) WriteFile(out, buf, got, &put, NULL);
        CloseHandle(out);
    }
    CloseHandle(in);
}

/* Make now-playing show album art for the streamed (runtime, kind-0xfd) song.
 * The now-playing art element loads media://<id>.art, whose handler reads content
 * prop 0x2000b. That prop is COMPUTED from the song id by zmedia 0x41a27474 as
 * \Flash2\Content\<id>>16>\<(id>>8)&0xff>\art\<id&0xff>_art.jpg, with no content-kind
 * gate (zmedia 0x41a1e540), so it resolves a path even for a runtime id. The string
 * prop 0x2000b itself is REJECTED by the runtime whitelist (zmedia 0x41a25ca0), so we
 * cannot point it anywhere; instead we drop the cached thumbnail AT that computed path
 * and set has-art (int prop 0x10008, which the runtime whitelist DOES accept). The zdk
 * song handle is the kind-tagged id. */
static void yt_attach_art(void* song, const char* video_id) {
    DWORD id = (DWORD)song;
    __try { ZDK_SETINT(song, 0x10008, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}

    static const wchar_t HEX[] = L"0123456789abcdef";
    wchar_t dir[64]; int n = 0;
    const wchar_t* root = L"\\Flash2\\Content\\";
    while (root[n]) { dir[n] = root[n]; n++; }
    dir[n++] = HEX[(id>>28)&0xf]; dir[n++] = HEX[(id>>24)&0xf];
    dir[n++] = HEX[(id>>20)&0xf]; dir[n++] = HEX[(id>>16)&0xf];
    dir[n] = 0; CreateDirectoryW(dir, NULL);
    dir[n++] = L'\\'; dir[n++] = HEX[(id>>12)&0xf]; dir[n++] = HEX[(id>>8)&0xf];
    dir[n] = 0; CreateDirectoryW(dir, NULL);
    dir[n++] = L'\\'; dir[n++] = L'a'; dir[n++] = L'r'; dir[n++] = L't';
    dir[n] = 0; CreateDirectoryW(dir, NULL);

    wchar_t src[160]; int o = 0;
    const wchar_t* sp = YT_ART_DIR L"\\";
    while (sp[o]) { src[o] = sp[o]; o++; }
    int j = 0; while (video_id[j] && o < 150) { src[o++] = (wchar_t)(unsigned char)video_id[j++]; }
    src[o++] = L'.'; src[o++] = L'j'; src[o++] = L'p'; src[o++] = L'g'; src[o] = 0;

    /* Filename <atom>_art.jpg, the resolver formats the atom byte as "%02x"
     * (device-confirmed: id 0xfd000001 -> "01_art.jpg"). */
    unsigned low = id & 0xff;
    wchar_t dst[80]; int m = 0, k;
    for (k = 0; k < n; k++) dst[m++] = dir[k];
    dst[m++] = L'\\';
    dst[m++] = HEX[(low>>4)&0xf]; dst[m++] = HEX[low&0xf];
    const wchar_t* sfx = L"_art.jpg"; int s = 0; while (sfx[s]) dst[m++] = sfx[s++]; dst[m] = 0;
    yt_copy_file(src, dst);
}

/* Stamp artist (0x20002) + album (0x20003, strings) + duration (0x10003, int ms) onto
 * the active song so now-playing shows them. Device-RE'd: title 0x20001 is already set
 * by PlaySongFromURL; duration uses the INT setter 0x4198464c (PlaySongFromFile's path),
 * NOT the string setter. Set synchronously at play time so the scene reads them on its
 * first activation (no /next round-trip, no late refresh). Also attaches album art. */
static void np_set_song_meta(const wchar_t* artist, const wchar_t* album, long duration_ms, const char* video_id) {
    QGetActiveIdxFn gidx = g_zdk ? (QGetActiveIdxFn)GetProcAddress(g_zdk, L"ZDKMedia_Queue_GetActiveSongIndex") : NULL;
    QGetSongAtFn   gsong = g_zdk ? (QGetSongAtFn)GetProcAddress(g_zdk, L"ZDKMedia_Queue_GetSongAtIndex") : NULL;
    if (!gidx || !gsong) return;
    int idx = -1; void* song = NULL;
    if (gidx(&idx) < 0 || idx < 0 || gsong(idx, &song) < 0 || !song) return;
    if (artist && artist[0]) __try { ZDK_SETPROP(song, 0x20002, (DWORD)artist); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (album  && album[0])  __try { ZDK_SETPROP(song, 0x20003, (DWORD)album);  } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (duration_ms > 0)     __try { ZDK_SETINT (song, 0x10003, (DWORD)duration_ms); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (video_id && video_id[0]) __try { yt_attach_art(song, video_id); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Stream a result row through the native queue. Only song rows are playable
 * (is_video); album/artist/playlist rows carry a browseId for future drill-in.
 * The id is ASCII; widen it into the ytm:// URL the source filter consumes. */
static void play_result(const YtSearchRow* r) {
    if (!r || !r->is_video) return;
    if (!g_play) {
        g_zdk = LoadLibraryW(L"zdksystem.dll");
        if (g_zdk) g_play = (PlaySongFromUrlFn)GetProcAddress(g_zdk, L"ZDKMedia_Queue_PlaySongFromURL");
    }
    if (!g_play) return;
    wchar_t url[64]; const wchar_t* pfx = L"ytm://"; int o = 0;
    while (pfx[o]) { url[o] = pfx[o]; o++; }
    int i = 0; while (r->id[i] && o < 62) { url[o++] = (wchar_t)(unsigned char)r->id[i++]; }
    url[o] = 0;
    const wchar_t* title = r->title[0] ? r->title : L"YouTube";
    __try { g_play(title, url); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    np_set_song_meta(r->artist, r->album, r->duration_ms, r->id);
}

/* Activate a tapped row. A song row (is_video) streams. A browseId row drills by its
 * entity type, matching the marketplace chain: an artist (UC…) opens the Albums/Songs
 * twist; an album (MPRE…) or playlist (VL…) opens its track list. Shared by every list
 * (search results, artist tabs, album tracks), so the gesture works at every level. */
static void yt_row_activate(const YtSearchRow* r) {
    if (!r || !r->id[0]) return;
    if (r->is_video) { play_result(r); return; }
    if (r->id[0]=='U' && r->id[1]=='C') yt_artist_submit(r->id, r->title);
    else                                yt_browse_submit(r->id, r->title, r->artist);
}

/* Render one list data-source pull (count / get-item / get-image / tap) against `buf`
 * for `self`'s list element. Shared by the search results scene and the album scene;
 * identical row data flow, only the backing buffer differs. Returns 1 if it consumed
 * the message (and set m[2]=1). The caller guarantees the message targets the list. */
static int yt_list_pull(GemYtResultsInstance* self, void* msg,
                        DataSourceSubStruct* sub, YtCatBuf* buf) {
    DWORD* m = (DWORD*)msg;
    int displayed = buf ? (int)buf->displayed : 0;
    DWORD sub_code = sub->sub_code;

    if (sub_code == SUB_DS_COUNT) {
        __try { *((DWORD*)(sub->output_area + 4)) = (DWORD)displayed; m[2] = 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 1;
    }
    if (sub_code == SUB_DS_GET_ITEM) {
        DWORD* output = NULL; int idx = -1, col = 0; DWORD out_8 = 0, out_c = 0;
        __try {
            output = (DWORD*)sub->output_area;
            if (output) { idx = (int)output[0]; col = (int)output[1]; out_8 = output[2]; out_c = output[3]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!output || idx < 0 || idx >= displayed) return 1;
        const YtSearchRow* row = &buf->rows[idx];
        const wchar_t* field = (col == 0) ? row->title :
                               (col == 1) ? row->artist : L"";
        HRESULT hr = -1;
        __try { hr = RAW_ROW_LABEL(out_8, out_c, field); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
        if (hr >= 0) { __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        return 1;
    }
    if (sub_code == SUB_DS_GET_IMAGE) {
        DWORD* output = NULL; int idx = -1; DWORD out_8 = 0, out_c = 0;
        __try {
            output = (DWORD*)sub->output_area;
            if (output) { idx = (int)output[0]; out_8 = output[2]; out_c = output[3]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!output || idx < 0 || idx >= displayed) return 1;
        const YtSearchRow* row = &buf->rows[idx];
        if (!row->id[0]) return 1;
        wchar_t src[180];
        int o = 0; const wchar_t* pfx = L"file://" YT_ART_DIR L"\\";
        for (int i = 0; pfx[i] && o < 170; i++) src[o++] = pfx[i];
        for (int i = 0; row->id[i] && o < 170; i++) src[o++] = (wchar_t)(unsigned char)row->id[i];
        src[o++] = L'.'; src[o++] = L'j'; src[o++] = L'p'; src[o++] = L'g'; src[o] = 0;
        if (GetFileAttributesW(src + 7) == 0xFFFFFFFF) return 1;   /* not fetched yet */
        DWORD a = out_8, b2 = out_c; HRESULT hr = -1;
        __try { hr = IMG_ROW_ASSIGN(out_8, out_c, src, &a, &b2, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
        if (hr >= 0) { __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        return 1;
    }
    if (sub_code == SUB_DS_SET_SEL) {
        int tapped = -1;
        __try { tapped = GET_SELECTED_IDX((void*)self->list_element, NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { tapped = -1; }
        if (tapped >= 0 && tapped < displayed) {
            /* a drill navigation morphs the breadcrumb from the tapped list element */
            self->nav_source_elem = self->list_element;
            yt_row_activate(&buf->rows[tapped]);
        }
        __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 1;
    }
    return 0;
}

/* Marketplace model: pages load in the BACKGROUND (cat.count = loaded), but the
 * list only shows cat.displayed of them. Pressing "more" reveals the next batch,
 * so the only list re-measure happens on a deliberate tap, never during a scroll.
 * A background prefetch keeps a page buffered ahead of what's displayed. */
#define YT_BUF_AHEAD 16   /* keep at least this many loaded-but-undisplayed rows buffered */

static YtCatBuf* yt_active(void) { return &g_cat[g_active_category]; }
static int buf_has_more(YtCatBuf* c){ return (c->displayed < c->count) || (c->cont[0] && c->count < YT_CAT_MAX); }
/* The buffer a results/album scene renders: its bound result_buf, falling back to the
 * active search category for a pull that lands before MSG_INIT_BIND binds the buffer. */
static YtCatBuf* scene_buf(GemYtResultsInstance* self){
    YtCatBuf* b = self ? (YtCatBuf*)self->result_buf : (YtCatBuf*)0;
    return b ? b : yt_active();
}

static void show_elem(DWORD e, int on) {
    if (e) { __try { SET_SHOW((void*)e, on ? 1 : 0); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
}

/* Set list / "Loading…" / "No items" / "more" visibility from the current state.
 * Loading = waiting for the first page of the active category; No-items = the search
 * came back empty; otherwise show the list and the "more" affordance if applicable.
 * Does NOT touch the list rows (no re-measure); flip_visibility does that. */
static void content_show_state(GemYtResultsInstance* self) {
    if (!self) return;
    YtCatBuf* c = scene_buf(self);
    int displayed = (int)c->displayed;
    int loading = (displayed == 0) && !c->fetched;     /* first page not answered yet */
    int more_btn = (displayed > 0) && buf_has_more(c);
    show_elem(self->list_element,    displayed > 0);
    show_elem(self->loading_elem,    loading);
    show_elem(self->noItems_element, (displayed == 0 && !loading));
    show_elem(self->more_elem,       more_btn);
    show_elem(self->moregroup_elem,  loading || more_btn);
}

static void flip_visibility(GemYtResultsInstance* self) {
    if (!self) return;
    if (self->list_element && (int)scene_buf(self)->displayed > 0) {
        __try { LIST_INVALIDATE((void*)self->list_element, 0, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { LIST_GET_ROW_COUNT((void*)self->list_element); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    content_show_state(self);
}

static HRESULT GemYtResults_OnInit(GemYtResultsInstance* self) {
    if (!self) return -1;
    self->breadcrumb_elem = 0; self->nav_source_elem = 0;
    self->list_element = 0; self->noItems_element = 0;
    self->more_elem = 0; self->moregroup_elem = 0; self->loading_elem = 0;
    self->result_buf = (DWORD)&g_cat[g_active_category];   /* MSG_INIT_BIND re-binds to the bound category */
    void* h = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &h, 0);
    self->breadcrumb_elem = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"list", &h, 0);
    self->list_element = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"noItems", &h, 0);
    self->noItems_element = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"more", &h, 0);
    self->more_elem = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"moreloadingGroup", &h, 0);
    self->moregroup_elem = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"loading", &h, 0);
    self->loading_elem = (DWORD)h;
    g_active_results = self;   /* current results scene; DONE-event refresh targets it */
    return 0;
}

static HRESULT GemYtResults_OnMessage(GemYtResultsInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    DWORD sub_code = 0, target = 0;
    DataSourceSubStruct* sub = NULL;
    if (msg_id == MSG_DATA_SOURCE) {
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (msg_id == MSG_INIT_BIND) {
        /* sub_idx (twist category) arrives pointer-to-pointer at m[4]:
         * *m[4] = &sub_idx, **m[4] = the category index. */
        DWORD cat = 0;
        __try {
            DWORD** outer = (DWORD**)m[4];
            if (outer && *outer) cat = **outer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (cat >= YT_CATEGORY_COUNT) cat = 0;
        g_active_category = (LONG)cat;
        self->result_buf = (DWORD)&g_cat[cat];   /* this scene renders its category's cache */
        /* cached → shows instantly; otherwise this queues the fetch */
        yt_request_category((int)cat);
        flip_visibility(self);
        __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    /* The engine sends MSG_DETACHED during both the entry morph (payload[0] 0/2) and
     * teardown (payload[0]==1). Only teardown means this scene is going away; drop
     * the dangling pointer then so a late DONE event can't refresh a freed instance. */
    if (msg_id == MSG_DETACHED) {
        DWORD ph = 0; __try { DWORD* p=(DWORD*)m[4]; if(p) ph=p[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ph == 1 && g_active_results == self) g_active_results = NULL;
    }

    /* "more" button tap, reveal the buffered rows, then top the buffer back up.
     * The re-measure from growing the displayed count happens here (deliberate tap),
     * never during a scroll. */
    if (msg_id == MSG_DATA_SOURCE && sub && sub_code == SUB_DS_SET_SEL && target && target == self->more_elem) {
        YtCatBuf* c = scene_buf(self);
        c->displayed = c->count;                     /* reveal everything loaded so far */
        flip_visibility(self);
        yt_pump();                                   /* top the buffer back up */
        __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    if (msg_id == MSG_DATA_SOURCE && sub && target == self->list_element) {
        if (yt_list_pull(self, msg, sub, scene_buf(self))) return 0;
    }

    /* nav-source query: return the tapped row element so a drill morphs from it. */
    if (msg_id == MSG_NAV_SOURCE_QUERY) {
        __try { DWORD* p = (DWORD*)m[4]; if (p) p[0] = self->nav_source_elem; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    HRESULT hr = 0;
    __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}

/* ═══════════════════════════════ GemYtAlbum ═══════════════════════════════
 * Drill-in track list, cloned from MarketplaceAlbum (top-level scene with breadcrumb).
 * Renders g_browse (the tracks of the tapped album/playlist, or an artist's top songs)
 * through the shared list machinery (result_buf = &g_browse). A track tap streams; a
 * browseId row (reached via a deeper drill) drills again. Reuses GemYtResultsInstance:
 * same field layout, more_elem/moregroup_elem stay NULL (no pagination here yet). */

static HRESULT GemYtAlbum_OnInit(GemYtResultsInstance* self) {
    if (!self) return -1;
    self->breadcrumb_elem = 0; self->nav_source_elem = 0;
    self->list_element = 0; self->noItems_element = 0;
    self->more_elem = 0; self->moregroup_elem = 0; self->loading_elem = 0;
    self->result_buf = (DWORD)&g_browse;
    void* h = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &h, 0);
    self->breadcrumb_elem = (DWORD)h;
    self->nav_source_elem = (DWORD)h;   /* default morph source until a row is tapped */
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"list", &h, 0);
    self->list_element = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"noItems", &h, 0);
    self->noItems_element = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"loading", &h, 0);
    self->loading_elem = (DWORD)h;
    /* header (cloned from MarketplaceAlbum): album = tapped title, artist = subtitle */
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"album", &h, 0);
    if (h && g_browse_title[0])    __try { SET_LABEL_TEXT(h, g_browse_title);    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"artist", &h, 0);
    if (h && g_browse_subtitle[0]) __try { SET_LABEL_TEXT(h, g_browse_subtitle); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_active_results = self;            /* DONE-event refresh targets this scene */
    flip_visibility(self);             /* shows the loading state until the browse answers */
    return 0;
}

static HRESULT GemYtAlbum_OnMessage(GemYtResultsInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    if (msg_id == MSG_NAV_SOURCE_QUERY) {
        __try { DWORD* p = (DWORD*)m[4]; if (p) p[0] = self->nav_source_elem; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }
    if (msg_id == MSG_DETACHED) {
        DWORD ph = 0; __try { DWORD* p=(DWORD*)m[4]; if(p) ph=p[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ph == 1 && g_active_results == self) g_active_results = NULL;
    }
    if (msg_id == MSG_DATA_SOURCE) {
        DataSourceSubStruct* sub = NULL; DWORD target = 0;
        __try { sub = (DataSourceSubStruct*)m[4]; if (sub) target = sub->target_elem; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (sub && target == self->list_element) {
            if (yt_list_pull(self, msg, sub, scene_buf(self))) return 0;
        }
    }

    HRESULT hr = 0;
    __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}

/* ═══════════════════ search-daemon IPC + UI-thread delivery ════════════════ */

static YtSearchBlock* map_search_section(void) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                  sizeof(YtSearchBlock), YT_SEARCH_SECTION_NAME);
    if (!h) return NULL;
    YtSearchBlock* b = (YtSearchBlock*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (b && b->version == 0) b->version = YT_SEARCH_VERSION;
    return b;
}

/* Issue one request to the daemon. mode 0 = first page of `cat`; mode 1 =
 * continuation (append) for `cat`. Serialised by g_fetch_inflight so the shared
 * section is never overwritten mid-request. */
static void yt_issue(int cat, int mode) {
    if (!g_search || !g_wake) return;
    g_search->category = cat;
    g_search->mode = mode;
    if (mode == 1) {
        int i = 0; for (; g_cat[cat].cont[i] && i < YT_SEARCH_CONT_LEN - 1; i++) g_search->continuation[i] = g_cat[cat].cont[i];
        g_search->continuation[i] = 0;
    }
    g_fetch_inflight = 1;
    g_inflight_gen = g_query_gen;
    InterlockedIncrement(&g_search->req_seq);
    SetEvent(g_wake);
}

/* Issue a drill-in browse (mode 2) for `bid` into g_browse. Same serialisation as
 * yt_issue: one daemon request in flight, guarded by g_fetch_inflight. */
static void yt_issue_browse(const char* bid) {
    if (!g_search || !g_wake) return;
    g_search->mode = 2;
    int i = 0; for (; bid[i] && i < YT_SEARCH_ID_LEN - 1; i++) g_search->browse_id[i] = bid[i];
    g_search->browse_id[i] = 0;
    g_fetch_inflight = 1;
    g_inflight_gen = g_query_gen;
    InterlockedIncrement(&g_search->req_seq);
    SetEvent(g_wake);
}

/* Issue an artist drill (mode 3): category carries the tab (0 = albums, 1 = songs),
 * browse_id the artist's UC id. The daemon caches the artist page so the second tab
 * is parse-only. */
static void yt_issue_artist(int tab) {
    if (!g_search || !g_wake) return;
    g_search->mode = 3;
    g_search->category = tab;
    int i = 0; for (; g_artist_browse_id[i] && i < YT_SEARCH_ID_LEN - 1; i++) g_search->browse_id[i] = g_artist_browse_id[i];
    g_search->browse_id[i] = 0;
    g_fetch_inflight = 1;
    g_inflight_gen = g_query_gen;
    InterlockedIncrement(&g_search->req_seq);
    SetEvent(g_wake);
}

/* When the daemon is free, pick the next request: a queued artist tab, then a queued
 * drill-in browse, then a deferred first-page search fetch, then topping up the active
 * search category's buffer, but only while a search scene is actually showing that
 * category, so a background search prefetch can't fire while the user is in a drill-in
 * view. One request in flight at a time. */
static void yt_pump(void) {
    if (g_fetch_inflight) return;
    if (g_pending_artist >= 0) {
        int tab = g_pending_artist; g_pending_artist = -1;
        if (tab >= 0 && tab < YT_ARTIST_TABS && !g_artist[tab].fetched) { yt_issue_artist(tab); return; }
    }
    if (g_pending_browse[0]) {
        char bid[YT_SEARCH_ID_LEN]; int i = 0;
        for (; g_pending_browse[i] && i < YT_SEARCH_ID_LEN - 1; i++) bid[i] = g_pending_browse[i];
        bid[i] = 0; g_pending_browse[0] = 0;
        yt_issue_browse(bid); return;
    }
    if (g_pending_category >= 0) {
        int p = g_pending_category; g_pending_category = -1;
        if (p >= 0 && p < YT_CATEGORY_COUNT && !g_cat[p].fetched) { yt_issue(p, 0); return; }
    }
    if (g_active_results && (YtCatBuf*)g_active_results->result_buf == yt_active()) {
        YtCatBuf* c = yt_active();
        if (c->cont[0] && c->count < YT_CAT_MAX && (c->count - c->displayed) < YT_BUF_AHEAD)
            yt_issue(g_active_category, 1);
    }
}

/* Daemon DONE: store the page into its category's buffer, refresh the active scene
 * if it's that category, then pump the next request. Runs on the UI/pump thread. */
static long g_last_done_seq = -1;

static void yt_search_snapshot(void) {
    if (!g_search) return;
    /* The daemon re-signals DONE (without bumping done_seq) after each background art
     * file lands. That's not new rows; just re-pull the visible rows so the freshly
     * downloaded art loads. A real search/continuation advances done_seq. */
    if (g_search->done_seq == g_last_done_seq) {
        if (g_active_results) flip_visibility(g_active_results);
        return;
    }
    g_last_done_seq = g_search->done_seq;
    /* ignore a DONE from a previous query/drill (the buffer was reset under it) */
    if (g_inflight_gen != g_query_gen) { g_fetch_inflight = 0; yt_pump(); return; }
    int mode = (int)g_search->mode;
    /* mode 3 fills an artist tab; mode 2 the drill-in buffer; mode 0/1 a search category. */
    YtCatBuf* c;
    if (mode == 3) { int tab = (int)g_search->category; if (tab < 0 || tab >= YT_ARTIST_TABS) tab = 0; c = &g_artist[tab]; }
    else if (mode == 2) c = &g_browse;
    else { int cat = (int)g_search->category; if (cat < 0 || cat >= YT_CATEGORY_COUNT) cat = 0; c = &g_cat[cat]; }
    int n = (int)g_search->count;
    if (n < 0) n = 0;
    if (n > YT_SEARCH_MAX_ROWS) n = YT_SEARCH_MAX_ROWS;

    int is_append = (mode == 1);
    if (is_append) {
        /* continuation: append to the buffer (count grows; displayed unchanged) */
        int base = (int)c->count;
        for (int i = 0; i < n && base + i < YT_CAT_MAX; i++) c->rows[base + i] = g_search->rows[i];
        int total = base + n; if (total > YT_CAT_MAX) total = YT_CAT_MAX;
        c->count = total;
    } else {
        /* first page / drill-in: replace + display immediately; mark the buffer fetched */
        for (int i = 0; i < n && i < YT_CAT_MAX; i++) c->rows[i] = g_search->rows[i];
        c->count = (n > YT_CAT_MAX) ? YT_CAT_MAX : n;
        c->displayed = c->count;
        c->fetched = 1;
    }
    { int i = 0; for (; g_search->continuation[i] && i < YT_SEARCH_CONT_LEN - 1; i++) c->cont[i] = g_search->continuation[i]; c->cont[i] = 0; }
    g_fetch_inflight = 0;

    /* refresh whichever scene is rendering the buffer we just filled */
    if (g_active_results && (YtCatBuf*)g_active_results->result_buf == c) {
        if (is_append) content_show_state(g_active_results);   /* affordances only, no re-measure */
        else           flip_visibility(g_active_results);      /* show the first page */
    }
    yt_pump();   /* deferred category fetch, or top up the buffer, in the background */
}

/* Show `cat` (cached → instant via flip_visibility); otherwise queue its first-page
 * fetch. The pump issues it now if the daemon is free, else after the current one. */
static void yt_request_category(int cat) {
    if (cat < 0 || cat >= YT_CATEGORY_COUNT) return;
    if (g_cat[cat].fetched) return;
    g_pending_category = cat;
    yt_pump();
}

/* Drill into a browseId: reset the drill buffer, remember the header text, queue the
 * /browse (mode 2) for the pump, and navigate to the album scene. Its DONE fills
 * g_browse and refreshes the scene (its result_buf points at g_browse). Bumping the
 * generation makes any search fetch still in flight be ignored when it lands. */
static void yt_browse_submit(const char* browse_id, const wchar_t* title, const wchar_t* subtitle) {
    if (!g_search || !g_wake || !browse_id || !browse_id[0]) return;
    g_browse.count = 0; g_browse.displayed = 0; g_browse.fetched = 0; g_browse.cont[0] = 0;
    { int i = 0; for (; browse_id[i] && i < YT_SEARCH_ID_LEN - 1; i++) g_pending_browse[i] = browse_id[i]; g_pending_browse[i] = 0; }
    { int i = 0; if (title)    for (; title[i]    && i < YT_SEARCH_TITLE_LEN  - 1; i++) g_browse_title[i]    = title[i];    g_browse_title[i] = 0; }
    { int i = 0; if (subtitle) for (; subtitle[i] && i < YT_SEARCH_ARTIST_LEN - 1; i++) g_browse_subtitle[i] = subtitle[i]; g_browse_subtitle[i] = 0; }
    g_query_gen++;
    yt_pump();
    nav_to_scene_by_name(L"YtAlbum.xur");
}

/* Request an artist tab's data (0=albums, 1=songs). Cached → the content scene shows
 * it immediately; otherwise queue the fetch (the pump issues it, and its DONE refreshes
 * the scene). Both tabs share one cached /browse on the daemon side. */
static void yt_request_artist(int tab) {
    if (tab < 0 || tab >= YT_ARTIST_TABS) return;
    if (g_artist[tab].fetched) return;
    g_pending_artist = tab;
    yt_pump();
}

/* Drill into an artist: reset both tab buffers, remember the artist id + name, navigate
 * to the artist twist scene. Its first tab (albums) requests on init; the songs tab
 * requests when shown (served from the daemon's cached page). */
static void yt_artist_submit(const char* browse_id, const wchar_t* name) {
    if (!g_search || !g_wake || !browse_id || !browse_id[0]) return;
    for (int t = 0; t < YT_ARTIST_TABS; t++) {
        g_artist[t].count = 0; g_artist[t].displayed = 0; g_artist[t].fetched = 0; g_artist[t].cont[0] = 0;
    }
    { int i = 0; for (; browse_id[i] && i < YT_SEARCH_ID_LEN - 1; i++) g_artist_browse_id[i] = browse_id[i]; g_artist_browse_id[i] = 0; }
    { int i = 0; if (name) for (; name[i] && i < YT_SEARCH_TITLE_LEN - 1; i++) g_artist_name[i] = name[i]; g_artist_name[i] = 0; }
    g_pending_artist = -1;
    g_query_gen++;
    nav_to_scene_by_name(L"YtArtist.xur");
}

/* Redirected coredll!MsgWaitForMultipleObjectsEx: append the daemon's DONE event
 * to the pump's wait set so completion is delivered on the UI thread, then remap
 * the result so the firmware's own dispatch is byte-identical (mirrors the modkit
 * mods_state_event consumer). */
static DWORD WINAPI MsgWait_proxy(DWORD count, const HANDLE* handles,
                                  DWORD ms, DWORD mask, DWORD flags) {
    HANDLE local[64];
    if (g_orig_wait == NULL) return WAIT_FAILED;
    if (g_done == NULL || count == 0 || count >= 63 || (flags & MWMO_WAITALL))
        return g_orig_wait(count, handles, ms, mask, flags);

    for (DWORD i = 0; i < count; i++) local[i] = handles[i];
    local[count] = g_done;

    DWORD r = g_orig_wait(count + 1, local, ms, mask, flags);

    if (r == WAIT_OBJECT_0 + count) {            /* DONE fired */
        yt_search_snapshot();
        return WAIT_TIMEOUT;                       /* loop re-pumps; nothing of its own */
    }
    if (r == WAIT_OBJECT_0 + count + 1)           /* message pseudo-handle, shifted +1 */
        return WAIT_OBJECT_0 + count;             /* what the loop expects for "message" */
    return r;
}

static void yt_search_install(void) {
    g_search = map_search_section();
    g_wake   = CreateEventW(NULL, FALSE, FALSE, YT_SEARCH_WAKE_EVENT);   /* auto-reset */
    g_done   = CreateEventW(NULL, FALSE, FALSE, YT_SEARCH_DONE_EVENT);   /* auto-reset */
    if (!g_search) L("yt_search: section map failed");
    if (!g_done) { L("yt_search: DONE event create failed"); return; }

    DWORD orig = 0;
    __try {
        orig = *(volatile DWORD*)MSGWAIT_IAT_GEMSTONE;
        g_orig_wait = (MsgWaitFn)orig;   /* set before redirect: a call landing on the
                                            proxy mid-install must already see the real fn */
        *(volatile DWORD*)MSGWAIT_IAT_GEMSTONE = (DWORD)&MsgWait_proxy;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("yt_search: MsgWait IAT patch faulted");
        return;
    }
    Lx("yt_search installed orig", (HRESULT)orig);
}

/* ════════════════════════════ GemYtSearchScene ════════════════════════════
 * Host scene cloned from MarketplaceSearch (searchTerm + twist + breadcrumb).
 * Registered with parent_name="GemLibraryBaseScene" so the engine creates the
 * linked content-swap helper: on a twist tab commit the helper sends us
 * MSG_CONTENT_LOAD (0x1800001c); we create YtSearchContent.xur by name with the
 * tab index as sub_idx and the helper AddChild-adopts + animates it. The twist
 * labels are served as literal rows (RAW_ROW_LABEL), same as our list; no
 * Strings.xus ids needed. Mirrors gem_mod_manager.cpp (device-validated). */

#define MSG_CONTENT_LOAD  0x1800001c

struct GemYtSearchInstance {
    DWORD vtable;            /* +0x00 */
    DWORD scene_handle;      /* +0x04 */
    DWORD breadcrumb_elem;   /* +0x08: XuiScene base reads for back-nav */
    DWORD reserved_0c[4];    /* +0x0c..+0x18: base writes */
    DWORD nav_source_elem;   /* +0x1c: returned by msg 0x18000022 */
    DWORD reserved_20[2];    /* +0x20..+0x24 */
    DWORD twist_elem;        /* +0x28 */
    DWORD searchTerm_elem;   /* +0x2c */
    DWORD content_sub_idx;   /* +0x30: backing for the &sub_idx passed to content create */
    DWORD reserved_34[7];    /* +0x34..+0x4c: pad to 0x50 */
};

static HRESULT GemYtSearch_OnInit(GemYtSearchInstance* self) {
    if (!self) return -1;
    self->breadcrumb_elem = 0; self->nav_source_elem = 0;
    self->twist_elem = 0; self->searchTerm_elem = 0; self->content_sub_idx = 0;
    void* h = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &h, 0);
    self->breadcrumb_elem = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"twist", &h, 0);
    self->twist_elem = (DWORD)h;
    self->nav_source_elem = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"searchTerm", &h, 0);
    self->searchTerm_elem = (DWORD)h;
    return 0;
}

static HRESULT GemYtSearch_OnMessage(GemYtSearchInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    DWORD sub_code = 0, target = 0;
    DataSourceSubStruct* sub = NULL;
    if (msg_id == MSG_DATA_SOURCE) {
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    /* Set the search-term label from the active query, then fall through so the
     * inherited helper runs its own msg=0x13 (which loads the first content tab). */
    if (msg_id == MSG_INIT_BIND) {
        if (self->searchTerm_elem && g_search) {
            wchar_t label[YT_SEARCH_QUERY_LEN + 16];
            const wchar_t* pfx = L"search: "; int o = 0;
            while (pfx[o]) { label[o] = pfx[o]; o++; }
            __try {
                int i = 0;
                while (g_search->query[i] && o < (int)(sizeof(label)/sizeof(label[0])) - 1)
                    label[o++] = g_search->query[i++];
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            label[o] = 0;
            __try { SET_LABEL_TEXT((void*)self->searchTerm_elem, label); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    /* Twist tab labels: literal rows via the same data-source protocol the list uses. */
    if (msg_id == MSG_DATA_SOURCE && sub && target == self->twist_elem) {
        if (sub_code == SUB_DS_COUNT) {
            __try { *((DWORD*)(sub->output_area + 4)) = (DWORD)YT_CATEGORY_COUNT; m[2] = 1; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (sub_code == SUB_DS_GET_ITEM) {
            DWORD* output = NULL; int idx = -1; DWORD out_8 = 0, out_c = 0;
            __try {
                output = (DWORD*)sub->output_area;
                if (output) { idx = (int)output[0]; out_8 = output[2]; out_c = output[3]; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (!output || idx < 0 || idx >= YT_CATEGORY_COUNT) return 0;
            HRESULT hr = -1;
            __try { hr = RAW_ROW_LABEL(out_8, out_c, YT_CATEGORIES[idx]); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
            if (hr >= 0) { __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            return 0;
        }
    }

    /* Content-swap: the helper asks us to create the content scene for the tapped
     * tab. We create YtSearchContent.xur by name with the tab idx as sub_idx;
     * the helper adopts the returned handle. */
    if (msg_id == MSG_CONTENT_LOAD) {
        DWORD* payload = NULL;
        __try { payload = (DWORD*)m[4]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (payload) {
            int idx = 0;
            __try { idx = (int)payload[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (idx < 0) idx = 0;
            if (idx >= YT_CATEGORY_COUNT) idx = YT_CATEGORY_COUNT - 1;
            self->content_sub_idx = (DWORD)idx;
            /* per-category list visual: songs = 2-line; albums/playlists = thumbnail
             * rows; artists = single line. The content class is shared; only the .xur
             * (and its list Visual) differs. */
            const wchar_t* content;
            switch (idx) {
                case 1:  content = L"YtSearchAlbums.xur";  break;  /* albums */
                case 2:  content = L"YtSearchArtists.xur"; break;  /* artists */
                case 3:  content = L"YtSearchAlbums.xur";  break;  /* playlists reuse album rows */
                default: content = L"YtSearchSongs.xur";   break;  /* songs */
            }
            void* hScene = NULL;
            __try {
                XUI_SCENE_CREATE(L"gem://", content, &self->content_sub_idx, &hScene);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            __try { payload[1] = (DWORD)hScene; m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    /* nav-source query: return the selected twist row element for the morph. */
    if (msg_id == MSG_NAV_SOURCE_QUERY) {
        __try {
            DWORD* payload = (DWORD*)m[4];
            if (payload) { GET_SELECTED_IDX((void*)self->twist_elem, (int*)payload); m[2] = 1; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    HRESULT hr = 0;
    __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}

/* ═══════════════════════════════ GemYtArtist ══════════════════════════════
 * Artist drill, cloned from the search host (twist + content-swap helper, parent
 * GemLibraryBaseScene). Two tabs (Albums, Songs) swapped as content scenes
 * (YtArtistAlbums.xur / YtArtistSongs.xur, class GemYtArtistContent). The header
 * shows the artist name. Reuses GemYtSearchInstance (same host layout). */

static const wchar_t* const YT_ARTIST_TAB_LABELS[] = { L"albums", L"songs" };

static HRESULT GemYtArtist_OnMessage(GemYtSearchInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    DWORD sub_code = 0, target = 0;
    DataSourceSubStruct* sub = NULL;
    if (msg_id == MSG_DATA_SOURCE) {
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    /* header label = the artist name (then fall through so the helper loads tab 0) */
    if (msg_id == MSG_INIT_BIND) {
        if (self->searchTerm_elem && g_artist_name[0]) {
            __try { SET_LABEL_TEXT((void*)self->searchTerm_elem, g_artist_name); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    /* twist tab labels (Albums, Songs) */
    if (msg_id == MSG_DATA_SOURCE && sub && target == self->twist_elem) {
        if (sub_code == SUB_DS_COUNT) {
            __try { *((DWORD*)(sub->output_area + 4)) = (DWORD)YT_ARTIST_TABS; m[2] = 1; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (sub_code == SUB_DS_GET_ITEM) {
            DWORD* output = NULL; int idx = -1; DWORD out_8 = 0, out_c = 0;
            __try {
                output = (DWORD*)sub->output_area;
                if (output) { idx = (int)output[0]; out_8 = output[2]; out_c = output[3]; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (!output || idx < 0 || idx >= YT_ARTIST_TABS) return 0;
            HRESULT hr = -1;
            __try { hr = RAW_ROW_LABEL(out_8, out_c, YT_ARTIST_TAB_LABELS[idx]); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
            if (hr >= 0) { __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            return 0;
        }
    }

    /* content-swap: create the tab's content scene by name */
    if (msg_id == MSG_CONTENT_LOAD) {
        DWORD* payload = NULL;
        __try { payload = (DWORD*)m[4]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (payload) {
            int idx = 0;
            __try { idx = (int)payload[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (idx < 0) idx = 0;
            if (idx >= YT_ARTIST_TABS) idx = YT_ARTIST_TABS - 1;
            self->content_sub_idx = (DWORD)idx;
            const wchar_t* content = (idx == 1) ? L"YtArtistSongs.xur" : L"YtArtistAlbums.xur";
            void* hScene = NULL;
            __try { XUI_SCENE_CREATE(L"gem://", content, &self->content_sub_idx, &hScene); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            __try { payload[1] = (DWORD)hScene; m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    if (msg_id == MSG_NAV_SOURCE_QUERY) {
        __try {
            DWORD* payload = (DWORD*)m[4];
            if (payload) { GET_SELECTED_IDX((void*)self->twist_elem, (int*)payload); m[2] = 1; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    HRESULT hr = 0;
    __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}

/* ════════════════════════════ GemYtArtistContent ═══════════════════════════
 * An artist tab's list (albums or songs). Reuses GemYtResults_OnInit (element
 * lookups) + yt_list_pull (row rendering + tap → drill/play); only the buffer
 * binding differs: tab 0 → g_artist[0] (albums), tab 1 → g_artist[1] (songs). */
static HRESULT GemYtArtistContent_OnMessage(GemYtResultsInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    DataSourceSubStruct* sub = NULL; DWORD target = 0;
    if (msg_id == MSG_DATA_SOURCE) {
        __try { sub = (DataSourceSubStruct*)m[4]; if (sub) target = sub->target_elem; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (msg_id == MSG_INIT_BIND) {
        DWORD tab = 0;
        __try { DWORD** outer = (DWORD**)m[4]; if (outer && *outer) tab = **outer; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (tab >= YT_ARTIST_TABS) tab = 0;
        self->result_buf = (DWORD)&g_artist[tab];   /* this tab renders its artist buffer */
        yt_request_artist((int)tab);
        flip_visibility(self);
        __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }
    if (msg_id == MSG_DETACHED) {
        DWORD ph = 0; __try { DWORD* p=(DWORD*)m[4]; if(p) ph=p[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ph == 1 && g_active_results == self) g_active_results = NULL;
    }
    if (msg_id == MSG_DATA_SOURCE && sub && target == self->list_element) {
        if (yt_list_pull(self, msg, sub, scene_buf(self))) return 0;
    }
    if (msg_id == MSG_NAV_SOURCE_QUERY) {
        __try { DWORD* p = (DWORD*)m[4]; if (p) p[0] = self->nav_source_elem; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    HRESULT hr = 0;
    __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}

/* ════════════════════════ class registration (C) ══════════════════════════ */

static DWORD g_hub_vtable[4];
static DWORD g_results_vtable[4];
static DWORD g_search_vtable[4];
static DWORD g_album_vtable[4];
static DWORD g_artisthost_vtable[4];
static DWORD g_artistcontent_vtable[4];

typedef void* (*AllocFn)(SIZE_T);
static HRESULT yt_make(SIZE_T size, DWORD* vtable, void* ctx, void** out) {
    *out = NULL;
    void* inst = ((AllocFn)ALLOCATOR)(size);
    if (!inst) return (HRESULT)0x8007000E;
    memset(inst, 0, size);
    ((DWORD*)inst)[0] = (DWORD)vtable;   /* ctor: vtable at +0x00 */
    ((DWORD*)inst)[1] = (DWORD)ctx;      /* scene_handle at +0x04 (load-bearing) */
    *out = inst;
    return 0;
}
/* engine calls factory(ctx in r0, out in r1) */
static HRESULT hub_factory(void* ctx, void** out)     { return yt_make(0x40, g_hub_vtable, ctx, out); }
static HRESULT results_factory(void* ctx, void** out) { return yt_make(0x50, g_results_vtable, ctx, out); }
static HRESULT search_factory(void* ctx, void** out)  { return yt_make(0x50, g_search_vtable, ctx, out); }
static HRESULT album_factory(void* ctx, void** out)   { return yt_make(0x50, g_album_vtable, ctx, out); }
static HRESULT artisthost_factory(void* ctx, void** out)    { return yt_make(0x50, g_artisthost_vtable, ctx, out); }
static HRESULT artistcontent_factory(void* ctx, void** out) { return yt_make(0x50, g_artistcontent_vtable, ctx, out); }

typedef HRESULT (*XuiRegisterClassFn)(void* name_field, void* descriptor);

static HRESULT register_scene_class(DWORD* desc, const wchar_t* name, void* factory, DWORD parent_name) {
    HMODULE x = GetModuleHandleW(L"xuidll.dll");
    if (!x) x = LoadLibraryW(L"xuidll.dll");
    if (!x) return (HRESULT)-1;
    XuiRegisterClassFn reg = (XuiRegisterClassFn)GetProcAddress(x, L"XuiRegisterClass");
    if (!reg) return (HRESULT)-1;
    for (int i = 0; i < 11; i++) desc[i] = 0;
    desc[1] = (DWORD)name;                /* +0x04 class name */
    desc[2] = parent_name;                /* +0x08 parent name */
    desc[5] = DESC_DESTRUCTOR;            /* +0x14 */
    desc[6] = (DWORD)factory;             /* +0x18 factory */
    desc[7] = DESC_FINALIZER;             /* +0x1c */
    return reg(&desc[1], &desc[0]);
}

/* descriptors kept static; registration may retain a pointer to the name slot. */
static DWORD g_desc_hub[11];
static DWORD g_desc_results[11];
static DWORD g_desc_search[11];
static DWORD g_desc_album[11];
static DWORD g_desc_artisthost[11];
static DWORD g_desc_artistcontent[11];
static const wchar_t HUB_NAME[]     = L"GemYtHub";
static const wchar_t RESULTS_NAME[] = L"GemYtResultsContentScene";
static const wchar_t SEARCH_NAME[]  = L"GemYtSearchScene";
static const wchar_t ALBUM_NAME[]   = L"GemYtAlbum";
static const wchar_t ARTISTHOST_NAME[]    = L"GemYtArtist";
static const wchar_t ARTISTCONTENT_NAME[] = L"GemYtArtistContent";

extern "C" __declspec(dllexport) int YtInstall(void) {
    L("YtInstall: reached (loaded into gemstone)");

    g_hub_vtable[0] = PARENT_VTABLE_BASE;
    g_hub_vtable[1] = (DWORD)GemYtHub_OnMessage;
    g_hub_vtable[2] = (DWORD)GemYtHub_OnInit;
    g_hub_vtable[3] = CLASS_DESTROY_SHARED;

    g_results_vtable[0] = PARENT_VTABLE_BASE;
    g_results_vtable[1] = (DWORD)GemYtResults_OnMessage;
    g_results_vtable[2] = (DWORD)GemYtResults_OnInit;
    g_results_vtable[3] = CLASS_DESTROY_SHARED;

    g_search_vtable[0] = PARENT_VTABLE_BASE;
    g_search_vtable[1] = (DWORD)GemYtSearch_OnMessage;
    g_search_vtable[2] = (DWORD)GemYtSearch_OnInit;
    g_search_vtable[3] = CLASS_DESTROY_SHARED;

    g_album_vtable[0] = PARENT_VTABLE_BASE;
    g_album_vtable[1] = (DWORD)GemYtAlbum_OnMessage;
    g_album_vtable[2] = (DWORD)GemYtAlbum_OnInit;
    g_album_vtable[3] = CLASS_DESTROY_SHARED;

    /* artist host reuses GemYtSearch_OnInit (same host layout: breadcrumb/twist/searchTerm);
     * artist content reuses GemYtResults_OnInit (same element lookups). */
    g_artisthost_vtable[0] = PARENT_VTABLE_BASE;
    g_artisthost_vtable[1] = (DWORD)GemYtArtist_OnMessage;
    g_artisthost_vtable[2] = (DWORD)GemYtSearch_OnInit;
    g_artisthost_vtable[3] = CLASS_DESTROY_SHARED;

    g_artistcontent_vtable[0] = PARENT_VTABLE_BASE;
    g_artistcontent_vtable[1] = (DWORD)GemYtArtistContent_OnMessage;
    g_artistcontent_vtable[2] = (DWORD)GemYtResults_OnInit;
    g_artistcontent_vtable[3] = CLASS_DESTROY_SHARED;

    HRESULT h1 = register_scene_class(g_desc_hub, HUB_NAME, (void*)hub_factory, GEMBASESCENE_NAME);
    Lx("GemYtHub register", h1);                 /* 0=ok, 0x80300005=already */
    HRESULT h2 = register_scene_class(g_desc_results, RESULTS_NAME, (void*)results_factory, GEMBASESCENE_NAME);
    Lx("GemYtResultsContentScene register", h2);
    HRESULT h3 = register_scene_class(g_desc_search, SEARCH_NAME, (void*)search_factory, GEMLIBRARYBASESCENE_NAME);
    Lx("GemYtSearchScene register", h3);
    HRESULT h4 = register_scene_class(g_desc_album, ALBUM_NAME, (void*)album_factory, GEMBASESCENE_NAME);
    Lx("GemYtAlbum register", h4);
    HRESULT h5 = register_scene_class(g_desc_artisthost, ARTISTHOST_NAME, (void*)artisthost_factory, GEMLIBRARYBASESCENE_NAME);
    Lx("GemYtArtist register", h5);
    HRESULT h6 = register_scene_class(g_desc_artistcontent, ARTISTCONTENT_NAME, (void*)artistcontent_factory, GEMBASESCENE_NAME);
    Lx("GemYtArtistContent register", h6);

    yt_search_install();
    return 0;
}

extern "C" BOOL WINAPI DllMain(HANDLE h, DWORD r, LPVOID l) { (void)h; (void)r; (void)l; return TRUE; }
