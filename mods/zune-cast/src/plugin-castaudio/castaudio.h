#ifndef CASTAUDIO_H
#define CASTAUDIO_H

// Winsock2 must precede windows.h on Windows CE.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "avp_capture.h"

// 1 = serve a generated 440 Hz tone (PoC 0 known-good fallback, no capture).
// 0 = stream the live AVP PCM capture. Flip to 1 to bisect cast-path issues
//     independent of the capture path.
#define CAST_USE_TEST_TONE        0

#define CAST_CONTROL_PORT         8009             // CASTV2 control channel (TLS)
#define CAST_MEDIA_PORT_DEFAULT   8010             // on-device HTTP media server (audio)
#define CAST_RECEIVER_APP_ID      "CC1AD845"       // Default Media Receiver
#define CAST_LOG_PATH             L"\\flash2\\automation\\zune-cast.log"

// Live audio is tagged at the PER-TRACK rate read from CAR (CaptureRing.sample_rate
// is 44.1k OR 48k per the source track, never fixed; tagging the wrong rate plays
// the whole track pitched). CAST_SAMPLE_RATE is only the 440 Hz test-tone rate
// (castaudio_build_test_wav) and the no-audio-timeout fallback when no CAR read has
// landed; it is never the live-path default.
#define CAST_SAMPLE_RATE          48000
#define CAST_CHANNELS             2
#define CAST_BITS                 16

// Shared Cast-queue state between the reconcile thread (owns ZDKMedia: builds the
// queue, mirrors receiver-initiated skips onto the device via MoveTo) and the HTTP
// thread (serves per item, reports the requested index in `serving_index`). Single
// writer per field by role. Except `serving_index`, the receiver/device rendezvous
// both threads write; each write is atomic, so plain volatiles suffice.
#define CAST_MAX_Q 10               // queue window size (also bounds the QUEUE_LOAD message)
// An active-track advance whose outgoing track was within this of its end is a
// natural track end (the receiver auto-advances on the per-item WAV's own EOF);
// anything earlier is a deliberate skip the reconcile thread reloads to mirror.
// Sized to absorb the ~200 ms reconcile poll plus position-read lag.
#define CAST_NATURAL_END_GAP_MS 2000
// After a device->cast SET_VOLUME, ignore the receiver's echo for this long so it
// can't bounce the device back a step during rapid on-device volume changes.
#define CAST_VOL_ECHO_MS 700
typedef struct {
    volatile LONG serving_index;    // Zune index the receiver last requested (-1 = none)
    volatile LONG device_index;     // reconcile->HTTP: device's active queue index (-1 = unknown)
    volatile LONG consume_cursor;   // continuous ring read position across items (-1 = (re)init at next serve)
    volatile LONG aligned_epoch;    // capture epoch the cursor was last known aligned to (re-anchor guard)
    volatile LONG generation;       // bumped each QUEUE_LOAD; stamped into the contentId path (/g<gen>/q<idx>.wav). A GET for a stale generation (a leftover session's in-flight request) is 404'd.
    int  base;                      // Zune index of window item 0
    int  count;                     // # items in the window
    int  dur_ms[CAST_MAX_Q];        // per-item duration in ms, indexed by (zune_index - base)
    wchar_t art[CAST_MAX_Q][128];   // per-item album-art file path ("" = none); reconcile writes, HTTP serves
} CastQueue;

// Context for the HTTP media thread. In live mode `ring` is set and `wav`
// is NULL; in test-tone mode the reverse.
typedef struct {
    HANDLE               stop_event;
    unsigned short       port;
    const unsigned char* wav;       // test-tone mode: finite WAV
    int                  wav_len;
    CaptureRing*         ring;       // live mode: PCM block ring
    CastQueue*           q;          // live mode: Cast-queue mirror (NULL in tone mode)
} HttpMediaCtx;

void           cast_log(const char* fmt, ...);

// Writes a 44-byte canonical PCM WAV header for `data_len` bytes of audio at
// `sample_rate` Hz (stereo 16-bit). Rate is passed (not constant) because the
// live tap rate follows the source track; see CaptureRing.sample_rate.
void           castaudio_build_wav_header(unsigned char* hdr44, unsigned int data_len,
                                          unsigned int sample_rate);

// Generates a finite 440 Hz test tone as an in-memory WAV (test-tone mode).
// Caller frees with free(). Returns NULL on allocation failure.
unsigned char* castaudio_build_test_wav(int* out_len);

DWORD WINAPI   http_media_thread(LPVOID arg);   // arg = HttpMediaCtx*

// Connects the CASTV2 control client, launches the receiver, points it at
// http://<local-ip>:<media_port>/a.wav, services heartbeats until stop_event.
// Returns 0 on clean stop, negative on a setup/connection error (caller may
// retry). wolfSSL_Init/Cleanup are owned by the caller.
int            castv2_run(HANDLE stop_event, const char* target_ip,
                          unsigned short control_port, unsigned short media_port,
                          CastQueue* q, CaptureRing* ring);

// On-screen device volume, published by the servicesd ZAM-writer detour
// (zuxhook mods_volume_state) into a named shared section. The reconcile thread
// pulls the latest on its tick and mirrors it to the receiver (SET_VOLUME).
// Cross-process ABI: MUST match payloads/replacement/zuxhook/mods_volume_state.h.
#define VOLUME_STATE_SECTION  L"zune-volume-state"
typedef struct {
    unsigned long version;   // 1 once initialised
    unsigned long seq;       // incremented on every change
    unsigned long vol;       // current on-screen volume, 0..max
    unsigned long max;       // full-scale (30 on Zune HD)
} VolumeStateBlock;

#endif // CASTAUDIO_H
