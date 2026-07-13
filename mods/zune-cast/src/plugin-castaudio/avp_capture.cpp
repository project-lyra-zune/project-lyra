#include "castaudio.h"
#include "avp_capture.h"
#include "kerncore.h"

#include <string.h>

// Kernel primitives (validated from nativeapp in op_diag.cpp).
#define KMEMCPY_F     0x80072318u   // CE6 u32-stride memcpy(dst, src, len)
#define NVRM_MAP      0xc094abf4u   // NvRmPhysicalMemMap(phys, size, 3, &out_va)
#define NVRM_UNMAP    0xc094ac1cu   // NvRmPhysicalMemUnmap(va)

#define DMA_CH_PAGE   0x6000b000u   // APB-DMA channel registers
#define I2S1_FIFO     0x70002840u   // ch15 APBPTR target = I2S1 TX FIFO
#define HEAP_LO       0x90000000u   // decoded-PCM block PA band
#define HEAP_HI       0xa0000000u

// Tegra 2 Clock & Reset Controller (CAR): the I2S sample-rate clock NvRm
// programs per track. Reading PLLA_BASE + CLK_SOURCE_I2S1 back gives the live
// rate (the I2S clock follows the source track; it is NOT fixed). Offsets are
// the standard T20 CAR layout. See notes/re-2026-06-01-zme0-sample-rate/.
#define CAR_PAGE          0x60006000u
#define CAR_OSC_CTRL      0x50u            // [31:30] osc freq sel
#define CAR_PLLA_BASE     0xb0u            // DIVM[4:0] DIVN[17:8] DIVP[22:20] EN[30]
#define CAR_CLK_I2S1      0x100u           // [31:30] src mux, [7:0] divider (U7.1)

// Per-channel register layout: 0x20 bytes/channel; +0x10 = AHB_PTR (the
// physical addr ch15 is currently reading), +0x18 = APBPTR (FIFO addr,
// used to identify ch15).
#define CH_STRIDE     0x20u
#define CH_AHB_OFF    0x10u
#define CH_APB_OFF    0x18u

// AVP firmware ctx: the deterministic PCM-block source (see
// wiki/architecture/audio-pipeline.md "AVP block list"). NK .data holds the
// kernel VA of the 32 MB NvRm carveout; the AVP firmware image is placed at a
// fixed carveout offset by the boot loader (not a heap allocation); the ch15
// ctx is at AVP-VA 0x7c04 within it. chreg_base validates we found it.
#define NK_CARVEOUT_KVA    0xc097e0c0u   // NK .data: *this = carveout kernel VA
#define AVP_IMG_OFFSET     0x000b2040u   // fixed carveout offset of the AVP image
#define AVP_CTX_AVPVA      0x00007c04u   // AVP-VA of the audio-channel ctx (handler idx 0)
#define AVP_CTX_BLOCKLIST  0x44u         // block_list[16] u32 phys addresses
#define AVP_CTX_CHREG      0x94u         // chreg_base u32 (validation anchor)
#define AVP_CTX_CHREG_CH15 0x6000b1e0u   // ch15 = 0x6000b000 + 15*0x20

static void kbulk(DWORD ksrc, void* udst, DWORD len)
{
    kerncore_kcall(KMEMCPY_F, (DWORD)udst, ksrc, len, 0, 0, 0);
}

static DWORD nvmap(DWORD phys, DWORD size)
{
    DWORD z = 0;
    kerncore_kmemcpy(KERNCORE_KSCRATCH, &z, 4);
    DWORD e = kerncore_kcall(NVRM_MAP, phys, size, 3u, KERNCORE_KSCRATCH, 0, 0);
    if (e != 0) return 0;
    return kerncore_kreadu32(KERNCORE_KSCRATCH);
}

static void nvunmap(DWORD va)
{
    if (va) kerncore_kcall(NVRM_UNMAP, va, 0, 0, 0, 0, 0);
}

// Snap a computed rate to the nearest standard PCM sample rate.
static DWORD snap_rate(DWORD hz)
{
    static const DWORD STD[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000 };
    DWORD best = STD[0], bestd = 0xFFFFFFFFu;
    for (int i = 0; i < (int)(sizeof(STD) / sizeof(STD[0])); i++) {
        DWORD d = (hz > STD[i]) ? (hz - STD[i]) : (STD[i] - hz);
        if (d < bestd) { bestd = d; best = STD[i]; }
    }
    return best;
}

// T20 oscillator frequency from OSC_CTRL[31:30].
static DWORD osc_hz(DWORD osc_ctrl)
{
    switch ((osc_ctrl >> 30) & 3) {
        case 0:  return 13000000u;
        case 1:  return 19200000u;
        case 2:  return 12000000u;
        default: return 26000000u;
    }
}

// Read the live I2S sample rate from CAR (device-validated on 44.1k + 48k tracks).
// The I2S clock derives from PLLA_OUT0, NOT the raw PLLA VCO: PLLA_BASE gives the
// VCO; PLLA_OUT0 (@0xB4) applies a post-divider (out = VCO*2/(ratio+2), ratio @
// [15:8]); and the device reprograms PLLA per rate so PLLA_OUT0 scales with it
// (44.1k→4.704 MHz, 48k→5.120 MHz; PLLA_OUT0/rate = 320/3 exactly). Folding in
// the CLK_SOURCE_I2S1 divider (DIV @ [7:0]):  rate = PLLA_OUT0 * 6 / ((DIV+2)*80).
// CAR-only: the I2S1 controller page (0x70002800) HANGS the read when its clock
// is gated, so it is never touched here.
static DWORD read_i2s_rate(DWORD car_kva)
{
    if (!car_kva) return 0;
    DWORD osc  = kerncore_kreadu32(car_kva + CAR_OSC_CTRL);
    DWORD plla = kerncore_kreadu32(car_kva + CAR_PLLA_BASE);
    DWORD pout = kerncore_kreadu32(car_kva + 0xb4);      // PLLA_OUT0 (the I2S source)
    DWORD i2s  = kerncore_kreadu32(car_kva + CAR_CLK_I2S1);

    DWORD ref  = osc_hz(osc);
    DWORD divm = plla & 0x1f, divn = (plla >> 8) & 0x3ff, divp = (plla >> 20) & 7;
    if (divm == 0) divm = 1;
    unsigned __int64 f_vco  = (unsigned __int64)ref * divn / ((unsigned __int64)divm << divp);
    DWORD ratio = (pout >> 8) & 0xff;
    unsigned __int64 f_pout = (f_vco * 2) / (ratio + 2);
    DWORD div = i2s & 0xff;
    DWORD raw = (DWORD)(f_pout * 6 / ((unsigned __int64)(div + 2) * 80));
    return snap_rate(raw);
}

// Re-read the live rate and publish it on change. Called at span (re)acquisition
// and when the reconcile thread flags a track change (rate_dirty), not per
// captured block. A track change does NOT always realloc the block list
// (in-place buffer reuse produces no re-map), so the reconcile signal catches a
// rate change on those boundaries; the re-map covers the rest.
static void update_rate(CaptureRing* ring, DWORD car_kva)
{
    DWORD r = read_i2s_rate(car_kva);
    if (r && r != (DWORD)ring->sample_rate) {
        cast_log("CAP rate %ld -> %lu Hz", (long)ring->sample_rate, r);
        ring->sample_rate = (LONG)r;
    }
}

// torn-read-reject (live MMIO): two reads must agree. `reg` is inside the
// NvRm-mapped DMA-channel page, which is accessible from this user process, so
// the hot-path poll reads it directly; no kerncore kcall (and thus no kerncore
// lock contention with concurrent code-patchers, e.g. the wifi-awake authority).
static DWORD rd2(DWORD reg)
{
    volatile DWORD* p = (volatile DWORD*)reg;
    DWORD a = p[0];
    DWORD b = p[0];
    return (a == b) ? a : p[0];
}

static DWORD find_ch15_ahb(DWORD chan_kva)
{
    for (DWORD ch = 0; ch < 16; ch++) {
        if (kerncore_kreadu32(chan_kva + ch * CH_STRIDE + CH_APB_OFF) == I2S1_FIFO)
            return chan_kva + ch * CH_STRIDE + CH_AHB_OFF;
    }
    return 0;
}

// Locate the ch15 AVP ctx: carveout kernel VA from NK .data + the fixed AVP
// image offset + the ctx AVP-VA, validated by chreg_base. Returns the ctx kernel
// VA, or 0 if validation fails (wrong offset / AVP not up). No scan: the image
// offset is fixed boot-loader placement; chreg_base is the determinism check.
static DWORD locate_avp_ctx(void)
{
    DWORD carveout = kerncore_kreadu32(NK_CARVEOUT_KVA);
    if (carveout < 0xc0000000u) return 0;
    DWORD ctx = carveout + AVP_IMG_OFFSET + AVP_CTX_AVPVA;
    if (kerncore_kreadu32(ctx + AVP_CTX_CHREG) != AVP_CTX_CHREG_CH15) return 0;
    return ctx;
}

// Read block_list[16] from the AVP ctx and bound the current track's PCM span
// from the in-band entries. Instant (no observation). 0 if no blocks yet.
static int blocklist_span(DWORD ctx_kva, DWORD* base, DWORD* len)
{
    DWORD bl[16];
    kbulk(ctx_kva + AVP_CTX_BLOCKLIST, bl, sizeof(bl));
    DWORD mn = 0xFFFFFFFFu, mx = 0;
    int n = 0;
    for (int i = 0; i < 16; i++) {
        DWORD v = bl[i];
        if (v >= HEAP_LO && v < HEAP_HI) { if (v < mn) mn = v; if (v > mx) mx = v; n++; }
    }
    if (n == 0 || mx <= mn) return 0;
    *base = mn & ~0xfffu;
    *len  = (mx - *base) + CAP_SLOT_SIZE + 0x1000u;
    return 1;
}

// Read the block list + map the span. Retries until blocks appear (audio
// playing) or stop. Returns the span kva, 0 on stop.
static DWORD acquire_span(DWORD ctx_kva, DWORD* base, DWORD* len, HANDLE stop)
{
    while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
        if (blocklist_span(ctx_kva, base, len)) {
            DWORD kva = nvmap(*base, *len);
            if (kva) return kva;
        }
        if (WaitForSingleObject(stop, 50) == WAIT_OBJECT_0) break;  // no audio yet
    }
    return 0;
}

DWORD WINAPI avp_capture_thread(LPVOID arg)
{
    CaptureRing* ring = (CaptureRing*)arg;

    // CE6 priority 110: this tee must copy the just-finished 8 KB block before
    // the I2S DMA recycles it, inside the ~42.7 ms inter-EOC interval. At the
    // default user priority (251) a block-period of preemption by the cast I/O /
    // HTTP / art-encode threads drops blocks (measured ~4-16/track). 110 keeps
    // it above any application-level thread but below the NvRm audio-DDK IST it
    // reads behind: the same value plugin-zpodclientdrvusb's identical producer
    // uses (220 was measured to starve to 500 ms).
    CeSetThreadPriority(GetCurrentThread(), 110);

    DWORD t0 = GetTickCount();
    while (!kerncore_is_ready()) {
        if (WaitForSingleObject(ring->stop_event, 200) == WAIT_OBJECT_0) return 0;
        if (GetTickCount() - t0 > 10000) { cast_log("CAP kerncore not ready"); return 1; }
    }
    kerncore_ensure_helpers();

    DWORD chan_kva = nvmap(DMA_CH_PAGE, 0x1000u);
    if (!chan_kva) { cast_log("CAP map DMA page fail"); return 2; }

    // ch15 (the I2S TX DMA channel) is only allocated while audio is actively
    // playing, so casting can be turned on before any track starts. Poll for it
    // (and the AVP ctx) instead of failing once; acquire_span below then waits
    // for the first block.
    DWORD ahb_reg = 0, ctx_kva = 0;
    {
        int logged = 0;
        for (;;) {
            ahb_reg = find_ch15_ahb(chan_kva);
            if (ahb_reg) { ctx_kva = locate_avp_ctx(); if (ctx_kva) break; }
            if (!logged) { cast_log("CAP waiting for ch15 (no audio yet)"); logged = 1; }
            if (WaitForSingleObject(ring->stop_event, 200) == WAIT_OBJECT_0) {
                nvunmap(chan_kva); return 0;   // toggled off before audio started
            }
        }
    }

    DWORD car_kva = nvmap(CAR_PAGE, 0x1000u);    // CAR: live I2S sample-rate clock
    cast_log("CAP ch15 ahb_reg=%08x ctx=%08x car=%08x", ahb_reg, ctx_kva, car_kva);

    DWORD blk_base = 0, blk_len = 0;
    DWORD blk_kva = acquire_span(ctx_kva, &blk_base, &blk_len, ring->stop_event);
    if (!blk_kva) { nvunmap(car_kva); nvunmap(chan_kva); return 0; }   // stopped before audio
    cast_log("CAP span base=%08x len=%08x kva=%08x", blk_base, blk_len, blk_kva);
    ring->epoch_produced = ring->produced;
    InterlockedIncrement(&ring->track_epoch);
    update_rate(ring, car_kva);
    ring->established = 1;

    DWORD prev = rd2(ahb_reg);
    while (WaitForSingleObject(ring->stop_event, 0) != WAIT_OBJECT_0) {
        DWORD cur = rd2(ahb_reg);
        if (cur == prev) { Sleep(3); continue; }  // one block per I2S EOC

        if (prev >= blk_base && prev + CAP_SLOT_SIZE <= blk_base + blk_len) {
            // `prev` block was just fully consumed by I2S -> valid decoded PCM.
            // (AHB_PTR walks a scattered, software-fed block sequence, so cur-prev
            // is NOT a contiguous +8 KB step; capture health is produced-vs-time,
            // not the AHB delta.)
            DWORD slot = (DWORD)ring->produced % CAP_RING_SLOTS;
            // blk_kva is NvRm-mapped into this process; copy the consumed block
            // directly in user mode, no kerncore kcall on the per-block hot path.
            memcpy(ring->buf + slot * CAP_SLOT_SIZE,
                   (const void*)(blk_kva + (prev - blk_base)), CAP_SLOT_SIZE);
            InterlockedIncrement(&ring->produced);
            // Re-read the rate only when the reconcile thread signalled a track
            // change that produced no re-map (in-place buffer reuse): a cheap
            // interlocked check per block instead of a CAR read on the hot path.
            if (InterlockedExchange(&ring->rate_dirty, 0)) update_rate(ring, car_kva);
        } else {
            // AHB_PTR left the mapped span -> track change reallocated the blocks.
            // Re-read the block list (instant) and re-map; no observation phase.
            ring->established = 0;
            nvunmap(blk_kva);
            blk_kva = acquire_span(ctx_kva, &blk_base, &blk_len, ring->stop_event);
            if (!blk_kva) break;
            cast_log("CAP re-map base=%08x len=%08x kva=%08x produced=%ld", blk_base, blk_len, blk_kva, ring->produced);
            ring->epoch_produced = ring->produced;
            InterlockedIncrement(&ring->track_epoch);
            ring->established = 1;
            update_rate(ring, car_kva);            // span re-acquired = track change
            InterlockedExchange(&ring->rate_dirty, 0);
            prev = rd2(ahb_reg);
            continue;
        }
        prev = cur;
    }

    if (blk_kva) nvunmap(blk_kva);
    nvunmap(car_kva);
    nvunmap(chan_kva);
    cast_log("CAP exit produced=%ld", ring->produced);
    return 0;
}
