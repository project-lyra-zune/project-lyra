/* mod_list_channel: daemon-side of a quick-toggle context-picker list channel.
 *
 * Generic modkit runtime shared by every mod whose quick-toggle declares
 * `context: { kind: select }`. The daemon publishes the picker's rows and reads
 * the user's selection back; it also composes the quick-toggle row's sub-label
 * (e.g. "View at 192.168.0.100:8080"). The channel is keyed by the mod's toggle
 * key (bound once via mod_channel_init), so a daemon and the HUD compute the same
 * section/scan-event names.
 *
 * Rows and selection carry an opaque `value` token whose meaning is the mod's own
 * (an "ip:port" for zune-cast's receiver picker, a mode id for screencast).
 *
 * Cross-process ABI: MUST match src/zuxhook/hud/mods_list_channel.h byte-for-byte
 * (the section/scan names carry the layout version, so a stale layout cannot map). */
#ifndef MOD_LIST_CHANNEL_H
#define MOD_LIST_CHANNEL_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODLISTCH_MAX_ROWS     8
#define MODLISTCH_NAME_LEN     48    /* wchar: row primary + sub label */
#define MODLISTCH_VAL_LEN      40    /* char: opaque selection token */
#define MODLISTCH_SUBLABEL_LEN 64    /* wchar: daemon-composed quick-toggle sub-label */

typedef struct {
    wchar_t name[MODLISTCH_NAME_LEN];
    wchar_t sub[MODLISTCH_NAME_LEN];
    char    value[MODLISTCH_VAL_LEN];
} ModListChannelRow;

typedef struct {
    DWORD             version;
    DWORD             list_seq;
    DWORD             count;
    ModListChannelRow row[MODLISTCH_MAX_ROWS];
    DWORD             sel_seq;
    char              sel_value[MODLISTCH_VAL_LEN];
    /* Daemon-composed sub-label for the quick-toggle row (overrides the setting's
     * status state label). "" = fall back to the state label. A trailing ellipsis
     * (…) renders as the native list's animated loading indicator. */
    wchar_t           sublabel[MODLISTCH_SUBLABEL_LEN];
} ModListChannelBlock;

/* Bind this daemon's channel to its toggle key ("setting/<mod>/<id>"). Call once
 * at startup; the section/scan-event names derive from it. */
void mod_channel_init(const char* toggle_key);

/* The scan-request event the HUD signals when the picker opens. A daemon that
 * refreshes a dynamic list waits on it. NULL only if it can't be created. */
HANDLE mod_channel_scan_event(void);

/* Publish `n` rows (clamped to MODLISTCH_MAX_ROWS) verbatim, bump list_seq, and
 * wake the HUD so an open picker re-queries. */
void mod_channel_publish(const ModListChannelRow* rows, int n);

/* Copy the currently-published rows into out (up to max); returns the count. Lets
 * a dynamic-discovery daemon union fresh results with what it published before. */
int mod_channel_get_rows(ModListChannelRow* out, int max);

/* Read the selected row's opaque value token into out. Returns 1 if a non-empty
 * selection exists, else 0. */
int mod_channel_get_selection(char* out, int out_sz);

/* Set the selected value token (dedup; wakes the HUD to re-mark the row). */
void mod_channel_set_selection(const char* value);

/* Set the quick-toggle row sub-label (wide, verbatim; dedup + wake). */
void mod_channel_set_sublabel(const wchar_t* text);

#ifdef __cplusplus
}
#endif

#endif /* MOD_LIST_CHANNEL_H */
