#include <winsock2.h>
#include <windows.h>
#include "screencast_frontend.h"
#include "screencast_keys.h"

typedef struct { HANDLE stop; sc_client_cb cb; } FeCtx;

static DWORD WINAPI http_fe(LPVOID p) {
    FeCtx* c = (FeCtx*)p;
    sc_http_run(SC_HTTP_PORT, SC_FRAME_MS, SC_JPEG_Q, c->cb, c->stop);
    return 0;
}

static DWORD WINAPI delta_fe(LPVOID p) {
    FeCtx* c = (FeCtx*)p;
    sc_delta_run(SC_DELTA_PORT, SC_FRAME_MS, c->cb, c->stop);
    return 0;
}

void sc_serve(HANDLE stop_event, sc_client_cb cb) {
    FeCtx ctx;
    ctx.stop = stop_event;
    ctx.cb = cb;
    HANDLE h1 = CreateThread(NULL, 0, http_fe, &ctx, 0, NULL);
    HANDLE h2 = CreateThread(NULL, 0, delta_fe, &ctx, 0, NULL);
    WaitForSingleObject(stop_event, INFINITE);   /* both frontends poll stop_event */
    if (h1) { WaitForSingleObject(h1, 3000); CloseHandle(h1); }
    if (h2) { WaitForSingleObject(h2, 3000); CloseHandle(h2); }
}
