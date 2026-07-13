#ifndef MODS_XUI_HANDLE_H
#define MODS_XUI_HANDLE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve an HXUIOBJ handle via the per-process engine handle table 0x41873bb0,
   matching the engine's own resolver (sub_0x41823274):
     lo16 = handle & 0xffff; hi8<<2 -> subtable @+0x8c; lo8<<3 -> slot;
     [slot] is the generation (== handle>>16); the object head is [slot+4].
   Returns NULL on a stale generation, out-of-range handle, or a faulting deref. */
void* XuiResolveObj(DWORD handle);

/* XuiResolveObj, then follow the base-class aggregation chain ([obj+4]) to its
   tail and return the renderer instance the tail carries at [tail+0x18]. For a
   ClassOverride element the chain has more than one layer, so the head (what an
   attached-prop setter writes) is not the tail the arrange/render reads.
   Returns 0 on failure. Walk is bounded (self-loop guard + 16-level cap). */
DWORD XuiResolveNode(DWORD handle);

#ifdef __cplusplus
}
#endif

#endif
