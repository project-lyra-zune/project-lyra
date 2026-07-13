#include "castaudio.h"
#include "cast_core.h"
#include "cast_keys.h"   // CAST_STATUS_KEY + status states
#include "cast_channel.h" // cast_channel_set_sublabel (row sub-label on reconnect)
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// Serializes log appends from the capture / HTTP / Cast-I/O / reconcile threads
// so lines don't interleave. cast_log_init() runs before any thread exists.
static CRITICAL_SECTION g_log_cs;
static volatile LONG     g_log_ready = 0;

void cast_log_init(void)
{
    if (InterlockedExchange(&g_log_ready, 1) == 0)
        InitializeCriticalSection(&g_log_cs);
}

void cast_log(const char* fmt, ...)
{
    char body[256];
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    body[n] = '\0';

    if (g_log_ready) EnterCriticalSection(&g_log_cs);
    HANDLE h = CreateFileW(CAST_LOG_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        int ln = _snprintf(line, sizeof(line), "[t=%lu] %s\r\n", GetTickCount(), body);
        if (ln > 0 && ln < (int)sizeof(line)) {
            DWORD w;
            WriteFile(h, line, (DWORD)ln, &w, NULL);
        }
        CloseHandle(h);
    }
    if (g_log_ready) LeaveCriticalSection(&g_log_cs);
}

int cast_run_session(const char* target, unsigned short control_port,
                     unsigned short media_port, HANDLE session_stop)
{
    CastQueue cq;
    memset(&cq, 0, sizeof(cq));
    cq.serving_index = -1; cq.device_index = -1;
    cq.consume_cursor = -1; cq.aligned_epoch = 0;

    HttpMediaCtx hc;
    hc.stop_event = session_stop;
    hc.port       = media_port;
    hc.wav        = NULL;
    hc.wav_len    = 0;
    hc.ring       = NULL;
    hc.q          = NULL;

#if CAST_USE_TEST_TONE
    int wav_len = 0;
    unsigned char* wav = castaudio_build_test_wav(&wav_len);
    if (wav == NULL) { cast_log("WAV-ALLOC-FAIL"); return -1; }
    hc.wav = wav; hc.wav_len = wav_len;
    HANDLE hcap = NULL;
    unsigned char* ringbuf = NULL;
#else
    unsigned char* ringbuf = (unsigned char*)malloc(CAP_RING_SLOTS * CAP_SLOT_SIZE);
    if (ringbuf == NULL) { cast_log("RING-ALLOC-FAIL"); return -1; }
    CaptureRing ring;
    ring.buf = ringbuf; ring.produced = 0; ring.established = 0;
    ring.track_epoch = 0; ring.epoch_produced = 0;
    ring.sample_rate = 0;                  // 0 = unmeasured; serve waits for the first CAR read before tagging
    ring.rate_dirty = 0;
    ring.stop_event = session_stop;
    hc.ring = &ring;
    hc.q    = &cq;
    HANDLE hcap = CreateThread(NULL, 0, avp_capture_thread, &ring, 0, NULL);
#endif

    HANDLE hhttp = CreateThread(NULL, 0, http_media_thread, &hc, 0, NULL);

    cast_log("SESSION-START target=%s:%u media_port=%u tone=%d",
             target, control_port, media_port, CAST_USE_TEST_TONE);

    // The control client owns one wolfSSL lifetime across reconnects within the
    // session; reconnect until session_stop (toggle off / shutdown).
    while (WaitForSingleObject(session_stop, 0) != WAIT_OBJECT_0) {
        // Dialing the receiver; reconcile_thread promotes this to connected/
        // casting once the link is live.
        mod_state_set_status(CAST_STATUS_KEY, CAST_STATUS_CONNECTING);
        int rc = castv2_run(session_stop, target, control_port, media_port, hc.q, hc.ring);
        if (WaitForSingleObject(session_stop, 0) == WAIT_OBJECT_0) break;
        // Returned while still wanted: the connect attempt failed or a live link
        // dropped. Surface error, back off, retry.
        mod_state_set_status(CAST_STATUS_KEY, CAST_STATUS_ERROR);
        cast_log("CTRL ended rc=%d; retry in 3s", rc);
        WaitForSingleObject(session_stop, 3000);
    }

    if (hhttp) { WaitForSingleObject(hhttp, 3000); CloseHandle(hhttp); }
#if CAST_USE_TEST_TONE
    free(wav);
#else
    if (hcap) { WaitForSingleObject(hcap, 3000); CloseHandle(hcap); }
    free(ringbuf);
#endif
    cast_log("SESSION-EXIT target=%s", target);
    return 0;
}
