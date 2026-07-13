#ifndef MODS_WIFI_AWAKE_H
#define MODS_WIFI_AWAKE_H

#include <windows.h>

/* On-battery Wi-Fi keepalive - a declarative demand on the device-proven
   keepalive patch set. The Zune HD drops Wi-Fi a few minutes into battery idle
   because two independent power layers tear the link down: znet's battery/idle
   Wi-Fi policy and ZAM's shallow-Suspend gate. The proven fix (device A/B, fw
   4.5) is three servicesd .text patches applied together: znet timer
   0x41a7feb0, znet idle-active 0x41a772bc, ZAM selector-8 threshold 0x4189eb88.

   This subsystem is the single owner of that patch set. The patches override
   stock battery power policy, so they are applied only while keepalive is
   *demanded* and restored to stock when it is not.

   Demand is PULLED from stable state, not pushed by leases: a mod declares
   `holds: ["wifi_awake"]` on a setting, which registers that setting's slot as
   a demand source. The authority ORs every registered demand slot each
   reconcile; keepalive is on iff some demand slot's state is active (>0). The
   "Keep WiFi on" toggle is just one such demand source, no different from a
   mod's (e.g. zune-cast's "Cast" toggle). Because demand is a function of a
   stable setting bit, it cannot flap mid-feature the way an imperative
   acquire/release lease tied to a session lifecycle did. */

#ifdef __cplusplus
extern "C" {
#endif

/* Register a ModStateBlock slot key whose active state (>0) demands wifi_awake.
   Idempotent. Called in servicesd for each setting that declares
   holds:["wifi_awake"]. The authority pulls all registered slots. */
void WifiAwakeRegisterDemand(const char* state_key);

/* Authority worker (servicesd). Sole owner of the keepalive patch set: on each
   demand change (or a periodic sweep) it reconciles the three patches to match
   current demand. Never returns. */
DWORD WINAPI WifiAwakeAuthorityThread(LPVOID param);

/* Activate the subsystem on demand (idempotent). Spawns the authority thread.
   Driven by the resolver when a mod requires/provides wifi_awake. No-op outside
   servicesd (the authority + patches live there). */
void WifiAwake_EnsureActive(void);

/* Wake the authority to re-evaluate demand immediately (e.g. after a quick-
   toggle flips, or after persisted state is restored at boot). Safe to call
   from any process; a no-op if the authority is not running. */
void WifiAwake_Notify(void);

#ifdef __cplusplus
}
#endif

#endif
