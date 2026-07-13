/* mod_state: daemon-side access to the zuxhook ModStateBlock + notify registry.
 *
 * Generic modkit runtime shared by every boot-spawned mod daemon (zune-cast,
 * screencast, ...). A daemon reads its setting slot ("setting/<mod>/<id>") to
 * learn its toggle state and writes its status slot ("status/<mod>/<id>") to
 * drive its HUD icon; both are pulled/pushed by role-namespaced key, so this
 * carries no mod-specific knowledge. The only per-daemon parameter is the unique
 * wake-event name, supplied once via mod_state_daemon_init.
 *
 * Cross-process ABI: MUST match payloads/replacement/zuxhook/mods_state_block.h
 * and mods_state_event.{c,h}. */
#ifndef MOD_STATE_H
#define MOD_STATE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time daemon setup: register this daemon's unique wake-event name. One
 * event per daemon, so a second daemon's signal cannot split this one's. Call
 * once at startup before mod_state_change_event(). The name must be unique per
 * daemon binary, e.g. L"zune-mod-state-evt-castd". */
void mod_state_daemon_init(const wchar_t* daemon_event_name);

/* The daemon's own change-notification event, signalled by a producer when a
 * slot changes. Wait on this instead of polling: a toggle is a discrete action,
 * so the edge is pushed, not sampled. First call creates the event (named by
 * mod_state_daemon_init) AND registers the daemon as a consumer in the shared
 * notify registry, so the producer's fan-out reaches it. Auto-reset (one
 * waiter). NULL if the name was never set or the event can't be created. Cached
 * for the process life. */
HANDLE mod_state_change_event(void);

/* Pull-reader for a slot's current state (0..N-1) by its role-namespaced key
 * ("setting/<mod>/<id>" | "status/<mod>/<id>"). Returns the state, or -1 if the
 * section or the key's slot is absent (caller treats -1 as state 0). Maps once
 * and caches the view; safe to poll on a tick. */
int mod_state_get_state(const char* key);

/* Fan a change out to every registered consumer (UI queues get a ping, daemon
 * events a SetEvent) without touching a state slot. Used to wake the HUD when a
 * daemon publishes an out-of-band change (e.g. a device-list channel refresh). */
void mod_state_notify(void);

/* Publish the daemon's own status: write `state` to the (pre-seeded) status
 * slot, stamp the daemon's pid as the owner (so the platform reaper resets it to
 * 0 if the daemon dies), and wake the UI hosts to re-render the icon. No-op
 * until the slot is seeded by the platform (register_status, Phase 2). */
void mod_state_set_status(const char* key, int state);

#ifdef __cplusplus
}
#endif

#endif /* MOD_STATE_H */
