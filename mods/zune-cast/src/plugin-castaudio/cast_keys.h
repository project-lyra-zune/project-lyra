/* zune-cast's ModStateBlock keys and status vocabulary. The daemon logic lives
 * in the shared src/mod-runtime/mod_state; only these keys are mod-specific. */
#ifndef CAST_KEYS_H
#define CAST_KEYS_H

#include "mod_state.h"

/* The cast control (intent) and status (effect) ModStateBlock keys. */
#define CAST_TOGGLE_KEY  "setting/zune-cast/cast"
#define CAST_STATUS_KEY  "status/zune-cast/casting"

/* status/zune-cast/casting state values (must match the manifest status[].states
 * order: ["Off","Connecting","Connected","Casting","Error"]). */
#define CAST_STATUS_OFF         0
#define CAST_STATUS_CONNECTING  1
#define CAST_STATUS_CONNECTED   2   /* receiver link up, no active queue */
#define CAST_STATUS_CASTING     3   /* active media session (queue playing) */
#define CAST_STATUS_ERROR       4   /* connect failure / communication failure */

/* zune-cast's unique daemon wake-event name (mod_state_daemon_init). */
#define CAST_DAEMON_EVENT  L"zune-mod-state-evt-castd"

#endif /* CAST_KEYS_H */
