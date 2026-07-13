#ifndef MODS_VOLUME_STATE_H
#define MODS_VOLUME_STATE_H

#include <windows.h>

/* On-screen system-volume publisher (servicesd-hosted).

   Taps the single ZAM volume-write choke point in zam_serv.dll and publishes
   the current on-screen volume into a named shared section, signalling a named
   auto-reset event on each change. Cross-process consumers (the zune-cast
   nativeapp plugin) wait on the event and pull the latest value: push wakeup,
   shared-buffer value (coalesces rapid changes to the latest by construction).

   This is the platform/capability half; the consumer half lives in the plugin.
   The section layout + names below are the cross-process ABI; a consumer
   defines a matching struct and opens these names. */

#define VOLUME_STATE_SECTION  L"zune-volume-state"
#define VOLUME_STATE_EVENT    L"zune-volume-evt"

typedef struct {
    DWORD version;   /* 1 once initialised */
    DWORD seq;       /* incremented on every change */
    DWORD vol;       /* current on-screen volume, 0..max */
    DWORD max;       /* full-scale (30 on Zune HD) */
} VolumeStateBlock;

#ifdef __cplusplus
extern "C" {
#endif

/* Create the shared section + event and arm the writer detour. servicesd only;
   no-op in other hosts. The detour is deferred onto a worker until kerncore's
   PT-flip gadget is ready (it patches zam_serv .text). Idempotent. */
void VolumeStateInstall(void);

#ifdef __cplusplus
}
#endif

#endif /* MODS_VOLUME_STATE_H */
