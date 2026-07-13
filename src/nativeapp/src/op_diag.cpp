#include "op_diag.h"
#include "zlog.h"

// Bulk-copy `len` bytes from a kernel VA to a user-space buffer in one
// PL1 call. Uses the same KMEMCPY_FN recipe as repl_primitives.cpp's
// DUMP_VA_TO_FILE, validated in-process for kernel→user copies.
static void op21_kbulk(DWORD kernel_src, void* user_dst, DWORD len) {
	kerncore_kcall(0x80072318u, (DWORD)user_dst, kernel_src, len, 0, 0, 0);
}

// ── Opcode 23: ping-pong (double-buffer) tee ────────────────────────────
//
// ch15 is a 2-buffer ping-pong DMA: AHB_PTR sweeps segment A monotonically,
// jumps ~700 KB to segment B, sweeps B, jumps back. Bases/tops are learned
// live per-track (no hardcoding). Tee copies behind AHB_PTR within a
// segment and handles the inter-segment flip explicitly.

DWORD op23_map(DWORD phys, DWORD size) {
	DWORD z = 0;
	kerncore_kmemcpy(KERNCORE_KSCRATCH, &z, 4);
	DWORD e = kerncore_kcall(0xc094abf4u, phys, size, 3u,
	                         KERNCORE_KSCRATCH, 0, 0);
	if (e != 0) return 0;
	return kerncore_kreadu32(KERNCORE_KSCRATCH);
}

void op23_unmap(DWORD va) {
	if (va) kerncore_kcall(0xc094ac1cu, va, 0, 0, 0, 0, 0); // NvRmPhysicalMemUnmap
}

// Pacing source: APB-DMA ch15. Each AHB_PTR change = one EOC = one block
// (BSZ=0x2000 → 8KB per EOC, matches Tegra20 WCOUNT=1023 + 4KB-per-burst).
#define OP23_BSZ      0x2000u      // 8 KB/block
#define OP23_I2S_FIFO 0x70002840u  // ch15 APBPTR (= I2S1 TX FIFO)

// torn-read-reject sample (two reads must agree)
static DWORD op23_rd(DWORD reg) {
	DWORD a = kerncore_kreadu32(reg);
	DWORD b = kerncore_kreadu32(reg);
	return (a == b) ? a : kerncore_kreadu32(reg);
}

struct Op23Result run_op23(u32 run_ms, u32 cap_bytes, u32 phys_base) {
	struct Op23Result r;
	memset(&r, 0, sizeof(r));
	(void)phys_base;
	if (run_ms == 0 || run_ms > 30000) run_ms = 8000;
	if (cap_bytes == 0 || cap_bytes > 0x400000) cap_bytes = 0x180000;
	if (!kerncore_is_ready())       { r.error_code = 1; return r; }
	if (!kerncore_ensure_helpers()) { r.error_code = 2; return r; }

	DWORD chan_kva = op23_map(0x6000b000u, 0x1000u);
	if (!chan_kva) { r.error_code = 3; return r; }
	int found = -1;
	int ch;
	for (ch = 0; ch < 16; ch++)
		if (kerncore_kreadu32(chan_kva + (DWORD)ch*0x20u + 0x18u) == OP23_I2S_FIFO) {
			found = ch; break;
		}
	if (found < 0) { op23_unmap(chan_kva); r.error_code = 4; return r; }
	r.channel = (u32)found;
	DWORD ahb_reg = chan_kva + (DWORD)found*0x20u + 0x10u;
	r.ahb0 = op23_rd(ahb_reg);

	// LEARN the streamed-block phys span from AHBPTR (~800 ms): the
	// software-fed scattered 4 KB-block sequence.
	DWORD gmin = 0xFFFFFFFFu, gmax = 0;
	DWORD ts = GetTickCount();
	while (GetTickCount() - ts < 800u) {
		DWORD v = op23_rd(ahb_reg);
		if (v >= 0x90000000u && v < 0xA0000000u) {
			if (v < gmin) gmin = v;
			if (v > gmax) gmax = v;
		}
	}
	if (gmax == 0 || gmax <= gmin) { op23_unmap(chan_kva); r.error_code = 9; return r; }
	DWORD blk_base = gmin & ~0xfffu;
	DWORD blk_len  = (gmax - blk_base) + OP23_BSZ + 0x1000u;
	r.segA_base = blk_base;
	r.segB_top  = gmax;
	DWORD blk_kva = op23_map(blk_base, blk_len);
	if (!blk_kva) { op23_unmap(chan_kva); r.error_code = 6; return r; }

	// Per-block flash WriteFile is far too slow (43 tiny writes/s flash-
	// bound the loop and we miss EOCs). Buffer in RAM, single WriteFile at
	// end.
	unsigned char* ram = (unsigned char*)calloc(cap_bytes, 1);
	if (!ram) { op23_unmap(blk_kva); op23_unmap(chan_kva);
	            r.error_code = 7; return r; }

	DWORD prev  = op23_rd(ahb_reg);
	DWORD start = GetTickCount();
	while ((GetTickCount() - start) < run_ms &&
	       r.bytes_captured + OP23_BSZ <= cap_bytes) {
		DWORD cur = op23_rd(ahb_reg);
		if (cur != prev) {
			if (prev >= blk_base && prev + OP23_BSZ <= blk_base + blk_len) {
				op21_kbulk(blk_kva + (prev - blk_base),
				           ram + r.bytes_captured, OP23_BSZ);
				r.bytes_captured += OP23_BSZ;
				r.flips++;                       // blocks captured
			}
			prev = cur;
		}
	}
	r.elapsed_ms = GetTickCount() - start;

	HANDLE hf = CreateFileW(L"\\flash2\\zd-tee.pcm", GENERIC_WRITE,
	                        FILE_SHARE_READ, NULL, CREATE_ALWAYS,
	                        FILE_ATTRIBUTE_NORMAL, NULL);
	if (hf == INVALID_HANDLE_VALUE) {
		free(ram); op23_unmap(blk_kva); op23_unmap(chan_kva);
		r.error_code = 8; return r;
	}
	{
		DWORD off2 = 0;
		while (off2 < r.bytes_captured) {
			DWORD n = r.bytes_captured - off2;
			if (n > 0x10000u) n = 0x10000u;
			DWORD w = 0;
			WriteFile(hf, ram + off2, n, &w, NULL);
			off2 += n;
		}
	}
	CloseHandle(hf);
	free(ram);
	op23_unmap(blk_kva);
	op23_unmap(chan_kva);
	return r;
}

// ── Opcode 24: USB iso audio tee (DDK TX-start) ─────────────────────────
//
// Consumer-paced ch15 source (op23 mechanism) → per AHB_PTR change feeds
// `pkt`-byte iso-IN submissions through libnvddk_misc!0xC08EC2F8 to
// EP 0x81. `submit_iso` is the single swap point for the future zero-copy
// dTD variant.
#define OP24_NVOSALLOC 0xC088D2B0u
#define OP24_NK_PROC   0x80BEE328u
#define OP24_DDK_TX    0xC08EC2F8u
#define OP24_FLAGS     0x101u

struct Op23Result run_op24(u32 run_ms, u32 handle, u32 ep_index,
                                  u32 pkt) {
	struct Op23Result r;
	memset(&r, 0, sizeof(r));
	if (run_ms == 0 || run_ms > 30000) run_ms = 8000;
	if (handle == 0) handle = 0xd3550280u;
	if (ep_index == 0) ep_index = 3u;
	// pkt = iso submit chunk size, runtime-selectable: 192 = per-iso-packet
	// (the burst-then-idle baseline), OP23_BSZ (0x2000) = whole 8 KB block
	// in one DDK-TX so ChipIdea fragments it into one 192 B packet/frame
	// across ~43 frames. pkt==1 stays the DIAG path.
	if (pkt == 0u) pkt = 192u;
	else if (pkt != 1u && pkt > OP23_BSZ) pkt = OP23_BSZ;
	if (!kerncore_is_ready())       { r.error_code = 1; return r; }
	if (!kerncore_ensure_helpers()) { r.error_code = 2; return r; }
	r.ahb0 = handle;

	// DIAG (pkt==1): trivial-stub 1-shot. Isolates the plant/exec/return
	// mechanism (no ch15, no DDK-TX). r.flips == 0x42 ⇒ mechanism OK.
	if (pkt == 1u) {
		// Plant into the ALWAYS-EXECUTABLE kerncore scratch (the page the
		// helpers run from), via plain kmemcpy - NOT NvOsAlloc+patch_code:
		// heap is XN, and patch_code only clears the write bit, never XN,
		// so kcall(heap stub) prefetch-aborts.
		DWORD DW[3] = { 0xE52DE004u,   // push {lr}
		                0xE3A00042u,   // mov  r0, #0x42
		                0xE49DF004u }; // pop  {pc}
		kerncore_kmemcpy(KERNCORE_KSCRATCH, DW, sizeof(DW));
		r.flips      = kerncore_kcall(KERNCORE_KSCRATCH, 0, 0, 0, 0, 0, 0);
		r.segB_base  = KERNCORE_KSCRATCH;          // expect r.flips==0x42
		r.error_code = 0;
		return r;
	}

	DWORD chan_kva = op23_map(0x6000b000u, 0x1000u);
	if (!chan_kva) { r.error_code = 3; return r; }
	int found = -1, ch;
	for (ch = 0; ch < 16; ch++)
		if (kerncore_kreadu32(chan_kva + (DWORD)ch*0x20u + 0x18u) == OP23_I2S_FIFO) {
			found = ch; break;
		}
	if (found < 0) { op23_unmap(chan_kva); r.error_code = 4; return r; }
	r.channel = (u32)found;
	DWORD ahb_reg = chan_kva + (DWORD)found*0x20u + 0x10u;

	DWORD gmin = 0xFFFFFFFFu, gmax = 0, ts = GetTickCount();
	while (GetTickCount() - ts < 800u) {
		DWORD v = op23_rd(ahb_reg);
		if (v >= 0x90000000u && v < 0xA0000000u) {
			if (v < gmin) gmin = v;
			if (v > gmax) gmax = v;
		}
	}
	if (gmax == 0 || gmax <= gmin) { op23_unmap(chan_kva); r.error_code = 9; return r; }
	DWORD blk_base = gmin & ~0xfffu;
	DWORD blk_len  = (gmax - blk_base) + OP23_BSZ + 0x1000u;
	r.segA_base = blk_base;
	r.segB_top  = gmax;
	DWORD blk_kva = op23_map(blk_base, blk_len);
	if (!blk_kva) { op23_unmap(chan_kva); r.error_code = 6; return r; }

	// The capstone-verified DDK-TX stub (len=r0, buf=r1). Planted into the
	// always-executable kerncore scratch via plain kmemcpy (heap is XN).
	// NK clobbers this region under load, so it is re-kmemcpy'd before
	// every kcall in the worker loop below.
	DWORD W[26] = {
		0xE92D4070u, 0xE1A05000u, 0xE1A06001u, 0xE24DD01Cu, 0xE3A04000u,
		0xE58D4000u, 0xE58D4004u, 0xE59F0030u, 0xE59F1030u, 0xE59F2030u,
		0xE3A03000u, 0xE58D5008u, 0xE58D600Cu, 0xE59F4024u, 0xE58D4010u,
		0xE28DD008u, 0xE59F401Cu, 0xE12FFF34u, 0xE24DD008u, 0xE28DD01Cu,
		0xE8BD8070u, handle, ep_index, OP24_FLAGS, 0u, OP24_DDK_TX };
	DWORD stub = KERNCORE_KSCRATCH;

	// ENDPT drain diagnostic. ChipIdea EP1-IN (TX) = bit17. ENDPTCOMPLETE
	// (MMIO+0x1BC) sets when the host consumes an IN transfer; ENDPTSTAT
	// (MMIO+0x1B8) bit17 is set while a primed dTD is armed and clears
	// once drained. Both counters zero ⇒ host never issued iso-IN.
	DWORD mmio    = kerncore_kreadu32(handle + 4u);
	DWORD epc_cnt = 0, clr_cnt = 0, primed = 0;

	DWORD prev  = op23_rd(ahb_reg);
	DWORD start = GetTickCount();
	while ((GetTickCount() - start) < run_ms) {
		DWORD cur = op23_rd(ahb_reg);
		if (cur != prev) {
			if (prev >= blk_base && prev + OP23_BSZ <= blk_base + blk_len) {
				// Sample BEFORE re-priming: reflects whether the host
				// drained the PRIOR block's dTD during the inter-block
				// gap. Sampling after a submit would always show the
				// endpoint freshly armed and never discriminate.
				if (primed) {
					DWORD st = kerncore_kreadu32(mmio + 0x1B8u); // STAT
					DWORD cp = kerncore_kreadu32(mmio + 0x1BCu); // COMPLETE
					if (cp & 0x20000u) { if (epc_cnt < 0xFFFFu) epc_cnt++; }
					if (!(st & 0x20000u)) { if (clr_cnt < 0xFFFFu) clr_cnt++; }
				}
				DWORD off;
				for (off = 0; off + pkt <= OP23_BSZ; off += pkt) {
					DWORD buf = blk_kva + (prev - blk_base) + off;
					// submit_iso (THE SWAP POINT) - one in-device copy.
					// Replant stub (NK clobbers exec scratch under load)
					// then call: len=r0, buf=r1.
					kerncore_kmemcpy(stub, W, sizeof(W));
					DWORD rv = kerncore_kcall(stub, pkt, buf, 0, 0, 0, 0);
					if (rv != 0) r.segA_top++;          // submit_err
					else { r.flips++; r.bytes_captured += pkt; primed = 1; }
				}
			}
			prev = cur;
		}
	}
	r.endpt_diag = (epc_cnt & 0xFFFFu) | ((clr_cnt & 0xFFFFu) << 16);
	r.segB_base  = prev;                                // last AHB_PTR
	r.elapsed_ms = GetTickCount() - start;
	op23_unmap(blk_kva);
	op23_unmap(chan_kva);
	return r;
}

static void w32(DWORD va, DWORD val) {
	kerncore_kmemcpy(va, &val, 4);
}

// opcode 25 - zero-copy iso ring. Per AVP-DMA ch15 block: write ONE
// ChipIdea dTD whose buffer pointers are the PCM block's PHYSICAL address
// (true zero-copy), link it into the live ep1 dQH, and ENDPTPRIME bit17.
// dTD/dQH live at fixed addresses reused every block ⇒ nothing is
// allocated/freed ⇒ DDK-pool-exhaustion wedge is impossible. Re-prime is
// gated on ENDPTSTAT bit17 == 0 so the running controller's dQH is never
// modified mid-transfer.
struct Op23Result run_op25_ring(u32 run_ms, u32 handle) {
	struct Op23Result r;
	memset(&r, 0, sizeof(r));
	if (run_ms == 0 || run_ms > 30000) run_ms = 8000;
	if (handle == 0) handle = 0xd3550280u;
	if (!kerncore_is_ready())       { r.error_code = 1; return r; }
	if (!kerncore_ensure_helpers()) { r.error_code = 2; return r; }
	r.ahb0 = handle;

	DWORD mmio   = kerncore_kreadu32(handle + 4u);
	// Controller will not transmit primed dТDs with TXE=0. Self-set
	// ENDPTCTRL[1]=0x00840000 (ISO + TXE) so endpt_diag=0 can be
	// attributed correctly.
	DWORD ec_pre = mmio ? kerncore_kreadu32(mmio + 0x1C4u) : 0;
	if (mmio) { DWORD ecv = 0x00840000u;
	            kerncore_kmemcpy(mmio + 0x1C4u, &ecv, 4); }
	zlog("ECRNG", ec_pre, mmio ? kerncore_kreadu32(mmio + 0x1C4u) : 0,
	     mmio);
	DWORD eplist = kerncore_kreadu32(mmio + 0x158u);     // dQH phys
	if (!eplist) { r.error_code = 7; return r; }
	DWORD pg = op23_map(eplist & ~0xFFFu, 0x1000u);
	if (!pg) { r.error_code = 7; return r; }
	DWORD dqh3 = pg + (eplist & 0xFFFu) + 3u * 0x40u;    // ep1 dQH (VA)
	DWORD pool_pa = kerncore_kreadu32(handle + 0x188u);
	if (!pool_pa) { op23_unmap(pg); r.error_code = 8; return r; }
	DWORD dtd = pool_pa + 0x10000u;                      // zero-copy dTD

	DWORD chan_kva = op23_map(0x6000b000u, 0x1000u);
	if (!chan_kva) { op23_unmap(pg); r.error_code = 3; return r; }
	int found = -1, ch;
	for (ch = 0; ch < 16; ch++)
		if (kerncore_kreadu32(chan_kva + (DWORD)ch*0x20u + 0x18u) == OP23_I2S_FIFO) {
			found = ch; break;
		}
	if (found < 0) { op23_unmap(chan_kva); op23_unmap(pg); r.error_code = 4; return r; }
	r.channel = (u32)found;
	DWORD ahb_reg = chan_kva + (DWORD)found*0x20u + 0x10u;

	DWORD gmin = 0xFFFFFFFFu, gmax = 0, ts = GetTickCount();
	while (GetTickCount() - ts < 800u) {
		DWORD v = op23_rd(ahb_reg);
		if (v >= 0x90000000u && v < 0xA0000000u) {
			if (v < gmin) gmin = v;
			if (v > gmax) gmax = v;
		}
	}
	if (gmax == 0 || gmax <= gmin) {
		op23_unmap(chan_kva); op23_unmap(pg); r.error_code = 9; return r;
	}
	DWORD blk_base = gmin & ~0xFFFu;
	DWORD blk_len  = (gmax - blk_base) + OP23_BSZ + 0x1000u;
	r.segA_base = blk_base;
	r.segB_top  = gmax;

	// Initialise ep1 dQH: iso-IN, Mult=1 (bit30), MaxPacketLen=192
	// (bits[26:16] = 0xC0<<16). cap = 0x40000000 | 0x00C00000.
	w32(dqh3 + 0x00u, 0x40C00000u);   // cap
	w32(dqh3 + 0x04u, 0x00000001u);   // current_dTD = invalid → load next
	w32(dqh3 + 0x08u, dtd);           // next_dTD
	w32(dqh3 + 0x0Cu, 0x00000000u);   // overlay token cleared

	DWORD prev  = op23_rd(ahb_reg);
	DWORD start = GetTickCount();
	DWORD epcacc = 0;   // OR-accumulate ENDPTCOMPLETE/STAT per prime
	while ((GetTickCount() - start) < run_ms) {
		DWORD cur = op23_rd(ahb_reg);
		if (cur != prev) {
			if (prev >= blk_base && prev + OP23_BSZ <= blk_base + blk_len) {
				DWORD stat = kerncore_kreadu32(mmio + 0x1B8u);
				if (!(stat & 0x20000u)) {           // ep1 idle → safe
					w32(dtd + 0x00u, 0x00000001u);  // next = terminate
					// token: total=8KB[30:16], IOC bit15, active bit7
					w32(dtd + 0x04u, (OP23_BSZ << 16) | 0x8000u | 0x80u);
					w32(dtd + 0x08u, prev);                       // buf0 = blk phys
					w32(dtd + 0x0Cu, (prev & ~0xFFFu) + 0x1000u); // buf1
					w32(dtd + 0x10u, (prev & ~0xFFFu) + 0x2000u); // buf2
					w32(dtd + 0x14u, 0u);
					w32(dtd + 0x18u, 0u);
					w32(dqh3 + 0x04u, 0x00000001u);
					w32(dqh3 + 0x08u, dtd);
					w32(dqh3 + 0x0Cu, 0x00000000u);
					// Re-assert ISO+TXE every prime: SET_INTERFACE /
					// libnvusbf may re-clear TXE between primes; a
					// cleared TXE silently drops every iso-IN.
					w32(mmio + 0x1C4u, 0x00840000u); // ENDPTCTRL[1]
					w32(mmio + 0x1B0u, 0x20000u);   // ENDPTPRIME bit17
					r.flips++; r.bytes_captured += OP23_BSZ;
					// Briefly watch ENDPTCOMPLETE bit17. OR-accumulate so
					// a transient completion between samples is not
					// missed.
					{ DWORD st = GetTickCount();
					  while (GetTickCount() - st < 5u)
					    epcacc |= kerncore_kreadu32(mmio + 0x1BCu); }
				} else {
					r.segA_top++;                   // ep1 busy → block dropped
				}
			}
			prev = cur;
		}
	}
	r.segB_base  = prev;
	r.elapsed_ms = GetTickCount() - start;
	// endpt_diag: bit0 = ENDPTCOMPLETE bit17 EVER seen; bit16 = end
	// ENDPTSTAT bit17. segB_top := final ENDPTCTRL[1] (shows whether
	// ISO+TXE held through the ring).
	r.endpt_diag =
		(((epcacc | kerncore_kreadu32(mmio + 0x1BCu)) & 0x20000u)
		 ? 1u : 0u) |
		(((kerncore_kreadu32(mmio + 0x1B8u) & 0x20000u) ? 1u : 0u)
		 << 16);
	r.segB_top = kerncore_kreadu32(mmio + 0x1C4u);  // final ENDPTCTRL[1]
	op23_unmap(chan_kva);
	op23_unmap(pg);
	return r;
}
