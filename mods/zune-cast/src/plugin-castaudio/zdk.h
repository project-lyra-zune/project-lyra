#ifndef ZDK_H
#define ZDK_H

#include <windows.h>

// Trimmed binding over zdksystem.dll's ZDKMedia_* native API (signatures
// mirror the device-validated set in payloads/replacement/zpod-iap1-svc/
// svc_zdk.cpp). Resolved once via LoadLibraryW + GetProcAddress; usable from
// any user-mode process, for now-playing metadata and (cast-side) play/pause
// control of the live queue.

int  zdk_init(void);                 // 1 on success
void zdk_shutdown(void);             // idempotent

int  zdk_active_index(void);         // active queue index, -1 on failure
// Live play position in ms (ZME0-backed). Advancing between polls = playing,
// frozen = paused. The correct play-state signal: Queue_GetPlayState reads a
// stale cached field and must not be used.
int  zdk_play_position_ms(void);     // -1 on failure
int  zdk_set_play_state(int state);  // 1=play, 2=pause, 3=stop (live ZME0 0x29/0x28/0x25)

// Now-playing queue enumeration + navigation (for the Cast queue mirror).
int  zdk_queue_count(void);          // # tracks in the now-playing queue, 0 on failure
// Per-queue-index metadata: fills ASCII title/artist/album + *dur_ms (zunedb). Returns 1 if resolved.
int  zdk_queue_track(int idx, char* title, int tsz, char* artist, int asz,
                     char* album, int alsz, int* dur_ms);
int  zdk_move_to(int idx);           // jump active queue index (ZDKMedia_Queue_MoveTo); 0 on success

// Playback volume as a float 0.0-1.0 (device-validated; maps 1:1 onto the Cast
// receiver's volume.level). -1.0f on failure. Set returns 0 on success.
// SetVolume drives the real device slider; GetVolume is a stale per-process
// cache (refreshed only by this process's own Set or by draining the notify
// queue below), so use the notification stream to track external changes.
float zdk_get_volume(void);
int   zdk_set_volume(float vol);

// ZDKSystem media-notification queue. open() returns a drainable MsgQueue handle
// (NULL on failure); next() non-blocking-reads one 12-byte record (3 u32s,
// buf3[0]=type code) returning 0 if a record was read, nonzero when empty.
// Arms the notify subscription (ZDKDisplay_Initialize, mislabeled; its body is
// the mode-1 media notify-init). Must run on the thread that pumps messages, so
// the 0x218 callback that freshens the GetVolume/GetPlayState cache is delivered
// there. 0 on success.
int   zdk_notify_arm(void);
void* zdk_notify_open(void);
int   zdk_notify_next(void* h, unsigned int buf3[3]);
void  zdk_notify_close(void* h);

// Fills ASCII title/artist/album of the currently-playing song. Any buffer may
// be NULL. Returns 1 if a song is playing (title resolved), 0 otherwise.
int  zdk_now_playing(char* title,  int title_sz,
                     char* artist, int artist_sz,
                     char* album,  int album_sz);

// Fills the album-art JPEG file path (\Flash2\Content\<bucket>\00\art\<atom>_art.jpg)
// for the song at queue index `idx`. Returns 1 if the album has art and a path was
// resolved, 0 otherwise. Device-validated: art is property 0x2000b, fetched via the
// firmware's string-property getter (see notes/re-2026-05-30-album-art). The texture
// API (Album_GetTexture/GetThumbnail) needs the GL subsystem and fails headless.
int zdk_album_art_path(int idx, wchar_t* buf, int max_chars);

#endif // ZDK_H
