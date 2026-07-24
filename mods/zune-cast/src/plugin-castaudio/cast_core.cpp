#include "castaudio.h"
#include "cast_core.h"
#include "cast_keys.h"   // CAST_STATUS_KEY + status states
#include "cast_channel.h" // cast_channel_set_sublabel (row sub-label on reconnect)
#include "ce_log.h"
#include <stdlib.h>
#include <stdarg.h>

CE_LOGGER_PUBLIC(cast_log, L"\\flash2\\automation\\zune-cast.log")

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
    // session; reconnect until session_stop (toggle off / shutdown). A receiver
    // that is simply off stays unreachable for hours, so a failing attempt backs
    // off and collapses its logging; both reset once a link actually comes up.
    DWORD backoff       = CAST_RETRY_MIN_MS;
    DWORD last_fail_log = 0;
    int   fail_streak   = 0;
    int   last_rc       = 1;                   // no rc yet; any real rc differs
    while (WaitForSingleObject(session_stop, 0) != WAIT_OBJECT_0) {
        // Dialing the receiver; reconcile_thread promotes this to connected/
        // casting once the link is live.
        mod_state_set_status(CAST_STATUS_KEY, CAST_STATUS_CONNECTING);
        int speak = (fail_streak == 0) ||
                    (GetTickCount() - last_fail_log >= CAST_RETRY_LOG_MS);
        int rc = castv2_run(session_stop, target, control_port, media_port,
                            hc.q, hc.ring, speak);
        if (WaitForSingleObject(session_stop, 0) == WAIT_OBJECT_0) break;
        // Returned while still wanted: the connect attempt failed or a live link
        // dropped. Surface error, back off, retry.
        mod_state_set_status(CAST_STATUS_KEY, CAST_STATUS_ERROR);
        if (rc == 0) {
            backoff     = CAST_RETRY_MIN_MS;   // the link was up; treat as a fresh episode
            fail_streak = 0;
            cast_log("CTRL link ended; retry in %lums", backoff);
            last_fail_log = GetTickCount();
        } else {
            fail_streak++;
            if (speak || rc != last_rc) {
                cast_log("CTRL setup failed rc=%d attempts=%d; retry in %lums",
                         rc, fail_streak, backoff);
                last_fail_log = GetTickCount();
            }
        }
        last_rc = rc;
        WaitForSingleObject(session_stop, backoff);
        if (rc != 0 && backoff < CAST_RETRY_MAX_MS) {
            backoff *= 2;
            if (backoff > CAST_RETRY_MAX_MS) backoff = CAST_RETRY_MAX_MS;
        }
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
