// castd.exe: the zune-cast daemon. The mod's `daemons` capability boot-spawns it
// (CreateProcessW, no args). Unlike the nativeapp plugin (RunDaemon, which shares
// nativeapp's Winsock and takes its target from the spawn arg), castd is its own
// process: it owns Winsock and reads its run-state from the ModStateBlock toggle.
//
// It lives for the device's uptime, but a cast session (capture + HTTP + TLS) runs
// only while the "Cast audio" toggle is on. The toggle is a discrete user action,
// so castd blocks on the ModStateBlock change event and reads the value on wake:
// a pushed edge, never polled.

#include "castaudio.h"
#include "cast_core.h"
#include "cast_keys.h"
#include "cast_channel.h"
#include "mdns.h"
#include "zdk.h"
#include <wolfssl/ssl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// CAST_TOGGLE_KEY (the control) and CAST_STATUS_KEY (the effect) + the status
// state constants come from cast_keys.h.

// Signalled when the toggle goes off, a mid-cast re-selection lands, or on
// shutdown, ending the live session.
static HANDLE g_session_stop = NULL;

// The target the running session dialed. The gate watcher compares the live
// selection against this to detect a mid-cast device change (hot-swap).
static char           g_run_ip[40] = { 0 };
static unsigned short g_run_port = 0;
// Set by the gate watcher when it ends a session because the user picked a
// different device; tells the main loop to reconnect (not treat it as an error).
static int            g_reselect = 0;

// The chosen receiver persists here across reboots ("ip:port"), so casting comes
// back to the same device without re-picking. Lives in the mod's own dir and is
// declared persistent in the manifest, so a mod update preserves it.
#define CAST_TARGET_FILE  L"\\flash2\\automation\\mods\\zune-cast\\target.txt"

static int load_persisted_target(char* ip, int ipsz, unsigned short* port)
{
    HANDLE f = CreateFileW(CAST_TARGET_FILE, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char  buf[64];
    DWORD rd = 0;
    char* colon;
    int   i;
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(f, buf, sizeof(buf) - 1, &rd, NULL) || rd == 0) { CloseHandle(f); return 0; }
    CloseHandle(f);
    buf[rd] = 0;
    for (i = (int)rd - 1; i >= 0 && (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t'); i--)
        buf[i] = 0;
    if (port) *port = CAST_CONTROL_PORT;
    colon = strchr(buf, ':');
    if (colon) {
        int p; *colon = 0; p = atoi(colon + 1);
        if (port && p > 0 && p < 65536) *port = (unsigned short)p;
    }
    _snprintf(ip, ipsz, "%s", buf); ip[ipsz - 1] = 0;
    return ip[0] ? 1 : 0;
}

static void persist_target(const char* ip, unsigned short port)
{
    char   line[64], cur_ip[40];
    unsigned short cur_p = 0;
    DWORD  wr = 0;
    HANDLE f;
    if (load_persisted_target(cur_ip, sizeof(cur_ip), &cur_p) && cur_p == port && strcmp(cur_ip, ip) == 0)
        return;   // unchanged: avoid a needless flash write
    _snprintf(line, sizeof(line), "%s:%u\r\n", ip, port);
    f = CreateFileW(CAST_TARGET_FILE, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, line, (DWORD)strlen(line), &wr, NULL);
    CloseHandle(f);
}

// Resolve the cast target by priority: the user's live selection (persisted on
// use), else the last persisted receiver. There is NO auto-pick: with neither a
// selection nor a remembered receiver this returns 0 and the caller waits for a
// pick, never dialing on its own. Records the chosen target so the gate watcher
// can spot a mid-cast change.
static int resolve_target(char* ip, int ipsz, unsigned short* out_port)
{
    char           sel[40];
    unsigned short p = CAST_CONTROL_PORT;
    if (cast_channel_get_selection(sel, sizeof(sel), &p)) {
        persist_target(sel, p);
    } else if (!load_persisted_target(sel, sizeof(sel), &p)) {
        return 0;
    }
    _snprintf(ip, ipsz, "%s", sel);
    ip[ipsz - 1] = 0;
    *out_port = p;
    _snprintf(g_run_ip, sizeof(g_run_ip), "%s", ip);
    g_run_port = p;
    // Reflect the active target as the selection so the picker marks it selected
    // even on a persisted auto-connect (where no live tap wrote sel_value).
    cast_channel_set_selection(ip, p);
    return 1;
}

// Background discovery worker (daemon lifetime). Scans once at startup for an
// initial list, then blocks on the HUD's scan-request event and re-scans on
// demand: no idle polling, so it holds no WiFi/battery when the picker is shut.
static DWORD WINAPI discovery_thread(LPVOID arg)
{
    HANDLE ev = cast_channel_scan_event();
    MdnsDevice devs[MODLISTCH_MAX_ROWS];
    (void)arg;
    for (;;) {
        // 5 s window with periodic re-queries: mDNS is lossy and some receivers
        // answer slowly, so a short single-query scan under-reports the LAN.
        int n = mdns_enumerate_chromecast(devs, MODLISTCH_MAX_ROWS, 5000);
        cast_channel_publish(devs, n);
        if (!ev) break;   // no trigger available: one-shot, degrade gracefully
        WaitForSingleObject(ev, INFINITE);
    }
    return 0;
}

// Block until a setting may have changed. The change event is a hard dependency
// (verified at startup), so there is no poll path: wait on it, plus session_stop
// when a session's watcher shares the wait.
static void wait_for_state_change(HANDLE also_stop)
{
    HANDLE evt = mod_state_change_event();
    if (also_stop) {
        HANDLE h[2]; h[0] = also_stop; h[1] = evt;
        WaitForMultipleObjects(2, h, FALSE, INFINITE);
    } else {
        WaitForSingleObject(evt, INFINITE);
    }
}

// Ends the running session the moment the toggle goes off, without waiting on a
// possibly-blocked TLS read in the control loop. Wakes on the change event (or
// session_stop), never polls.
static DWORD WINAPI gate_watch_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        if (WaitForSingleObject(g_session_stop, 0) == WAIT_OBJECT_0) break;
        if (mod_state_get_state(CAST_TOGGLE_KEY) == 0) {
            SetEvent(g_session_stop);
            break;
        }
        // Selection changed while casting -> end this session so the main loop
        // reconnects to the newly chosen receiver (hot-swap). g_reselect tells it
        // this was a deliberate switch, not a failure.
        {
            char           ip[40];
            unsigned short p = 0;
            if (cast_channel_get_selection(ip, sizeof(ip), &p) &&
                (strcmp(ip, g_run_ip) != 0 || p != g_run_port)) {
                g_reselect = 1;
                SetEvent(g_session_stop);
                break;
            }
        }
        wait_for_state_change(g_session_stop);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow)
{
    (void)hInstance; (void)hPrev; (void)lpCmdLine; (void)nShow;

    cast_log_init();
    mod_state_daemon_init(CAST_DAEMON_EVENT);
    mod_channel_init(CAST_TOGGLE_KEY);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        cast_log("WSA-STARTUP-FAIL");
        return 1;
    }
    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        cast_log("WOLF-INIT-FAIL");
        WSACleanup();
        return 1;
    }
    // The toggle change event is the daemon's wakeup source; fail fast if it
    // can't be created rather than degrade to polling.
    if (!mod_state_change_event()) {
        cast_log("STATE-EVT-FAIL");
        wolfSSL_Cleanup();
        WSACleanup();
        return 1;
    }
    if (!zdk_init()) cast_log("ZDK unavailable; metadata/controls disabled");

    // Background device discovery feeding the HUD picker; runs for the daemon's
    // life, independent of whether a cast session is active.
    { HANDLE hdisc = CreateThread(NULL, 0, discovery_thread, NULL, 0, NULL);
      if (hdisc) CloseHandle(hdisc); else cast_log("DISCOVERY-THREAD-FAIL"); }

    cast_log("CASTD-START toggle_key=%s tone=%d", CAST_TOGGLE_KEY, CAST_USE_TEST_TONE);
    cast_channel_set_sublabel(L"OFF");   // initial row sub-label until the toggle flips on

    int scan_sent = 0;   // one discovery request per no-device episode
    for (;;) {
        // Idle until casting is on: block on the change event, no poll.
        while (mod_state_get_state(CAST_TOGGLE_KEY) != 1) {
            scan_sent = 0;
            wait_for_state_change(NULL);
        }

        char           target[40];
        unsigned short ctrl_port = CAST_CONTROL_PORT;
        if (!resolve_target(target, sizeof(target), &ctrl_port)) {
            // No selection, no remembered receiver, none discovered yet. Ask the
            // discovery worker for a fresh scan once, then wait; never dial a
            // hardcoded address. A publish, a user selection, or a toggle wakes
            // us; scan_sent gates one request per episode so an empty result
            // can't spin a rescan loop.
            mod_state_set_status(CAST_STATUS_KEY, CAST_STATUS_CONNECTING);
            if (!scan_sent) {
                cast_channel_set_sublabel(L"SCANNING\x2026");   // animated … while discovering
                HANDLE se = cast_channel_scan_event();
                if (se) SetEvent(se);
                scan_sent = 1;
            } else {
                cast_channel_set_sublabel(L"HOLD TO SELECT");   // scan done, awaiting a pick
            }
            wait_for_state_change(NULL);
            continue;
        }
        scan_sent = 0;
        cast_channel_set_sublabel(L"CONNECTING\x2026");

        g_session_stop = CreateEventW(NULL, TRUE, FALSE, NULL);   // manual reset
        if (!g_session_stop) { cast_log("SESSION-EVT-FAIL"); break; }   // fatal
        g_reselect = 0;

        HANDLE hgate = CreateThread(NULL, 0, gate_watch_thread, NULL, 0, NULL);
        if (!hgate) {
            // Without the gate watcher a toggle-off can't end the session, which
            // would hold the WiFi lease indefinitely. Treat as fatal-for-session:
            // tear down and wait for the next change rather than hot-loop.
            cast_log("GATE-THREAD-FAIL");
            CloseHandle(g_session_stop);
            g_session_stop = NULL;
            wait_for_state_change(NULL);
            continue;
        }
        // castd holds no WiFi lease itself: the cast toggle declares
        // holds:["wifi_awake"], so the platform authority reads the toggle and
        // holds keepalive the whole time casting is on. No per-session
        // acquire/release to flap the link.

        // Publish the connecting status; cast_run_session bumps to "casting"
        // once the receiver link is live.
        mod_state_set_status(CAST_STATUS_KEY, CAST_STATUS_CONNECTING);

        cast_run_session(target, ctrl_port, CAST_MEDIA_PORT_DEFAULT, g_session_stop);

        SetEvent(g_session_stop);   // ensure the watcher exits even on a control-setup failure
        if (hgate) { WaitForSingleObject(hgate, 2000); CloseHandle(hgate); }
        CloseHandle(g_session_stop);
        g_session_stop = NULL;

        // Why did the session end?
        //  - toggle off      -> status off, idle until the next toggle-on.
        //  - re-selection     -> reconnect to the new target immediately (loop
        //                        back with the toggle still on); not an error.
        //  - toggle still on  -> it aborted abnormally (receiver unreachable /
        //                        stream dropped): surface error and wait for the
        //                        next change so a persistent failure can't hot-loop.
        if (mod_state_get_state(CAST_TOGGLE_KEY) != 1) {
            mod_state_set_status(CAST_STATUS_KEY, CAST_STATUS_OFF);
            cast_channel_set_sublabel(L"OFF");
        } else if (g_reselect) {
            cast_log("RESELECT -> reconnecting to new target");
            // fall through: the outer loop re-resolves and reconnects.
        } else {
            mod_state_set_status(CAST_STATUS_KEY, CAST_STATUS_ERROR);
            cast_channel_set_sublabel(L"ERROR");
            wait_for_state_change(NULL);
        }
    }

    zdk_shutdown();
    wolfSSL_Cleanup();
    WSACleanup();
    return 1;
}
