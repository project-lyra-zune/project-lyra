#ifndef MODS_LIST_CHANNEL_H
#define MODS_LIST_CHANNEL_H

#include <windows.h>

#include "mods_state_block.h"   /* MOD_STATE_ID_LEN */

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

   ABI: the daemon side MUST mirror ModListChannelBlock byte-for-byte; the
   section/event names carry the layout version so a stale layout cannot map.
   Cast's mirror: netapps/zune-cast/src/plugin-castaudio/cast_channel.{h,c}. */

#define MODLISTCH_MAX_ROWS   8
#define MODLISTCH_NAME_LEN   48    /* wchar: row primary + sub label */
#define MODLISTCH_VAL_LEN    40    /* char: opaque selection token, e.g. "ip:port" */
#define MODLISTCH_SUBLABEL_LEN 64  /* wchar: daemon-composed quick-toggle row sub-label */

typedef struct {
    wchar_t name[MODLISTCH_NAME_LEN];   /* primary label (row main) */
    wchar_t sub[MODLISTCH_NAME_LEN];    /* optional sub-label ("" = none) */
    char    value[MODLISTCH_VAL_LEN];   /* selection token the daemon understands */
} ModListChannelRow;

typedef struct {
    DWORD             version;                  /* 1 once initialised */
    DWORD             list_seq;                 /* daemon bumps on each publish */
    DWORD             count;                    /* # valid rows */
    ModListChannelRow row[MODLISTCH_MAX_ROWS];
    DWORD             sel_seq;                  /* HUD bumps on each selection */
    char              sel_value[MODLISTCH_VAL_LEN];  /* chosen token (HUD writes, daemon reads) */
    /* Daemon-composed sub-label for the owning quick-toggle row; overrides the
       setting's status state label when non-empty. A trailing ellipsis (…) is
       rendered by the native list as the animated loading indicator. */
    wchar_t           sublabel[MODLISTCH_SUBLABEL_LEN];
} ModListChannelBlock;

#ifdef __cplusplus
extern "C" {
#endif

/* Map (creating if absent) the channel for a setting key. Cached per key for the
   process lifetime; returns NULL only if the section can't be created. */
ModListChannelBlock* ModListChannelMap(const char* setting_key);

/* Signal the channel's scan-request event so the daemon refreshes its list.
   Called when the picker opens. No-op if the event can't be created. */
void ModListChannelSignalScan(const char* setting_key);

/* Publish a selection: write `value` into sel_value and bump sel_seq. The caller
   wakes the daemon separately (ModStateEventPublish). */
void ModListChannelSelect(const char* setting_key, const char* value);

/* The daemon-composed sub-label for this key's channel, or NULL if the channel
   is absent or its sublabel is empty (caller falls back to the state label).
   The returned pointer is into the mapped section; read immediately. */
const wchar_t* ModListChannelSubLabel(const char* setting_key);

#ifdef __cplusplus
}
#endif

#endif /* MODS_LIST_CHANNEL_H */
