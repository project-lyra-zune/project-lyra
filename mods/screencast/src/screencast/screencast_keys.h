/* screencast's ModStateBlock keys and daemon config. The daemon logic lives in
 * the shared src/mod-runtime/mod_state; only these keys are mod-specific. */
#ifndef SCREENCAST_KEYS_H
#define SCREENCAST_KEYS_H

#include "mod_state.h"

/* The share control (intent) and status (effect) ModStateBlock keys. */
#define SC_TOGGLE_KEY  "setting/screencast/screencast"
#define SC_STATUS_KEY  "status/screencast/sharing"

/* Frontend mode, chosen from the quick-toggle long-press picker. The daemon runs
 * only the selected frontend. Browser is the default (empty selection). */
#define SC_MODE_BROWSER  0
#define SC_MODE_DESKTOP  1
#define SC_MODE_VAL_BROWSER  "browser"
#define SC_MODE_VAL_DESKTOP  "desktop"

/* status/screencast/sharing state values (must match the manifest status[].states
 * order: ["Off","Ready","Live"]). */
#define SC_STATUS_OFF    0
#define SC_STATUS_READY  1   /* server up, no client connected */
#define SC_STATUS_LIVE   2   /* a browser or desktop client is streaming */

/* screencast's unique daemon wake-event name (mod_state_daemon_init). */
#define SC_DAEMON_EVENT  L"zune-mod-state-evt-screencastd"

/* Serving config. HTTP for browsers, the delta protocol for zune-screencast.py. */
#define SC_HTTP_PORT   8080
#define SC_DELTA_PORT  1339
#define SC_FRAME_MS    100u   /* frame-rate cap (~10 fps) */
#define SC_JPEG_Q      60u    /* MJPEG quality 0..100 (bandwidth vs sharpness) */

#endif /* SCREENCAST_KEYS_H */
