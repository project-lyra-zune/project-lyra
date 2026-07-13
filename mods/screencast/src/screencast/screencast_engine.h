/* screencast_engine: the device-validated screen-capture + touch-injection core,
 * shared by both frontends (MJPEG/HTTP for browsers, the binary delta protocol
 * for zune-screencast.py).
 *
 * Capture reads the live display front buffer from the NvRm carveout via
 * kerncore's PL1 kernel-memcpy; touch injection writes native type-3 records
 * into servicesd's ZAM input ring and signals gemstone's input thread. Both
 * paths are device-validated; see notes/re-2026-05-28-screencap and
 * notes/re-2026-05-29-zam-input-inject.
 *
 * Capture writes into a caller-owned buffer (no shared frame BSS), so the two
 * frontends can capture concurrently on their own threads. Touch state is a
 * single engine-global: there is one gemstone input target, so one finger at a
 * time across all clients is the correct model. */
#ifndef SCREENCAST_ENGINE_H
#define SCREENCAST_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Framebuffer geometry (device-validated, portrait A8R8G8B8). */
#define SC_FB_W      272u
#define SC_FB_H      480u
#define SC_FB_STRIDE (SC_FB_W * 4u)          /* tight, no row padding */
#define SC_FB_BYTES  (SC_FB_STRIDE * SC_FB_H)
#define SC_NPIX      (SC_FB_W * SC_FB_H)

/* kerncore ready (the arbitrary-kernel-RW gadget resolved)? Both capture and
 * injection need it; a frontend must not start until this returns 1. */
int  sc_engine_ready(void);

/* One-time setup: ensure kerncore helpers, resolve the gemstone/servicesd touch
 * targets, and plant the input-signal stub. Cheap to call again (re-resolves
 * only on a target process restart). */
void sc_engine_init(void);

/* Capture the live framebuffer into argb (>= SC_FB_BYTES, B,G,R,A byte order:
 * this is PixelFormat32bppARGB, feed it straight to ce_image_encode_jpeg).
 * Returns 1 on a fresh frame, 0 if the front-buffer PA was momentarily invalid
 * (caller should retry after a short wait). */
int  sc_capture(unsigned char* argb);

/* B,G,R,A framebuffer -> RGB565 (SC_NPIX u16), for the delta path. */
void sc_argb_to_rgb565(const unsigned char* argb, unsigned short* rgb565);

/* Delta-encode cur vs prev as runs of [u16 skip][u16 copy][copy * u16 pixels].
 * Returns encoded length, or -1 if it would exceed out_cap. */
int  sc_encode_delta(const unsigned short* cur, const unsigned short* prev,
                     unsigned char* out, int out_cap);

/* Inject one touch. action: 0=tap 1=down 2=move 3=up; x,y are portrait pixels
 * (0..SC_FB_W-1, 0..SC_FB_H-1). Safe to call from any frontend thread. */
void sc_inject(unsigned char action, unsigned int x, unsigned int y);

/* While a finger is held still, gemstone's long-press detection needs a live
 * stationary-contact stream. A frontend calls this on its idle ticks; it emits
 * a phase-2 hold sample only once the finger has settled. No-op if none held. */
void sc_feed_hold(void);

/* Is a finger currently down (between down and up)? Frontends tighten their idle
 * tick while held so the hold stream stays live. */
int  sc_held(void);

/* Append a line to the screencast log (\flash2\automation\screencast.log). */
void sc_log(const char* msg);

#ifdef __cplusplus
}
#endif

#endif /* SCREENCAST_ENGINE_H */
