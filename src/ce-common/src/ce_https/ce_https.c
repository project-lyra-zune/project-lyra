/* ce_https.c: standalone synchronous HTTPS client (see ce_https.h).
 *
 * One request per call: resolve, TCP connect, blocking TLS handshake, write
 * the request, read the whole response (Connection: close), de-chunk if the
 * server used Transfer-Encoding: chunked, split status/headers/body. The
 * shared verifying TLS context (ce_tls_ctx) is built once and cached for
 * process lifetime. */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <wolfssl/ssl.h>

#include "ce_tls_ctx.h"
#include "ce_https.h"

/* Recv/send timeout (ms) so a stalled peer can't hang the calling thread. */
#define CE_HTTPS_IO_TIMEOUT_MS 20000

static int          g_wsa_up = 0;
static WOLFSSL_CTX *g_ctx    = NULL;
static int          g_last_tls_err = 0;   /* wolfSSL_get_error of the last failed handshake */

int ce_https_last_tls_error(void) { return g_last_tls_err; }

/* ── per-host TLS session resumption ───────────────────────────────────────
 * The TLS handshake's asymmetric crypto (ECDHE + cert verify) is the dominant
 * cost on this CPU: ~1.5-2s per request. The build has HAVE_SESSION_TICKET +
 * TLS 1.3, so caching the negotiated session and resuming it on the next
 * connection to the same host skips that crypto (the 2nd+ request to a host
 * becomes ~an RTT). Native wolfSSL API (OPENSSL_EXTRA is off, so no get1):
 * wolfSSL_get_session returns a cache-owned pointer, never freed here, just
 * cached. A consumer talks to one or two hosts serially, so the small internal
 * session cache never churns and the pointer stays valid between requests. */
#define CE_SESS_HOSTS 4
static struct { char host[256]; WOLFSSL_SESSION *sess; } g_sess[CE_SESS_HOSTS];

static WOLFSSL_SESSION *sess_get(const char *host)
{
    int i;
    for (i = 0; i < CE_SESS_HOSTS; i++) {
        if (g_sess[i].sess != NULL && strcmp(g_sess[i].host, host) == 0) {
            return g_sess[i].sess;
        }
    }
    return NULL;
}

static void sess_put(const char *host, WOLFSSL_SESSION *s)
{
    int i, slot = -1;
    if (s == NULL) return;
    for (i = 0; i < CE_SESS_HOSTS; i++) {
        if (g_sess[i].sess != NULL && strcmp(g_sess[i].host, host) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        for (i = 0; i < CE_SESS_HOSTS; i++) { if (g_sess[i].sess == NULL) { slot = i; break; } }
    }
    if (slot < 0) slot = 0;
    g_sess[slot].sess = s;   /* cache-owned (native get_session); do not free */
    _snprintf(g_sess[slot].host, sizeof(g_sess[slot].host), "%s", host);
}

/* Last request's phase timing (ms), for latency diagnosis. */
static int g_t_connect = 0, g_t_tls = 0, g_t_recv = 0;
void ce_https_last_timing(int *connect_ms, int *tls_ms, int *recv_ms)
{
    if (connect_ms) *connect_ms = g_t_connect;
    if (tls_ms)     *tls_ms     = g_t_tls;
    if (recv_ms)    *recv_ms    = g_t_recv;
}

static enum ce_https_result ensure_globals(void)
{
    if (!g_wsa_up) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return CE_HTTPS_ERR_INIT;
        }
        g_wsa_up = 1;
    }
    if (g_ctx == NULL) {
        g_ctx = ce_tls_client_ctx_create();
        if (g_ctx == NULL) {
            return CE_HTTPS_ERR_INIT;
        }
    }
    return CE_HTTPS_OK;
}

/* ── growable byte buffer ──────────────────────────────────────────────── */

struct buf {
    char  *p;
    size_t len;
    size_t cap;
};

static int buf_reserve(struct buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) {
        return 1;
    }
    {
        size_t ncap = b->cap ? b->cap : 8192;
        char  *np;
        while (ncap < b->len + extra + 1) {
            ncap *= 2;
        }
        np = (char *)realloc(b->p, ncap);
        if (np == NULL) {
            return 0;
        }
        b->p = np;
        b->cap = ncap;
    }
    return 1;
}

static int buf_append(struct buf *b, const void *data, size_t n)
{
    if (!buf_reserve(b, n)) {
        return 0;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 1;
}

/* ── transport ─────────────────────────────────────────────────────────── */

static SOCKET tcp_connect(const char *host, enum ce_https_result *err)
{
    struct addrinfo hints, *res = NULL, *ai;
    SOCKET s = INVALID_SOCKET;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* CE getaddrinfo: IPv4 only path */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(host, "443", &hints, &res);
    if (rc != 0 || res == NULL) {
        *err = CE_HTTPS_ERR_RESOLVE;
        return INVALID_SOCKET;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) {
            continue;
        }
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            break;
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) {
        *err = CE_HTTPS_ERR_CONNECT;
        return INVALID_SOCKET;
    }
    {
        DWORD tmo = CE_HTTPS_IO_TIMEOUT_MS;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
    }
    return s;
}

static int tls_write_all(WOLFSSL *ssl, const char *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int w = wolfSSL_write(ssl, p + off, (int)(n - off));
        if (w <= 0) {
            return 0;
        }
        off += (size_t)w;
    }
    return 1;
}

/* ── response post-processing ──────────────────────────────────────────── */

/* In-place de-chunk of an HTTP/1.1 chunked body. Returns new length, or
 * (size_t)-1 on malformed input. */
static size_t dechunk(char *body, size_t len)
{
    size_t in = 0, out = 0;
    while (in < len) {
        /* parse hex chunk size up to CRLF */
        size_t sz = 0;
        int saw_digit = 0;
        while (in < len && body[in] != '\r' && body[in] != ';') {
            char c = body[in++];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return (size_t)-1;
            sz = sz * 16 + (size_t)d;
            saw_digit = 1;
        }
        if (!saw_digit) return (size_t)-1;
        /* skip any chunk extension to CRLF */
        while (in < len && body[in] != '\r') in++;
        if (in + 1 >= len || body[in] != '\r' || body[in + 1] != '\n') {
            return (size_t)-1;
        }
        in += 2;
        if (sz == 0) {
            break;                       /* last chunk */
        }
        if (in + sz > len) {
            return (size_t)-1;
        }
        memmove(body + out, body + in, sz);
        out += sz;
        in += sz;
        if (in + 1 < len && body[in] == '\r' && body[in + 1] == '\n') {
            in += 2;                     /* chunk-data trailing CRLF */
        }
    }
    body[out] = '\0';
    return out;
}

static int header_has(const char *headers, const char *name, const char *val)
{
    /* case-insensitive substring search for "name: ... val" on one line */
    const char *line = headers;
    size_t nlen = strlen(name);
    while (line != NULL && *line != '\0') {
        if (_strnicmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char *eol = strstr(line, "\r\n");
            size_t llen = eol ? (size_t)(eol - line) : strlen(line);
            /* crude: search val within this header line */
            {
                char tmp[256];
                size_t cp = llen < sizeof(tmp) - 1 ? llen : sizeof(tmp) - 1;
                size_t i;
                memcpy(tmp, line, cp);
                tmp[cp] = '\0';
                for (i = 0; tmp[i]; i++) {
                    if (tmp[i] >= 'A' && tmp[i] <= 'Z') tmp[i] = (char)(tmp[i] + 32);
                }
                if (strstr(tmp, val) != NULL) return 1;
            }
        }
        line = strstr(line, "\r\n");
        if (line) line += 2;
    }
    return 0;
}

/* ── public entry ──────────────────────────────────────────────────────── */

enum ce_https_result ce_https_request(const char *host,
                                      const char *path,
                                      const char *method,
                                      const char *extra_hdrs,
                                      const void *body,
                                      size_t body_len,
                                      const char *content_type,
                                      struct ce_https_response *out)
{
    enum ce_https_result rv;
    SOCKET sock = INVALID_SOCKET;
    WOLFSSL *ssl = NULL;
    struct buf req, resp;
    char hdr[512];
    char *sep;

    memset(out, 0, sizeof(*out));
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    rv = ensure_globals();
    if (rv != CE_HTTPS_OK) {
        return rv;
    }

    {
        DWORD t0 = GetTickCount();
        sock = tcp_connect(host, &rv);
        g_t_connect = (int)(GetTickCount() - t0);
    }
    if (sock == INVALID_SOCKET) {
        return rv;
    }

    ssl = wolfSSL_new(g_ctx);
    if (ssl == NULL) { rv = CE_HTTPS_ERR_INIT; goto done; }
    wolfSSL_set_fd(ssl, (int)sock);
    wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, (unsigned short)strlen(host));
    wolfSSL_check_domain_name(ssl, host);
    {
        WOLFSSL_SESSION *cached = sess_get(host);   /* resume → skip the handshake crypto */
        if (cached != NULL) wolfSSL_set_session(ssl, cached);
    }
    {
        DWORD t0 = GetTickCount();
        int hs = wolfSSL_connect(ssl);
        g_t_tls = (int)(GetTickCount() - t0);
        if (hs != WOLFSSL_SUCCESS) {
            g_last_tls_err = wolfSSL_get_error(ssl, -1);
            rv = CE_HTTPS_ERR_TLS;
            goto done;
        }
    }

    /* Build request. Connection: close lets us read until EOF; identity
     * encoding keeps the body un-gzipped. */
    /* Transport-level headers only. User-Agent and any API headers are the
     * caller's concern (passed via extra_hdrs); YouTube's ANDROID/IOS clients
     * are UA-sensitive, so the transport must not impose one. */
    _snprintf(hdr, sizeof(hdr),
              "%s %s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "Accept-Encoding: identity\r\n"
              "Connection: close\r\n",
              method, path, host);
    if (!buf_append(&req, hdr, strlen(hdr))) { rv = CE_HTTPS_ERR_NOMEM; goto done; }
    if (extra_hdrs != NULL && extra_hdrs[0] != '\0') {
        if (!buf_append(&req, extra_hdrs, strlen(extra_hdrs)) ||
            !buf_append(&req, "\r\n", 2)) { rv = CE_HTTPS_ERR_NOMEM; goto done; }
    }
    if (body != NULL && body_len > 0) {
        _snprintf(hdr, sizeof(hdr),
                  "Content-Type: %s\r\nContent-Length: %lu\r\n",
                  content_type ? content_type : "application/json",
                  (unsigned long)body_len);
        if (!buf_append(&req, hdr, strlen(hdr))) { rv = CE_HTTPS_ERR_NOMEM; goto done; }
    }
    if (!buf_append(&req, "\r\n", 2)) { rv = CE_HTTPS_ERR_NOMEM; goto done; }
    if (body != NULL && body_len > 0) {
        if (!buf_append(&req, body, body_len)) { rv = CE_HTTPS_ERR_NOMEM; goto done; }
    }

    if (!tls_write_all(ssl, req.p, req.len)) { rv = CE_HTTPS_ERR_SEND; goto done; }

    /* Read the whole response until clean close. */
    {
        DWORD t0 = GetTickCount();
        for (;;) {
            char tmp[4096];
            int r = wolfSSL_read(ssl, tmp, sizeof(tmp));
            if (r > 0) {
                if (!buf_append(&resp, tmp, (size_t)r)) { rv = CE_HTTPS_ERR_NOMEM; goto done; }
                continue;
            }
            {
                int e = wolfSSL_get_error(ssl, r);
                if (r == 0 || e == WOLFSSL_ERROR_ZERO_RETURN || e == SOCKET_PEER_CLOSED_E) {
                    break;                   /* clean EOF */
                }
                /* Any data already received is still usable; only fail hard if we
                 * got nothing at all. */
                if (resp.len == 0) { rv = CE_HTTPS_ERR_RECV; goto done; }
                break;
            }
        }
        g_t_recv = (int)(GetTickCount() - t0);
    }

    if (resp.len == 0) { rv = CE_HTTPS_ERR_RECV; goto done; }

    /* Cache the negotiated session for resumption. Captured after the full read so
     * the TLS 1.3 NewSessionTicket (which arrives post-handshake) is included. */
    sess_put(host, wolfSSL_get_session(ssl));

    /* Split header block from body. */
    sep = NULL;
    {
        size_t i;
        for (i = 0; i + 3 < resp.len; i++) {
            if (resp.p[i] == '\r' && resp.p[i + 1] == '\n' &&
                resp.p[i + 2] == '\r' && resp.p[i + 3] == '\n') {
                sep = resp.p + i;
                break;
            }
        }
    }
    if (sep == NULL) { rv = CE_HTTPS_ERR_PROTOCOL; goto done; }

    /* Parse status code from "HTTP/1.x NNN ...". */
    if (strncmp(resp.p, "HTTP/1.", 7) != 0) { rv = CE_HTTPS_ERR_PROTOCOL; goto done; }
    {
        const char *sp = strchr(resp.p, ' ');
        if (sp == NULL) { rv = CE_HTTPS_ERR_PROTOCOL; goto done; }
        out->status = atoi(sp + 1);
    }

    /* Copy headers (status line + header lines, no trailing CRLFCRLF). */
    out->headers_len = (size_t)(sep - resp.p);
    out->headers = (char *)malloc(out->headers_len + 1);
    if (out->headers == NULL) { rv = CE_HTTPS_ERR_NOMEM; goto done; }
    memcpy(out->headers, resp.p, out->headers_len);
    out->headers[out->headers_len] = '\0';

    /* Copy body (after the CRLFCRLF). */
    {
        char  *bstart = sep + 4;
        size_t blen   = resp.len - (size_t)(bstart - resp.p);
        out->body = (char *)malloc(blen + 1);
        if (out->body == NULL) { rv = CE_HTTPS_ERR_NOMEM; goto done; }
        memcpy(out->body, bstart, blen);
        out->body[blen] = '\0';
        out->body_len = blen;

        if (header_has(out->headers, "Transfer-Encoding", "chunked")) {
            size_t dl = dechunk(out->body, out->body_len);
            if (dl == (size_t)-1) { rv = CE_HTTPS_ERR_PROTOCOL; goto done; }
            out->body_len = dl;
        }
    }

    rv = CE_HTTPS_OK;

done:
    if (ssl != NULL) {
        wolfSSL_free(ssl);
    }
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
    free(req.p);
    free(resp.p);
    if (rv != CE_HTTPS_OK) {
        ce_https_response_free(out);
    }
    return rv;
}

/* Split "https://host/path" into host + path (path includes leading '/'). */
static int parse_https_url(const char *url, char *host, size_t hostsz,
                           char *path, size_t pathsz)
{
    const char *h, *slash;
    size_t hl;
    if (_strnicmp(url, "https://", 8) != 0) {
        return 0;
    }
    h = url + 8;
    slash = strchr(h, '/');
    if (slash == NULL) {
        hl = strlen(h);
        if (hl >= hostsz) return 0;
        memcpy(host, h, hl); host[hl] = '\0';
        if (pathsz < 2) return 0;
        path[0] = '/'; path[1] = '\0';
        return 1;
    }
    hl = (size_t)(slash - h);
    if (hl >= hostsz || strlen(slash) >= pathsz) return 0;
    memcpy(host, h, hl); host[hl] = '\0';
    strcpy(path, slash);
    return 1;
}

/* Find a header value (single line) into out; returns 1 if present. */
static int header_value(const char *headers, const char *name,
                        char *out, size_t outsz)
{
    const char *line = headers;
    size_t nlen = strlen(name);
    while (line && *line) {
        if (_strnicmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            const char *eol;
            size_t vl;
            while (*v == ' ') v++;
            eol = strstr(v, "\r\n");
            vl = eol ? (size_t)(eol - v) : strlen(v);
            if (vl >= outsz) vl = outsz - 1;
            memcpy(out, v, vl); out[vl] = '\0';
            return 1;
        }
        line = strstr(line, "\r\n");
        if (line) line += 2;
    }
    return 0;
}

enum ce_https_result ce_https_download_url(const char *url,
                                           const char *extra_hdrs,
                                           const wchar_t *out_path,
                                           unsigned long max_bytes,
                                           int *out_status,
                                           unsigned long *out_bytes)
{
    enum ce_https_result rv = CE_HTTPS_ERR_PROTOCOL;
    char cur_url[2048];
    int redirects;

    if (out_status) *out_status = 0;
    if (out_bytes)  *out_bytes  = 0;

    if (ensure_globals() != CE_HTTPS_OK) return CE_HTTPS_ERR_INIT;
    if (strlen(url) >= sizeof(cur_url)) return CE_HTTPS_ERR_PROTOCOL;
    strcpy(cur_url, url);

    for (redirects = 0; redirects < 4; redirects++) {
        char host[256];
        char *path;
        char *req;
        SOCKET sock = INVALID_SOCKET;
        WOLFSSL *ssl = NULL;
        char hdrbuf[8192];
        size_t hdrlen = 0;
        char *sep = NULL;
        int status = 0;
        HANDLE fh = INVALID_HANDLE_VALUE;
        char te[64], loc[2048];
        int is_chunked, is_redirect;

        path = (char *)malloc(2048);
        req  = (char *)malloc(3072);
        if (!path || !req) { free(path); free(req); return CE_HTTPS_ERR_NOMEM; }

        if (!parse_https_url(cur_url, host, sizeof(host), path, 2048)) {
            free(path); free(req); return CE_HTTPS_ERR_PROTOCOL;
        }

        sock = tcp_connect(host, &rv);
        if (sock == INVALID_SOCKET) { free(path); free(req); return rv; }

        ssl = wolfSSL_new(g_ctx);
        if (!ssl) { closesocket(sock); free(path); free(req); return CE_HTTPS_ERR_INIT; }
        wolfSSL_set_fd(ssl, (int)sock);
        wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, (unsigned short)strlen(host));
        wolfSSL_check_domain_name(ssl, host);
        if (wolfSSL_connect(ssl) != WOLFSSL_SUCCESS) {
            wolfSSL_free(ssl); closesocket(sock); free(path); free(req);
            return CE_HTTPS_ERR_TLS;
        }

        _snprintf(req, 3072,
                  "GET %s HTTP/1.1\r\nHost: %s\r\n"
                  "Accept-Encoding: identity\r\nConnection: close\r\n%s%s\r\n",
                  path, host,
                  (extra_hdrs && extra_hdrs[0]) ? extra_hdrs : "",
                  (extra_hdrs && extra_hdrs[0]) ? "\r\n" : "");
        if (!tls_write_all(ssl, req, strlen(req))) {
            wolfSSL_free(ssl); closesocket(sock); free(path); free(req);
            return CE_HTTPS_ERR_SEND;
        }

        /* Read until the end of the header block. */
        while (hdrlen < sizeof(hdrbuf) - 1) {
            int r = wolfSSL_read(ssl, hdrbuf + hdrlen, (int)(sizeof(hdrbuf) - 1 - hdrlen));
            if (r <= 0) break;
            hdrlen += (size_t)r;
            hdrbuf[hdrlen] = '\0';
            sep = strstr(hdrbuf, "\r\n\r\n");
            if (sep) break;
        }
        if (!sep) {
            wolfSSL_free(ssl); closesocket(sock); free(path); free(req);
            return CE_HTTPS_ERR_PROTOCOL;
        }
        *sep = '\0';                       /* terminate headers for parsing */

        if (strncmp(hdrbuf, "HTTP/1.", 7) == 0) {
            const char *spc = strchr(hdrbuf, ' ');
            if (spc) status = atoi(spc + 1);
        }
        is_chunked  = header_value(hdrbuf, "Transfer-Encoding", te, sizeof(te)) &&
                      (strstr(te, "chunked") != NULL || strstr(te, "Chunked") != NULL);
        is_redirect = (status == 301 || status == 302 || status == 303 ||
                       status == 307 || status == 308);

        if (is_redirect && header_value(hdrbuf, "Location", loc, sizeof(loc))) {
            wolfSSL_free(ssl); closesocket(sock); free(path); free(req);
            if (strlen(loc) >= sizeof(cur_url)) return CE_HTTPS_ERR_PROTOCOL;
            strcpy(cur_url, loc);
            continue;                      /* follow redirect */
        }

        if (out_status) *out_status = status;

        if (is_chunked) {                  /* streaming de-chunk not implemented */
            wolfSSL_free(ssl); closesocket(sock); free(path); free(req);
            return CE_HTTPS_ERR_PROTOCOL;
        }

        fh = CreateFileW(out_path, GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) {
            wolfSSL_free(ssl); closesocket(sock); free(path); free(req);
            return CE_HTTPS_ERR_RECV;
        }

        /* Write the body bytes already read past the header boundary. */
        {
            char *body0 = sep + 4;
            size_t body0_len = hdrlen - (size_t)(body0 - hdrbuf);
            DWORD w;
            unsigned long total = 0;
            if (body0_len > 0) {
                if (max_bytes && body0_len > max_bytes) body0_len = max_bytes;
                WriteFile(fh, body0, (DWORD)body0_len, &w, NULL);
                total += (unsigned long)body0_len;
            }
            while (!max_bytes || total < max_bytes) {
                char tmp[8192];
                int want = (int)sizeof(tmp);
                int r;
                if (max_bytes && (unsigned long)(total + want) > max_bytes) {
                    want = (int)(max_bytes - total);
                }
                r = wolfSSL_read(ssl, tmp, want);
                if (r > 0) {
                    WriteFile(fh, tmp, (DWORD)r, &w, NULL);
                    total += (unsigned long)r;
                    continue;
                }
                break;                     /* EOF or error: server closed */
            }
            if (out_bytes) *out_bytes = total;
        }

        CloseHandle(fh);
        wolfSSL_free(ssl);
        closesocket(sock);
        free(path); free(req);
        return CE_HTTPS_OK;
    }
    return CE_HTTPS_ERR_PROTOCOL;          /* too many redirects */
}

/* ── persistent keep-alive connection ──────────────────────────────────── */

struct ce_https_conn {
    SOCKET   sock;
    WOLFSSL *ssl;
    char     host[256];
    char     path[2048];
    char     extra[1024];
};

static enum ce_https_result conn_dial(struct ce_https_conn *c)
{
    enum ce_https_result rv = CE_HTTPS_OK;
    c->sock = tcp_connect(c->host, &rv);
    if (c->sock == INVALID_SOCKET) {
        return rv;
    }
    c->ssl = wolfSSL_new(g_ctx);
    if (c->ssl == NULL) {
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
        return CE_HTTPS_ERR_INIT;
    }
    wolfSSL_set_fd(c->ssl, (int)c->sock);
    wolfSSL_UseSNI(c->ssl, WOLFSSL_SNI_HOST_NAME, c->host, (unsigned short)strlen(c->host));
    wolfSSL_check_domain_name(c->ssl, c->host);
    if (wolfSSL_connect(c->ssl) != WOLFSSL_SUCCESS) {
        g_last_tls_err = wolfSSL_get_error(c->ssl, -1);
        wolfSSL_free(c->ssl);
        c->ssl = NULL;
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
        return CE_HTTPS_ERR_TLS;
    }
    return CE_HTTPS_OK;
}

static void conn_drop(struct ce_https_conn *c)
{
    if (c->ssl != NULL) {
        wolfSSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->sock != INVALID_SOCKET) {
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
    }
}

/* One ranged GET on the live socket: write request, read the header block, then
 * read exactly Content-Length body bytes into buf (leaving the socket clean for
 * the next keep-alive request). No reconnect; the caller retries. */
static enum ce_https_result conn_request_once(struct ce_https_conn *c,
        unsigned long off, unsigned long len,
        void *buf, size_t bufsz, size_t *out_len,
        unsigned long *out_total, int *out_status)
{
    char   req[3328];
    char   hbuf[8192];
    size_t hlen = 0;
    char  *sep = NULL;
    int    status = 0;
    unsigned long total = 0, clen = 0;
    char   cr[160], cl[64];
    size_t body_pre, copied = 0;
    char  *out = (char *)buf;

    _snprintf(req, sizeof(req),
              "GET %s HTTP/1.1\r\nHost: %s\r\n"
              "Accept-Encoding: identity\r\nConnection: keep-alive\r\n"
              "Range: bytes=%lu-%lu\r\n%s%s\r\n",
              c->path, c->host, off, off + len - 1,
              (c->extra[0] ? c->extra : ""),
              (c->extra[0] ? "\r\n" : ""));
    if (!tls_write_all(c->ssl, req, strlen(req))) {
        return CE_HTTPS_ERR_SEND;
    }

    for (;;) {
        int r;
        if (hlen >= sizeof(hbuf) - 1) {
            return CE_HTTPS_ERR_PROTOCOL;
        }
        r = wolfSSL_read(c->ssl, hbuf + hlen, (int)(sizeof(hbuf) - 1 - hlen));
        if (r <= 0) {
            return CE_HTTPS_ERR_RECV;
        }
        hlen += (size_t)r;
        hbuf[hlen] = '\0';
        sep = strstr(hbuf, "\r\n\r\n");
        if (sep != NULL) {
            break;
        }
    }
    *sep = '\0';

    if (strncmp(hbuf, "HTTP/1.", 7) == 0) {
        const char *spc = strchr(hbuf, ' ');
        if (spc != NULL) {
            status = atoi(spc + 1);
        }
    }
    if (out_status) *out_status = status;

    if (header_value(hbuf, "Content-Length", cl, sizeof(cl))) {
        clen = strtoul(cl, NULL, 10);
    }
    if (header_value(hbuf, "Content-Range", cr, sizeof(cr))) {
        char *slash = strrchr(cr, '/');
        if (slash != NULL) {
            total = strtoul(slash + 1, NULL, 10);
        }
    }
    if (out_total) *out_total = total;

    /* body bytes already pulled past the header separator during the header read */
    body_pre = hlen - (size_t)((sep + 4) - hbuf);
    if (clen == 0) {
        clen = (unsigned long)body_pre;     /* no Content-Length: trust what we read */
    }
    if ((size_t)clen > bufsz) {
        return CE_HTTPS_ERR_NOMEM;           /* caller must size buf >= length */
    }
    if (body_pre > 0) {
        size_t take = body_pre > (size_t)clen ? (size_t)clen : body_pre;
        memcpy(out, sep + 4, take);
        copied = take;
    }
    while (copied < (size_t)clen) {
        int r = wolfSSL_read(c->ssl, out + copied, (int)((size_t)clen - copied));
        if (r <= 0) {
            return CE_HTTPS_ERR_RECV;
        }
        copied += (size_t)r;
    }
    if (out_len) *out_len = copied;
    return CE_HTTPS_OK;
}

ce_https_conn *ce_https_conn_open(const char *url, const char *extra_hdrs,
                                  enum ce_https_result *err)
{
    struct ce_https_conn *c;
    enum ce_https_result e;

    if (err) *err = CE_HTTPS_ERR_INIT;
    if (ensure_globals() != CE_HTTPS_OK) {
        return NULL;
    }
    c = (struct ce_https_conn *)calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->sock = INVALID_SOCKET;
    if (!parse_https_url(url, c->host, sizeof(c->host), c->path, sizeof(c->path))) {
        free(c);
        if (err) *err = CE_HTTPS_ERR_PROTOCOL;
        return NULL;
    }
    if (extra_hdrs != NULL && extra_hdrs[0] != '\0' && strlen(extra_hdrs) < sizeof(c->extra)) {
        strcpy(c->extra, extra_hdrs);
    }
    e = conn_dial(c);
    if (e != CE_HTTPS_OK) {
        free(c);
        if (err) *err = e;
        return NULL;
    }
    if (err) *err = CE_HTTPS_OK;
    return c;
}

enum ce_https_result ce_https_conn_get(ce_https_conn *c,
                                       unsigned long offset,
                                       unsigned long length,
                                       void *buf, size_t bufsz,
                                       size_t *out_len,
                                       unsigned long *out_total,
                                       int *out_status)
{
    enum ce_https_result rv;

    if (c == NULL || length == 0 || buf == NULL) {
        return CE_HTTPS_ERR_PROTOCOL;
    }
    rv = conn_request_once(c, offset, length, buf, bufsz, out_len, out_total, out_status);
    if (rv == CE_HTTPS_ERR_SEND || rv == CE_HTTPS_ERR_RECV) {
        /* peer closed the keep-alive socket between requests: redial once */
        conn_drop(c);
        rv = conn_dial(c);
        if (rv != CE_HTTPS_OK) {
            return rv;
        }
        rv = conn_request_once(c, offset, length, buf, bufsz, out_len, out_total, out_status);
    }
    return rv;
}

void ce_https_conn_close(ce_https_conn *c)
{
    if (c == NULL) {
        return;
    }
    conn_drop(c);
    free(c);
}

void ce_https_response_free(struct ce_https_response *resp)
{
    if (resp == NULL) {
        return;
    }
    free(resp->headers);
    free(resp->body);
    resp->headers = NULL;
    resp->body = NULL;
    resp->headers_len = 0;
    resp->body_len = 0;
}

const char *ce_https_result_str(enum ce_https_result r)
{
    switch (r) {
    case CE_HTTPS_OK:           return "OK";
    case CE_HTTPS_ERR_INIT:     return "INIT";
    case CE_HTTPS_ERR_RESOLVE:  return "RESOLVE";
    case CE_HTTPS_ERR_CONNECT:  return "CONNECT";
    case CE_HTTPS_ERR_TLS:      return "TLS";
    case CE_HTTPS_ERR_SEND:     return "SEND";
    case CE_HTTPS_ERR_RECV:     return "RECV";
    case CE_HTTPS_ERR_PROTOCOL: return "PROTOCOL";
    case CE_HTTPS_ERR_NOMEM:    return "NOMEM";
    default:                    return "?";
    }
}
