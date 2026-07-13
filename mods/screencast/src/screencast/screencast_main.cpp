/* screencastd.exe: the screencast daemon. The mod's `daemons` capability
 * boot-spawns it (CreateProcessW, no args). It owns its own Winsock and reads
 * its run-state from the ModStateBlock "Screen share" quick-toggle.
 *
 * The toggle's long-press picker chooses the frontend: Browser (MJPEG over HTTP,
 * the default) or Desktop (the binary delta protocol zune-screencast.py speaks).
 * The daemon runs only the selected frontend, and composes the quick-toggle row
 * sub-label to show where to reach it (e.g. "View at 192.168.0.100:8080").
 *
 * Serving runs only while the toggle is on. The toggle and the picker selection
 * are discrete user actions, so the daemon blocks on the ModStateBlock change
 * event and reacts on wake: a pushed edge, never polled. */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "screencast_engine.h"
#include "screencast_frontend.h"
#include "screencast_keys.h"
#include "mod_list_channel.h"

/* Connected-client count; drives the Ready/Live status. on_client runs on
 * frontend threads, so the count is interlocked. */
static volatile LONG g_clients = 0;
static void on_client(int active) {
    LONG n = active ? InterlockedIncrement(&g_clients) : InterlockedDecrement(&g_clients);
    mod_state_set_status(SC_STATUS_KEY, n > 0 ? SC_STATUS_LIVE : SC_STATUS_READY);
}

static HANDLE g_serve_stop = NULL;
static int    g_serve_mode = SC_MODE_BROWSER;

static DWORD WINAPI serve_thread(LPVOID p) {
    (void)p;
    if (g_serve_mode == SC_MODE_DESKTOP)
        sc_delta_run(SC_DELTA_PORT, SC_FRAME_MS, on_client, g_serve_stop);
    else
        sc_http_run(SC_HTTP_PORT, SC_FRAME_MS, SC_JPEG_Q, on_client, g_serve_stop);
    return 0;
}

/* Publish the two picker options once. Row value is the mode token the picker
 * writes back as the selection. */
static void publish_modes(void) {
    ModListChannelRow rows[2];
    memset(rows, 0, sizeof(rows));
    wcscpy(rows[0].name, L"Browser");
    wcscpy(rows[0].sub,  L"Watch in a web browser");
    _snprintf(rows[0].value, MODLISTCH_VAL_LEN, "%s", SC_MODE_VAL_BROWSER);
    wcscpy(rows[1].name, L"Desktop");
    wcscpy(rows[1].sub,  L"zune-screencast.py");
    _snprintf(rows[1].value, MODLISTCH_VAL_LEN, "%s", SC_MODE_VAL_DESKTOP);
    mod_channel_publish(rows, 2);
}

static int read_mode(void) {
    char token[MODLISTCH_VAL_LEN];
    if (mod_channel_get_selection(token, sizeof(token)) &&
        strcmp(token, SC_MODE_VAL_DESKTOP) == 0)
        return SC_MODE_DESKTOP;
    return SC_MODE_BROWSER;   /* default (empty or "browser") */
}

/* This device's WiFi IPv4 as a dotted string. "0.0.0.0" if unavailable. */
static void local_ip(char* out, int out_sz) {
    char host[128];
    struct hostent* he;
    _snprintf(out, out_sz, "%s", "0.0.0.0");
    out[out_sz - 1] = 0;
    if (gethostname(host, sizeof(host)) != 0) return;
    he = gethostbyname(host);
    if (he && he->h_addr_list && he->h_addr_list[0]) {
        struct in_addr a;
        memcpy(&a, he->h_addr_list[0], sizeof(a));
        _snprintf(out, out_sz, "%s", inet_ntoa(a));
        out[out_sz - 1] = 0;
    }
}

/* Compose the quick-toggle sub-label for the active mode: where to reach it. */
static void update_sublabel(int mode) {
    char  ip[40];
    char  line[96];
    wchar_t wide[MODLISTCH_SUBLABEL_LEN];
    int i;
    local_ip(ip, sizeof(ip));
    if (mode == SC_MODE_DESKTOP)
        _snprintf(line, sizeof(line), "App at %s:%d", ip, SC_DELTA_PORT);
    else
        _snprintf(line, sizeof(line), "View at %s:%d", ip, SC_HTTP_PORT);
    line[sizeof(line) - 1] = 0;
    for (i = 0; i < MODLISTCH_SUBLABEL_LEN - 1 && line[i]; i++) wide[i] = (wchar_t)(unsigned char)line[i];
    wide[i] = 0;
    mod_channel_set_sublabel(wide);
}

static void wait_change(void) {
    HANDLE e = mod_state_change_event();
    if (e) WaitForSingleObject(e, INFINITE);
    else   Sleep(200);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow) {
    (void)hInstance; (void)hPrev; (void)lpCmdLine; (void)nShow;

    mod_state_daemon_init(SC_DAEMON_EVENT);
    mod_channel_init(SC_TOGGLE_KEY);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { sc_log("screencastd: WSA-STARTUP-FAIL\n"); return 1; }
    if (!mod_state_change_event()) { sc_log("screencastd: STATE-EVT-FAIL\n"); WSACleanup(); return 1; }
    publish_modes();
    sc_log("screencastd: start\n");

    for (;;) {
        while (mod_state_get_state(SC_TOGGLE_KEY) != 1) wait_change();

        if (!sc_engine_ready()) { sc_log("screencastd: kerncore not ready\n"); wait_change(); continue; }
        sc_engine_init();

        int mode = read_mode();
        update_sublabel(mode);
        g_clients = 0;
        g_serve_mode = mode;
        mod_state_set_status(SC_STATUS_KEY, SC_STATUS_READY);

        g_serve_stop = CreateEventW(NULL, TRUE, FALSE, NULL);   /* manual reset */
        if (!g_serve_stop) {
            sc_log("screencastd: SERVE-EVT-FAIL\n");
            mod_state_set_status(SC_STATUS_KEY, SC_STATUS_OFF);
            wait_change();
            continue;
        }
        HANDLE st = CreateThread(NULL, 0, serve_thread, NULL, 0, NULL);
        sc_log(mode == SC_MODE_DESKTOP ? "screencastd: serving (desktop)\n"
                                       : "screencastd: serving (browser)\n");

        /* Hold until the toggle goes off or the picker switches the mode. */
        for (;;) {
            wait_change();
            if (mod_state_get_state(SC_TOGGLE_KEY) != 1) break;   /* toggled off */
            if (read_mode() != mode) break;                       /* mode switched */
        }

        SetEvent(g_serve_stop);
        if (st) { WaitForSingleObject(st, 10000); CloseHandle(st); }
        CloseHandle(g_serve_stop);
        g_serve_stop = NULL;

        if (mod_state_get_state(SC_TOGGLE_KEY) != 1) {
            mod_state_set_status(SC_STATUS_KEY, SC_STATUS_OFF);
            mod_channel_set_sublabel(L"");
            sc_log("screencastd: stopped\n");
        }
        /* else: mode switched while on; the loop re-serves with the new frontend. */
    }
    /* not reached */
}
