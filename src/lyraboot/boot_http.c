#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "boot_http.h"

#define IO_TIMEOUT_SEC   20
#define HDR_CAP          4096

/* SO_RCVTIMEO is accepted and ignored on this CE build. */
static int wait_readable(SOCKET s) {
    fd_set rd;
    struct timeval tv;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    tv.tv_sec = IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    return select((int)s + 1, &rd, NULL, NULL, &tv) > 0;
}

static int recv_timed(SOCKET s, char* buf, int cap) {
    if (!wait_readable(s)) return -1;
    return recv(s, buf, cap, 0);
}

static int split_url(const char* url, char* host, int host_cap, char* port, int port_cap,
                     char* path, int path_cap) {
    const char* p;
    const char* slash;
    const char* colon;
    int n;

    if (strncmp(url, "http://", 7) != 0) return 0;
    p = url + 7;

    slash = strchr(p, '/');
    n = slash ? (int)(slash - p) : (int)strlen(p);
    if (n <= 0 || n >= host_cap) return 0;

    colon = (const char*)memchr(p, ':', n);
    if (colon) {
        int hn = (int)(colon - p), pn = n - hn - 1;
        if (hn <= 0 || hn >= host_cap || pn <= 0 || pn >= port_cap) return 0;
        memcpy(host, p, hn); host[hn] = 0;
        memcpy(port, colon + 1, pn); port[pn] = 0;
    } else {
        memcpy(host, p, n); host[n] = 0;
        strncpy(port, "80", port_cap - 1); port[port_cap - 1] = 0;
    }

    if (!slash) {
        strncpy(path, "/", path_cap - 1); path[path_cap - 1] = 0;
    } else {
        if ((int)strlen(slash) >= path_cap) return 0;
        strcpy(path, slash);
    }
    return 1;
}

static SOCKET tcp_connect(const char* host, const char* port, int* err) {
    struct addrinfo hints, *res = NULL, *ai;
    SOCKET s = INVALID_SOCKET;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        *err = BOOT_HTTP_ERR_RESOLVE;
        return INVALID_SOCKET;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) *err = BOOT_HTTP_ERR_CONNECT;
    return s;
}

static const char* header_value(const char* hdrs, const char* name) {
    int nlen = (int)strlen(name);
    const char* p = hdrs;
    while (*p) {
        const char* eol = strstr(p, "\r\n");
        if (!eol) return NULL;
        if (eol - p > nlen && _strnicmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char* v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            return v;
        }
        p = eol + 2;
        if (p[0] == '\r' && p[1] == '\n') return NULL;
    }
    return NULL;
}

int boot_http_download(const char* url, const wchar_t* dest, unsigned long max_bytes,
                       boot_http_progress cb, void* ctx,
                       int* http_status, unsigned long* got_out) {
    char host[128], port[8], path[512];
    char req[768];
    char hdr[HDR_CAP];
    static char buf[8192];
    SOCKET s;
    HANDLE hf;
    const char* clen_v;
    const char* body;
    int hdr_len = 0, status = 0, body_prefix, n, rv = BOOT_HTTP_OK;
    unsigned long total, got = 0;
    DWORD w;

    if (http_status) *http_status = 0;
    if (got_out) *got_out = 0;

    if (!split_url(url, host, sizeof(host), port, sizeof(port), path, sizeof(path)))
        return BOOT_HTTP_ERR_URL;

    s = tcp_connect(host, port, &rv);
    if (s == INVALID_SOCKET) return rv;

    _snprintf(req, sizeof(req),
              "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: lyraboot/1\r\n"
              "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    req[sizeof(req) - 1] = 0;
    n = (int)strlen(req);
    if (send(s, req, n, 0) != n) { closesocket(s); return BOOT_HTTP_ERR_SEND; }

    for (;;) {
        if (hdr_len >= HDR_CAP - 1) { closesocket(s); return BOOT_HTTP_ERR_RESPONSE; }
        n = recv_timed(s, hdr + hdr_len, HDR_CAP - 1 - hdr_len);
        if (n <= 0) { closesocket(s); return BOOT_HTTP_ERR_RECV; }
        hdr_len += n;
        hdr[hdr_len] = 0;
        if (strstr(hdr, "\r\n\r\n")) break;
    }

    if (strncmp(hdr, "HTTP/1.", 7) != 0 || hdr_len < 12) { closesocket(s); return BOOT_HTTP_ERR_RESPONSE; }
    status = atoi(hdr + 9);
    if (http_status) *http_status = status;
    if (status != 200) { closesocket(s); return BOOT_HTTP_ERR_STATUS; }

    clen_v = header_value(hdr, "Content-Length");
    if (!clen_v) { closesocket(s); return BOOT_HTTP_ERR_NO_LENGTH; }
    total = (unsigned long)strtoul(clen_v, NULL, 10);
    if (total == 0 || total > max_bytes) { closesocket(s); return BOOT_HTTP_ERR_TOO_LARGE; }

    body = strstr(hdr, "\r\n\r\n") + 4;
    body_prefix = hdr_len - (int)(body - hdr);

    hf = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { closesocket(s); return BOOT_HTTP_ERR_FILE; }

    if (body_prefix > 0) {
        if (!WriteFile(hf, body, body_prefix, &w, NULL)) rv = BOOT_HTTP_ERR_FILE;
        got = (unsigned long)body_prefix;
        if (cb) cb(got, total, ctx);
    }

    while (rv == BOOT_HTTP_OK && got < total) {
        unsigned long want = total - got;
        int cap = (want < sizeof(buf)) ? (int)want : (int)sizeof(buf);
        n = recv_timed(s, buf, cap);
        if (n == 0) { rv = BOOT_HTTP_ERR_SHORT; break; }
        if (n < 0)  { rv = BOOT_HTTP_ERR_RECV;  break; }
        if (!WriteFile(hf, buf, n, &w, NULL) || w != (DWORD)n) { rv = BOOT_HTTP_ERR_FILE; break; }
        got += (unsigned long)n;
        if (cb) cb(got, total, ctx);
    }

    CloseHandle(hf);
    closesocket(s);

    if (rv == BOOT_HTTP_OK && got != total) rv = BOOT_HTTP_ERR_SHORT;
    if (got_out) *got_out = got;
    if (rv != BOOT_HTTP_OK) DeleteFileW(dest);
    return rv;
}

const char* boot_http_result_str(int r) {
    switch (r) {
        case BOOT_HTTP_OK:            return "OK";
        case BOOT_HTTP_ERR_URL:       return "ERR_URL";
        case BOOT_HTTP_ERR_RESOLVE:   return "ERR_RESOLVE";
        case BOOT_HTTP_ERR_CONNECT:   return "ERR_CONNECT";
        case BOOT_HTTP_ERR_SEND:      return "ERR_SEND";
        case BOOT_HTTP_ERR_RECV:      return "ERR_RECV";
        case BOOT_HTTP_ERR_RESPONSE:  return "ERR_RESPONSE";
        case BOOT_HTTP_ERR_STATUS:    return "ERR_STATUS";
        case BOOT_HTTP_ERR_NO_LENGTH: return "ERR_NO_LENGTH";
        case BOOT_HTTP_ERR_TOO_LARGE: return "ERR_TOO_LARGE";
        case BOOT_HTTP_ERR_SHORT:     return "ERR_SHORT";
        case BOOT_HTTP_ERR_FILE:      return "ERR_FILE";
        default:                      return "ERR_UNKNOWN";
    }
}
