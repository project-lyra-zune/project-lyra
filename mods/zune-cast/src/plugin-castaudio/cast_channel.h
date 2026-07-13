#ifndef CAST_CHANNEL_H
#define CAST_CHANNEL_H

#include <windows.h>
#include "mod_list_channel.h"   /* the generic picker channel + its ABI */
#include "mdns.h"               /* MdnsDevice */

#ifdef __cplusplus
extern "C" {
#endif

/* Cast-specific wrappers over the shared mod_list_channel: the receiver picker.
 * The channel is bound to CAST_TOGGLE_KEY via mod_channel_init at daemon startup;
 * these adapt discovered MdnsDevices to rows and "ip:port" selection tokens. */

/* The scan-request event the HUD signals when the picker opens. */
HANDLE cast_channel_scan_event(void);

/* Publish the discovered device list (friendly name widened; value = "ip:port"),
 * unioned with the previously-published list so a lossy mDNS scan does not drop a
 * device still on the LAN. */
void cast_channel_publish(const MdnsDevice* devs, int n);

/* Read the user-selected target. Parses the selection token ("ip" or "ip:port")
 * into out_ip / *out_port (8009 if no ":port"). Returns 1 if a non-empty
 * selection exists, else 0. */
int cast_channel_get_selection(char* out_ip, int out_ip_sz, unsigned short* out_port);

/* Friendly name of the published row whose value matches `ip`/`port`. Returns 1
 * if found, else 0. */
int cast_channel_name_for_target(const char* ip, unsigned short port,
                                 wchar_t* out_name, int out_name_len);

/* Set the quick-toggle row sub-label (already uppercase; end with an ellipsis for
 * the animated loading style). */
void cast_channel_set_sublabel(const wchar_t* text);

/* Record the active target ("ip:port") as the selection so the picker marks it,
 * needed on a persisted auto-connect where no live tap wrote the selection. */
void cast_channel_set_selection(const char* ip, unsigned short port);

#ifdef __cplusplus
}
#endif

#endif /* CAST_CHANNEL_H */
