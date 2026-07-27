/* screencastd.exe: the screencast daemon. The mod's `daemons` capability
 * boot-spawns it (CreateProcessW, no args). It owns its own Winsock and reads
 * its run-state from the "Screen share" quick-toggle slot.
 *
 * The toggle's long-press picker chooses the frontend: Browser (MJPEG over HTTP,
 * the default) or Desktop (the binary delta protocol zune-screencast.py speaks).
 * The daemon runs only the selected frontend, and composes the quick-toggle row
 * sub-label to show where to reach it (e.g. "View at 192.168.0.100:8080").
 *
 * Serving runs only while the toggle is on. The toggle and the picker selection
 * are discrete user actions, so the daemon blocks on the runtime's change
 * event and reacts on wake: a pushed edge, never polled. */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "screencast_engine.h"
#include "screencast_frontend.h"
#include "screencast_keys.h"
#include "lyra.h"

/* Connected-client count; drives the Ready/Live status. on_client runs on
 * frontend threads, so the count is interlocked. */
static volatile LONG g_clients = 0;
static void on_client(int active) {
    LONG n = active ? InterlockedIncrement(&g_clients) : InterlockedDecrement(&g_clients);
    lyra_state_set_status(SC_STATUS_KEY, n > 0 ? SC_STATUS_LIVE : SC_STATUS_READY);
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
    lyra_channel_stage_row(SC_TOGGLE_KEY, 0, L"Browser", L"Watch in a web browser", SC_MODE_VAL_BROWSER);
    lyra_channel_stage_row(SC_TOGGLE_KEY, 1, L"Desktop", L"zune-screencast.py",     SC_MODE_VAL_DESKTOP);
    lyra_channel_commit(SC_TOGGLE_KEY, 2);
}

static int read_mode(void) {
    char token[LYRA_CHANNEL_VALUE_MAX];
    if (lyra_channel_get_selection(SC_TOGGLE_KEY, token, sizeof(token)) &&
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
    wchar_t wide[LYRA_CHANNEL_SUBLABEL_MAX];
    int i;
    local_ip(ip, sizeof(ip));
    if (mode == SC_MODE_DESKTOP)
        _snprintf(line, sizeof(line), "App at %s:%d", ip, SC_DELTA_PORT);
    else
        _snprintf(line, sizeof(line), "View at %s:%d", ip, SC_HTTP_PORT);
    line[sizeof(line) - 1] = 0;
    for (i = 0; i < LYRA_CHANNEL_SUBLABEL_MAX - 1 && line[i]; i++) wide[i] = (wchar_t)(unsigned char)line[i];
    wide[i] = 0;
    lyra_channel_set_sublabel(SC_TOGGLE_KEY, wide);
}

static void wait_change(void) {
    HANDLE e = lyra_state_change_event(SC_DAEMON_EVENT);
    if (e) WaitForSingleObject(e, INFINITE);
    else   Sleep(200);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow) {
    (void)hInstance; (void)hPrev; (void)lpCmdLine; (void)nShow;


    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { sc_log("screencastd: WSA-STARTUP-FAIL"); return 1; }
    if (!lyra_state_change_event(SC_DAEMON_EVENT)) { sc_log("screencastd: STATE-EVT-FAIL"); WSACleanup(); return 1; }
    publish_modes();
    sc_log("screencastd: start");

    for (;;) {
        while (lyra_state_get(SC_TOGGLE_KEY) != 1) wait_change();

        if (!sc_engine_ready()) { sc_log("screencastd: kerncore not ready"); wait_change(); continue; }
        sc_engine_init();

        int mode = read_mode();
        update_sublabel(mode);
        g_clients = 0;
        g_serve_mode = mode;
        lyra_state_set_status(SC_STATUS_KEY, SC_STATUS_READY);

        g_serve_stop = CreateEventW(NULL, TRUE, FALSE, NULL);   /* manual reset */
        if (!g_serve_stop) {
            sc_log("screencastd: SERVE-EVT-FAIL");
            lyra_state_set_status(SC_STATUS_KEY, SC_STATUS_OFF);
            wait_change();
            continue;
        }
        HANDLE st = CreateThread(NULL, 0, serve_thread, NULL, 0, NULL);
        sc_log(mode == SC_MODE_DESKTOP ? "screencastd: serving (desktop)"
                                       : "screencastd: serving (browser)");

        /* Hold until the toggle goes off or the picker switches the mode. */
        for (;;) {
            wait_change();
            if (lyra_state_get(SC_TOGGLE_KEY) != 1) break;   /* toggled off */
            if (read_mode() != mode) break;                       /* mode switched */
        }

        SetEvent(g_serve_stop);
        if (st) { WaitForSingleObject(st, 10000); CloseHandle(st); }
        CloseHandle(g_serve_stop);
        g_serve_stop = NULL;

        if (lyra_state_get(SC_TOGGLE_KEY) != 1) {
            lyra_state_set_status(SC_STATUS_KEY, SC_STATUS_OFF);
            lyra_channel_set_sublabel(SC_TOGGLE_KEY, L"");
            sc_log("screencastd: stopped");
        }
        /* else: mode switched while on; the loop re-serves with the new frontend. */
    }
    /* not reached */
}
