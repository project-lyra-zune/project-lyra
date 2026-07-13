#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "screencast_engine.h"
#include "screencast_frontend.h"

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x0001
#endif

#define FRAME_DELTA 0
#define FRAME_RAW   1

static int send_all(SOCKET s, const void* buf, int len) {
    const char* p = (const char*)buf;
    int sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, len - sent, 0);
        if (n == SOCKET_ERROR || n == 0) return -1;
        sent += n;
    }
    return 0;
}

typedef struct { SOCKET c; HANDLE stop; volatile int done; } ClientCtx;

/* Per-client input reader: full-duplex on the same socket as the frame stream.
 * Reads [u8 action][u16 x][u16 y] and injects. On idle it feeds the hold stream
 * so a held finger keeps registering as a long-press. */
static DWORD WINAPI input_thread(LPVOID p) {
    ClientCtx* cc = (ClientCtx*)p;
    /* Above the stream thread so a tap's kcalls win the kerncore lock promptly
     * instead of queuing behind the framebuffer read's kcalls. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    unsigned char cmd[5];
    while (!cc->done && WaitForSingleObject(cc->stop, 0) != WAIT_OBJECT_0) {
        fd_set rf; FD_ZERO(&rf); FD_SET(cc->c, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = sc_held() ? 140000 : 150000;
        int r = select(0, &rf, NULL, NULL, &tv);
        if (r <= 0) { if (r == 0) sc_feed_hold(); continue; }
        int got = 0;
        while (got < 5) {
            int n = recv(cc->c, (char*)cmd + got, 5 - got, 0);
            if (n <= 0) { cc->done = 1; break; }
            got += n;
        }
        if (got < 5) break;
        unsigned int x = (unsigned int)(cmd[1] | (cmd[2] << 8));
        unsigned int y = (unsigned int)(cmd[3] | (cmd[4] << 8));
        sc_inject(cmd[0], x, y);
    }
    return 0;
}

/* Stream RGB565 delta frames to one client until stop or disconnect. */
static void stream_client(SOCKET c, HANDLE stop, unsigned int frame_ms) {
    unsigned char*  argb = (unsigned char*)malloc(SC_FB_BYTES);
    unsigned short* cur  = (unsigned short*)malloc(SC_NPIX * sizeof(unsigned short));
    unsigned short* prev = (unsigned short*)malloc(SC_NPIX * sizeof(unsigned short));
    unsigned char*  enc  = (unsigned char*)malloc(SC_NPIX * 2 + 16);
    if (!argb || !cur || !prev || !enc) { free(argb); free(cur); free(prev); free(enc); return; }

    memset(prev, 0, SC_NPIX * sizeof(unsigned short));
    while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
        DWORD t0 = GetTickCount();
        if (!sc_capture(argb)) { Sleep(20); continue; }
        sc_argb_to_rgb565(argb, cur);

        unsigned char type;
        const unsigned char* payload;
        unsigned int payload_len;
        int n = sc_encode_delta(cur, prev, enc, (int)(SC_NPIX * 2));
        if (n >= 0) {
            type = FRAME_DELTA; payload = enc; payload_len = (unsigned int)n;
        } else {
            type = FRAME_RAW; payload = (const unsigned char*)cur; payload_len = SC_NPIX * 2;
        }
        memcpy(prev, cur, SC_NPIX * sizeof(unsigned short));

        unsigned char hdr[5];
        hdr[0] = type;
        hdr[1] = (unsigned char)(payload_len);       hdr[2] = (unsigned char)(payload_len >> 8);
        hdr[3] = (unsigned char)(payload_len >> 16);  hdr[4] = (unsigned char)(payload_len >> 24);
        if (send_all(c, hdr, 5)) break;
        if (send_all(c, payload, (int)payload_len)) break;

        DWORD el = GetTickCount() - t0;
        if (el < frame_ms) Sleep(frame_ms - el);
    }
    free(argb); free(cur); free(prev); free(enc);
}

int sc_delta_run(int port, unsigned int frame_ms, sc_client_cb on_client, HANDLE stop_event) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) return -1;
    { int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one)); }
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) { closesocket(ls); return -2; }
    if (listen(ls, 1) == SOCKET_ERROR) { closesocket(ls); return -3; }

    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        fd_set rf; FD_ZERO(&rf); FD_SET(ls, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int r = select(0, &rf, NULL, NULL, &tv);
        if (r <= 0) continue;
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        { int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one)); }

        if (on_client) on_client(1);
        ClientCtx cc; cc.c = c; cc.stop = stop_event; cc.done = 0;
        HANDLE ith = CreateThread(NULL, 0, input_thread, &cc, 0, NULL);
        stream_client(c, stop_event, frame_ms);
        cc.done = 1;
        closesocket(c);
        if (ith) { WaitForSingleObject(ith, 2000); CloseHandle(ith); }
        if (on_client) on_client(0);
    }
    closesocket(ls);
    return 0;
}
