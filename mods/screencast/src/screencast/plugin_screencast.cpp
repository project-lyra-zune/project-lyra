/* plugin-screencast.dll: the nativeapp RunDaemon entry over the screencast
 * engine. The reboot-free iteration vehicle and the SDK's screen-mirror plugin
 * example. It has no quick-toggle or picker, so it runs BOTH frontends (browser
 * MJPEG + the delta protocol) until stop_event, letting a dev reach it from a
 * browser or zune-screencast.py. The shipped mod (screencastd.exe) is the
 * picker-driven, toggle-gated product; only it ships in the .zmod.
 *
 * Winsock is owned by nativeapp: do NOT WSAStartup/WSACleanup here. */

#include <winsock2.h>
#include <windows.h>
#include "screencast_engine.h"
#include "screencast_frontend.h"

extern "C" __declspec(dllexport) int RunDaemon(const void* arg, int arg_len, HANDLE stop_event) {
    (void)arg; (void)arg_len;
    if (!sc_engine_ready()) return -1;
    sc_engine_init();
    sc_serve(stop_event, NULL);   /* both frontends; no status (no toggle owner) */
    return 0;
}
