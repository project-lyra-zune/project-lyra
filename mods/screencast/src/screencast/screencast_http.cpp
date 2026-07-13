#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screencast_engine.h"
#include "screencast_frontend.h"
#include "ce_image.h"
#include "viewer_html.h"

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x0001
#endif

#define MJPEG_BOUNDARY "zscframe"

static int send_all(SOCKET s, const char* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

typedef struct {
    SOCKET       c;
    HANDLE       stop;
    unsigned int frame_ms;
    unsigned int jpeg_q;
    sc_client_cb on_client;
} HttpConn;

/* Read the request head (up to cap-1 bytes) until the CRLFCRLF terminator, so
 * the request line and any small body are in one buffer. Returns bytes read. */
static int read_request(SOCKET c, char* buf, int cap) {
    int n = 0;
    while (n < cap - 1) {
        int r = recv(c, buf + n, cap - 1 - n, 0);
        if (r <= 0) break;
        n += r;
        buf[n] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[n] = 0;
    return n;
}

static void serve_page(SOCKET c) {
    char hdr[160];
    int hn = _snprintf(hdr, sizeof(hdr),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/html; charset=utf-8\r\n"
                       "Content-Length: %u\r\n"
                       "Connection: close\r\n\r\n",
                       (unsigned int)(sizeof(SC_VIEWER_HTML) - 1));
    if (hn > 0 && send_all(c, hdr, hn) == 0)
        send_all(c, SC_VIEWER_HTML, (int)(sizeof(SC_VIEWER_HTML) - 1));
}

static void serve_404(SOCKET c) {
    static const char* r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                           "Connection: close\r\n\r\n";
    send_all(c, r, (int)strlen(r));
}

/* POST /tap body: "a=<action>&p=<x> <y>" (plain text, not URL-encoded). */
static void handle_tap(SOCKET c, const char* buf) {
    const char* body = strstr(buf, "\r\n\r\n");
    static const char* ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                            "Connection: close\r\n\r\n";
    if (body) {
        body += 4;
        const char* pa = strstr(body, "a=");
        const char* pp = strstr(body, "p=");
        if (pa && pp) {
            int action = atoi(pa + 2);
            int x = 0, y = 0;
            if (sscanf(pp + 2, "%d %d", &x, &y) == 2 && action >= 0 && action <= 3)
                sc_inject((unsigned char)action, (unsigned int)x, (unsigned int)y);
        }
    }
    send_all(c, ok, (int)strlen(ok));
}

static void serve_mjpeg(HttpConn* h) {
    static const char* hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n\r\n";
    if (send_all(h->c, hdr, (int)strlen(hdr)) != 0) return;

    unsigned char* argb = (unsigned char*)malloc(SC_FB_BYTES);
    if (!argb) return;

    if (h->on_client) h->on_client(1);
    while (WaitForSingleObject(h->stop, 0) != WAIT_OBJECT_0) {
        DWORD t0 = GetTickCount();
        if (!sc_capture(argb)) { Sleep(20); continue; }

        unsigned char* jpg = NULL;
        unsigned int jlen = 0;
        if (ce_image_encode_jpeg(argb, SC_FB_W, SC_FB_H, SC_FB_STRIDE, h->jpeg_q, &jpg, &jlen) != CE_IMAGE_OK) {
            Sleep(20);
            continue;
        }
        char part[128];
        int pn = _snprintf(part, sizeof(part),
                           "--" MJPEG_BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                           "Content-Length: %u\r\n\r\n", jlen);
        int bad = (pn <= 0) || send_all(h->c, part, pn)
               || send_all(h->c, (const char*)jpg, (int)jlen)
               || send_all(h->c, "\r\n", 2);
        ce_image_free(jpg);
        if (bad) break;

        DWORD el = GetTickCount() - t0;
        if (el < h->frame_ms) Sleep(h->frame_ms - el);
    }
    free(argb);
    if (h->on_client) h->on_client(0);
}

static DWORD WINAPI conn_thread(LPVOID p) {
    HttpConn* h = (HttpConn*)p;
    char buf[1536];
    int n = read_request(h->c, buf, sizeof(buf));
    if (n > 0) {
        if      (!strncmp(buf, "GET /stream", 11)) serve_mjpeg(h);
        else if (!strncmp(buf, "POST /tap", 9))    handle_tap(h->c, buf);
        else if (!strncmp(buf, "GET / ", 6) ||
                 !strncmp(buf, "GET /index", 10) ||
                 !strncmp(buf, "GET / HTTP", 10)) serve_page(h->c);
        else serve_404(h->c);
    }
    closesocket(h->c);
    free(h);
    return 0;
}

int sc_http_run(int port, unsigned int frame_ms, unsigned int jpeg_q,
                sc_client_cb on_client, HANDLE stop_event) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) return -1;
    { int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one)); }
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) { closesocket(ls); return -2; }
    if (listen(ls, 4) == SOCKET_ERROR) { closesocket(ls); return -3; }

    /* Track live connection threads so they are all joined before returning:
     * a detached MJPEG streamer must not outlive the stop event the caller owns.
     * MAX_CONN bounds concurrent connections (a persistent /stream plus a few
     * transient /tap POSTs is the normal shape). */
#define MAX_CONN 8
    HANDLE conns[MAX_CONN];
    int nconn = 0, i;
    for (i = 0; i < MAX_CONN; i++) conns[i] = NULL;

    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        for (i = 0; i < nconn; ) {                       /* reap finished threads */
            if (WaitForSingleObject(conns[i], 0) == WAIT_OBJECT_0) {
                CloseHandle(conns[i]);
                conns[i] = conns[--nconn];
            } else i++;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int r = select(0, &rf, NULL, NULL, &tv);
        if (r <= 0) continue;
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        {
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        }
        if (nconn >= MAX_CONN) { closesocket(c); continue; }   /* over cap: refuse */
        HttpConn* h = (HttpConn*)malloc(sizeof(HttpConn));
        if (!h) { closesocket(c); continue; }
        h->c = c; h->stop = stop_event; h->frame_ms = frame_ms;
        h->jpeg_q = jpeg_q; h->on_client = on_client;
        HANDLE t = CreateThread(NULL, 0, conn_thread, h, 0, NULL);
        if (t) conns[nconn++] = t;
        else { closesocket(c); free(h); }
    }
    closesocket(ls);                                     /* stop accepting */
    for (i = 0; i < nconn; i++) {                        /* join outstanding connections */
        WaitForSingleObject(conns[i], 4000);
        CloseHandle(conns[i]);
    }
    return 0;
#undef MAX_CONN
}
