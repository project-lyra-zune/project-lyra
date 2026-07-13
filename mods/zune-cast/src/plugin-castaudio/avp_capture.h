#ifndef AVP_CAPTURE_H
#define AVP_CAPTURE_H

#include <windows.h>

// One decoded PCM block = 8 KB = ~42.7 ms @ 48000/16/stereo (the AVP/I2S block
// granularity; ch15 AHB_PTR advances one block per EOC).
#define CAP_SLOT_SIZE   0x2000u
// Cursor-drift threshold (blocks) past which a real track boundary triggers a
// self-correcting snap to the new track's head: bounds any glitch (e.g. an
// overrun drop) to a single track instead of permanently offsetting the queue.
// ~64 blocks ≈ 2.7 s: far above clean per-track drift, far below a real glitch.
#define CAP_SELFCORRECT_DRIFT  64
// ~11 s of headroom: the consumer starts each new track at the epoch's head
// block, which must still be in the ring when the receiver requests the next
// item, so the ring must outlast the end-to-end cast latency. The receiver's
// jitter buffer alone runs ~9 s (measured), and a long inter-item request gap at
// 128 slots (~5.5 s) clamped, dropping a track's head; 256 covers it.
#define CAP_RING_SLOTS  256u

// Shared single-producer (capture thread) / single-consumer (HTTP thread)
// ring of decoded PCM blocks. `produced` is the only synchronization: the
// producer fills buf[(produced % N)] then InterlockedIncrement(&produced);
// the consumer reads slots in [its cursor, produced).
typedef struct {
    unsigned char* buf;            // CAP_RING_SLOTS * CAP_SLOT_SIZE, caller-owned
    volatile LONG  produced;       // total blocks captured (monotonic)
    volatile LONG  established;    // 1 once ch15 + the block span are resolved
    // Decoder track boundary, exposed to the consumer so each Cast queue item
    // streams exactly its track's PCM: `track_epoch` increments each time the
    // PCM span is (re)acquired (a track change reallocs the AVP block list);
    // `epoch_produced` is the `produced` index of that epoch's first block.
    volatile LONG  track_epoch;
    volatile LONG  epoch_produced;
    // Live sample rate of the ch15 I2S stream, read from the Tegra CAR I2S clock.
    // The I2S is clocked at the *source* track's rate (NOT a fixed 48 kHz), so this
    // varies per track and the WAV header / Content-Length must follow it or
    // playback is pitched.
    volatile LONG  sample_rate;
    // Set by the reconcile thread on a track change, cleared by the capture
    // thread after re-reading CAR. Replaces a per-block CAR read: the rate is
    // re-measured only at span (re)acquisition and on a signalled track change,
    // not every captured block.
    volatile LONG  rate_dirty;
    HANDLE         stop_event;
} CaptureRing;

// Consumer-paced AVP tee (ported from nativeapp op_diag op23): reads decoded
// PCM blocks behind the ch15 DMA read pointer via kerncore, into the ring.
// Runs until stop_event. arg = CaptureRing*.
DWORD WINAPI avp_capture_thread(LPVOID arg);

#endif // AVP_CAPTURE_H
