#include "castaudio.h"
#include "zdk.h"

// Signatures validated against zdksystem v4.5 (plugin-zdk-probe); see
// wiki/architecture/zdk-media-api.md. Every accessor returns 0 on success.
typedef int (*FN_init)(void);
typedef int (*FN_shutdown)(void);
typedef int (*FN_q_getidx)(int* out);
typedef int (*FN_q_getsong)(int idx, void** out_song);
typedef int (*FN_q_getpos)(int* out_ms);
typedef int (*FN_q_setstate)(int state);
typedef int (*FN_item_getname)(void* item, wchar_t* buf, int max_chars);
typedef int (*FN_song_getrel)(void* song, void** out_handle);
typedef int (*FN_q_getcount)(int* out);
typedef int (*FN_song_geti32)(void* song, int* out);
typedef int (*FN_album_geti32)(void* album, int* out);
typedef int (*FN_item_getpath)(void* item, wchar_t* buf, int max_chars);
// Volume is a float 0.0-1.0, not an int; the wiki's int signature is wrong
// (device-validated: GetVolume returns 0x3F800000 = 1.0f at full). The CE ARM
// soft-float ABI passes the float in r0, so plain float typedefs are correct.
typedef int (*FN_q_getvol)(float* out);
typedef int (*FN_q_setvol)(float vol);
// ZDKSystem media-notification queue: GetNotifyHandle(&h) opens a cross-process
// MsgQueue (0 on success); GetNextNotification(h, buf) non-blocking-reads one
// 12-byte record (3 u32; buf[0]=type code), 0 if a record was read.
typedef int (*FN_zsys_getnotify)(void** out_h);
typedef int (*FN_zsys_getnext)(void* h, void* buf12);
typedef int (*FN_zsys_closenotify)(void* h);
// ZDKDisplay_Initialize is mislabeled: its body IS the media notify-init (mode 1).
// It registers the 0x218 callback via the ZAM0:/0x204 ZDKApp broker, creates the
// republish queue, and subscribes the live ZME0/0x201 session. Calling it once
// (on the draining/pumping thread) arms GetVolume to track the slider.
typedef int (*FN_notify_arm)(void);

static HMODULE         g_zdk = NULL;
static FN_init         pInit = NULL;
static FN_shutdown     pShutdown = NULL;
static FN_q_getidx     pQGetIdx = NULL;
static FN_q_getsong    pQGetSong = NULL;
static FN_q_getpos     pQGetPos = NULL;
static FN_q_setstate   pQSetState = NULL;
static FN_item_getname pItemGetName = NULL;
static FN_song_getrel  pSongGetArtist = NULL;
static FN_song_getrel  pSongGetAlbum = NULL;
static FN_q_getcount   pQGetCount = NULL;
static FN_song_geti32  pSongGetDur = NULL;
static FN_q_setstate   pMoveTo = NULL;
static FN_album_geti32 pAlbumHasArt = NULL;
static FN_item_getpath pItemGetPath = NULL;
static FN_q_getvol     pQGetVol = NULL;
static FN_q_setvol     pQSetVol = NULL;
static FN_zsys_getnotify   pNotifyOpen = NULL;
static FN_zsys_getnext     pNotifyNext = NULL;
static FN_zsys_closenotify pNotifyClose = NULL;
static FN_notify_arm       pNotifyArm = NULL;

static void wide_to_ascii(const wchar_t* src, char* dst, int max)
{
    if (max <= 0) return;
    int i = 0;
    while (i < max - 1 && src[i]) {
        wchar_t c = src[i];
        dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        i++;
    }
    dst[i] = 0;
}

int zdk_init(void)
{
    if (g_zdk) return 1;
    g_zdk = LoadLibraryW(L"zdksystem.dll");
    if (!g_zdk) { cast_log("ZDK load fail %d", GetLastError()); return 0; }

    pInit          = (FN_init)        GetProcAddress(g_zdk, L"ZDKMedia_Initialize");
    pShutdown      = (FN_shutdown)    GetProcAddress(g_zdk, L"ZDKMedia_Shutdown");
    pQGetIdx       = (FN_q_getidx)    GetProcAddress(g_zdk, L"ZDKMedia_Queue_GetActiveSongIndex");
    pQGetSong      = (FN_q_getsong)   GetProcAddress(g_zdk, L"ZDKMedia_Queue_GetSongAtIndex");
    pQGetPos       = (FN_q_getpos)    GetProcAddress(g_zdk, L"ZDKMedia_Queue_GetPlayPosition");
    pQSetState     = (FN_q_setstate)  GetProcAddress(g_zdk, L"ZDKMedia_Queue_SetPlayState");
    pItemGetName   = (FN_item_getname)GetProcAddress(g_zdk, L"ZDKMedia_Item_GetName");
    pSongGetArtist = (FN_song_getrel) GetProcAddress(g_zdk, L"ZDKMedia_Song_GetArtist");
    pSongGetAlbum  = (FN_song_getrel) GetProcAddress(g_zdk, L"ZDKMedia_Song_GetAlbum");
    pQGetCount     = (FN_q_getcount)  GetProcAddress(g_zdk, L"ZDKMedia_Queue_GetSongCount");
    pSongGetDur    = (FN_song_geti32) GetProcAddress(g_zdk, L"ZDKMedia_Song_GetDuration");
    pMoveTo        = (FN_q_setstate)  GetProcAddress(g_zdk, L"ZDKMedia_Queue_MoveTo");
    pAlbumHasArt   = (FN_album_geti32)GetProcAddress(g_zdk, L"ZDKMedia_Album_HasAlbumArt");
    pItemGetPath   = (FN_item_getpath)GetProcAddress(g_zdk, L"ZDKMedia_Item_GetFilePath");
    pQGetVol       = (FN_q_getvol)    GetProcAddress(g_zdk, L"ZDKMedia_Queue_GetVolume");
    pQSetVol       = (FN_q_setvol)    GetProcAddress(g_zdk, L"ZDKMedia_Queue_SetVolume");
    pNotifyOpen    = (FN_zsys_getnotify)  GetProcAddress(g_zdk, L"ZDKSystem_GetNotifyHandle");
    pNotifyNext    = (FN_zsys_getnext)    GetProcAddress(g_zdk, L"ZDKSystem_GetNextNotification");
    pNotifyClose   = (FN_zsys_closenotify)GetProcAddress(g_zdk, L"ZDKSystem_CloseNotifyHandle");
    pNotifyArm     = (FN_notify_arm)      GetProcAddress(g_zdk, L"ZDKDisplay_Initialize");

    if (!pInit || !pQGetIdx || !pQGetSong || !pItemGetName) {
        cast_log("ZDK resolve fail");
        FreeLibrary(g_zdk); g_zdk = NULL;
        return 0;
    }
    int hr = pInit();
    cast_log("ZDK init hr=%d", hr);
    return 1;
}

void zdk_shutdown(void)
{
    if (!g_zdk) return;
    if (pShutdown) pShutdown();
    FreeLibrary(g_zdk);
    g_zdk = NULL;
}

int zdk_active_index(void)
{
    if (!g_zdk || !pQGetIdx) return -1;
    int idx = -1;
    if (pQGetIdx(&idx) != 0) return -1;
    return idx;
}

int zdk_play_position_ms(void)
{
    if (!g_zdk || !pQGetPos) return -1;
    int ms = 0;
    if (pQGetPos(&ms) != 0) return -1;
    return ms;
}

int zdk_set_play_state(int state)
{
    if (!g_zdk || !pQSetState) return -1;
    return pQSetState(state);
}

static void* current_song(void)
{
    int idx = zdk_active_index();
    if (idx < 0) return NULL;
    void* song = NULL;
    if (pQGetSong(idx, &song) != 0) return NULL;
    return song;
}

static void item_name(void* h, char* buf, int sz)
{
    if (!buf || sz <= 0) return;
    buf[0] = 0;
    if (!h || !pItemGetName) return;
    wchar_t w[128]; w[0] = 0;
    if (pItemGetName(h, w, 128) != 0) return;
    wide_to_ascii(w, buf, sz);
}

static void rel_name(FN_song_getrel rel, void* song, char* buf, int sz)
{
    if (buf && sz > 0) buf[0] = 0;
    if (!song || !rel) return;
    void* h = NULL;
    if (rel(song, &h) != 0 || !h) return;
    item_name(h, buf, sz);
}

int zdk_now_playing(char* title, int title_sz, char* artist, int artist_sz,
                    char* album, int album_sz)
{
    if (title && title_sz)  title[0]  = 0;
    if (artist && artist_sz) artist[0] = 0;
    if (album && album_sz)  album[0]  = 0;

    void* song = current_song();
    if (!song) return 0;
    item_name(song, title, title_sz);
    rel_name(pSongGetArtist, song, artist, artist_sz);
    rel_name(pSongGetAlbum,  song, album,  album_sz);
    return title && title[0] ? 1 : (song != NULL);
}

int zdk_queue_count(void)
{
    if (!g_zdk || !pQGetCount) return 0;
    int n = 0;
    if (pQGetCount(&n) != 0) return 0;
    return n;
}

int zdk_queue_track(int idx, char* title, int tsz, char* artist, int asz,
                    char* album, int alsz, int* dur_ms)
{
    if (title && tsz)  title[0]  = 0;
    if (artist && asz) artist[0] = 0;
    if (album && alsz) album[0]  = 0;
    if (dur_ms) *dur_ms = 0;
    if (!g_zdk || !pQGetSong || idx < 0) return 0;
    void* song = NULL;
    if (pQGetSong(idx, &song) != 0 || !song) return 0;
    item_name(song, title, tsz);
    rel_name(pSongGetArtist, song, artist, asz);
    rel_name(pSongGetAlbum,  song, album,  alsz);
    if (dur_ms && pSongGetDur) { int d = 0; if (pSongGetDur(song, &d) == 0) *dur_ms = d; }
    return 1;
}

int zdk_move_to(int idx)
{
    if (!g_zdk || !pMoveTo) return -1;
    return pMoveTo(idx);
}

float zdk_get_volume(void)
{
    if (!g_zdk || !pQGetVol) return -1.0f;
    float v = -1.0f;
    if (pQGetVol(&v) != 0) return -1.0f;
    return v;
}

int zdk_set_volume(float vol)
{
    if (!g_zdk || !pQSetVol) return -1;
    return pQSetVol(vol);
}

int zdk_notify_arm(void)
{
    if (!g_zdk || !pNotifyArm) return -1;
    return pNotifyArm();
}

void* zdk_notify_open(void)
{
    if (!g_zdk || !pNotifyOpen) return NULL;
    void* h = NULL;
    if (pNotifyOpen(&h) != 0) return NULL;
    return h;
}

int zdk_notify_next(void* h, unsigned int buf3[3])
{
    if (!h || !pNotifyNext) return -1;
    return pNotifyNext(h, buf3);   // 0 = a 12-byte record was read; nonzero = empty/error
}

void zdk_notify_close(void* h)
{
    if (h && pNotifyClose) pNotifyClose(h);
}

int zdk_album_art_path(int idx, wchar_t* buf, int max_chars)
{
    if (buf && max_chars > 0) buf[0] = 0;
    if (!g_zdk || !pQGetSong || !pSongGetAlbum || !pItemGetPath || !buf || max_chars <= 0 || idx < 0)
        return 0;
    void* song = NULL;
    if (pQGetSong(idx, &song) != 0 || !song) return 0;
    void* album = NULL;
    if (pSongGetAlbum(song, &album) != 0 || !album) return 0;
    if (pAlbumHasArt) { int has = 0; if (pAlbumHasArt(album, &has) != 0 || !has) return 0; }

    // The art path (zmdb property 0x2000b) is built by the un-exported
    // sub_0x4197b628 (the GetTexture path-builder), reached via a slide from the
    // exported Item_GetFilePath anchor (static VA 0x4197af20). It does its own
    // has-art check and selects 0x2000b; the HasAlbumArt guard above keeps a
    // no-art album from yielding the 0x2000a fallback (album .z). See
    // notes/re-2026-05-30-album-art.
    typedef int (*FN_artpath)(void* album, wchar_t* out, int max);
    unsigned int slide = (unsigned int)pItemGetPath - 0x4197af20u;
    FN_artpath art_path_fn = (FN_artpath)(0x4197b628u + slide);
    int hr = art_path_fn(album, buf, max_chars);
    return (hr >= 0 && buf[0]) ? 1 : 0;
}
