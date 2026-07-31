#ifndef MODS_LIST_CHANNEL_H
#define MODS_LIST_CHANNEL_H

#include <windows.h>

#include "mod_state_abi.h"   /* MOD_STATE_ID_LEN + the channel layout */

/* Cross-process list channel backing a quick-toggle's context picker.

   A setting that declares "context" in its manifest gets one of these, keyed by
   the setting's role-namespaced key. The mod's DAEMON publishes a dynamic option
   list into the shared section (e.g. castd's discovered Chromecast receivers);
   the HUD picker reads those rows and, on select, writes an opaque selection
   token the daemon reads back. Pull-model shared buffer: the picker reads the
   latest rows when it opens (and live-refreshes on the daemon's change notify),
   the daemon reads the latest selection when it needs to act. Neither side owns
   the section: whoever maps first zero-inits it (additive, like the notify
   registry).

   The layout lives in mod_state_abi.h; this header is the HUD side's API over
   it. */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the process-local lock. Called once at DLL process attach, before
   any verb is reachable. */
void ModListChannelProcessAttach(void);

/* Map (creating if absent) the channel for a setting key. Cached per key for the
   process lifetime; returns NULL only if the section can't be created. */
ModListChannelBlock* ModListChannelMap(const char* setting_key);

/* Signal the channel's scan-request event so the daemon refreshes its list.
   Called when the picker opens. No-op if the event can't be created. */
void ModListChannelSignalScan(const char* setting_key);

/* The same event, for a waiter rather than a signaller. */
HANDLE ModListChannelScanEvent(const char* setting_key);

/* Stage a row privately, then publish the first `count` staged rows. Staging is
   per channel so two mods in one host cannot trample each other. */
int  ModListChannelStage(const char* setting_key, int idx, const wchar_t* name,
                         const wchar_t* sub, const char* value);
void ModListChannelCommit(const char* setting_key, int count);

/* Publish a selection: write `value` into sel_value and bump sel_seq. The caller
   wakes the daemon separately (ModStateEventPublish). */
void ModListChannelSelect(const char* setting_key, const char* value);

/* Ask the HUD to raise this key's picker. Callable from any process; the request
   crosses to the HUD through the shared block. */
void ModListChannelRequestOpen(const char* setting_key);

/* HUD side: 1 once per outstanding request, then acknowledged. */
int  ModListChannelTakeOpenRequest(const char* setting_key);

/* The daemon-composed sub-label for this key's channel, or NULL if the channel
   is absent or its sublabel is empty (caller falls back to the state label).
   The returned pointer is into the mapped section; read immediately. */
const wchar_t* ModListChannelSubLabel(const char* setting_key);

#ifdef __cplusplus
}
#endif

#endif /* MODS_LIST_CHANNEL_H */
