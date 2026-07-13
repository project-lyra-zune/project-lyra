#include "castaudio.h"
#include "cast_core.h"
#include "mdns.h"
#include "zdk.h"
#include <wolfssl/ssl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// nativeapp loads this via opcode 21 with entry "RunDaemon". This DLL is the
// fast on-device iteration vehicle; the shipped modkit mod runs castd.exe
// (cast_main.cpp) instead. Both call the shared cast_run_session core. The
// process-wide Winsock instance is owned by nativeapp, so do NOT
// WSAStartup/WSACleanup here.
extern "C" __declspec(dllexport) int RunDaemon(const void* arg, int arg_len, HANDLE stop_event)
{
    cast_log_init();

    // arg (optional) = ASCII target "ip" or "ip:port". A bare ip uses the 8009
    // control port; the ":port" form selects a Cast group (dynamic receiver
    // port). Empty -> first discovered device (else bail; no fallback).
    char arg_ip[40] = { 0 };
    if (arg != NULL && arg_len > 0) {
        int n = (arg_len < (int)sizeof(arg_ip) - 1) ? arg_len : (int)sizeof(arg_ip) - 1;
        memcpy(arg_ip, arg, n); arg_ip[n] = '\0';
    }
    cast_log("RUN-START arg_target='%s' tone=%d", arg_ip, CAST_USE_TEST_TONE);

    if (!zdk_init()) cast_log("ZDK unavailable; metadata/controls disabled");

    // Target priority: arg override -> first mDNS responder. There is no
    // hardcoded fallback; with no arg and nothing discovered there is nothing to
    // cast to, so bail. The control port travels with the target.
    char target[40];
    char discovered[32];
    unsigned short discovered_port = 0;
    int got = mdns_discover_chromecast(discovered, sizeof(discovered), &discovered_port, 3000);
    unsigned short target_port = CAST_CONTROL_PORT;
    if (arg_ip[0]) {
        char* colon = strchr(arg_ip, ':');
        if (colon) {
            *colon = '\0';
            int p = atoi(colon + 1);
            if (p > 0 && p < 65536) target_port = (unsigned short)p;
        }
        _snprintf(target, sizeof(target), "%s", arg_ip);
        cast_log("CAST target=%s:%u (arg)", target, target_port);
    } else if (got) {
        _snprintf(target, sizeof(target), "%s", discovered);
        target_port = discovered_port;
        cast_log("CAST target=%s:%u (mdns first)", target, target_port);
    } else {
        cast_log("CAST no target (no arg, no mdns): nothing to cast to");
        zdk_shutdown();
        cast_log("RUN-EXIT");
        return 0;
    }

    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        cast_log("WOLF-INIT-FAIL");
    } else {
        cast_run_session(target, target_port, CAST_MEDIA_PORT_DEFAULT, stop_event);
        wolfSSL_Cleanup();
    }

    zdk_shutdown();
    cast_log("RUN-EXIT");
    return 0;
}
