#pragma once

#include "nativeapp_common.h"

struct Op23Result {
	u32 channel, ahb0;
	u32 segA_base, segA_top, segB_base, segB_top;
	u32 flips, bytes_captured, elapsed_ms, error_code;
	// opcode-24 ENDPT drain diagnostic (low16 = ENDPTCOMPLETE-bit17
	// observations, high16 = post-prime ENDPTSTAT-bit17-clear
	// observations). Either nonzero ⇒ host issued iso-IN on EP 0x81.
	u32 endpt_diag;
};

struct Op23Result run_op23(u32 run_ms, u32 cap_bytes, u32 phys_base);
struct Op23Result run_op24(u32 run_ms, u32 handle, u32 ep_index, u32 pkt);
struct Op23Result run_op25_ring(u32 run_ms, u32 handle);

// Map / unmap a physical range into kernel VA via NvRmPhysicalMemMap.
// Returns 0 on map failure. Other RPC opcodes use these for MMIO probes.
DWORD op23_map(DWORD phys, DWORD size);
void  op23_unmap(DWORD va);
