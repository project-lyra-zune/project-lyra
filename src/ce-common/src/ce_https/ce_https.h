/* ce_https.h: standalone synchronous HTTPS client for the Zune HD.
 *
 * A blocking request/response client for the YouTube (Innertube) client and
 * any other non-NetSurf consumer that just wants "send a request, get the
 * body back". This is deliberately NOT NetSurf's async fetcher (ce_http);
 * that one is driven by NetSurf's poll loop and dispatches FETCH_* events;
 * this one blocks the calling thread and returns a buffer.
 *
 * TLS comes from the shared verifying client context (ce_tls_ctx). One CTX is
 * built lazily and reused across requests for process lifetime.
 */
#ifndef ZB_CE_HTTPS_H
#define ZB_CE_HTTPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ce_https_result {
    CE_HTTPS_OK = 0,
    CE_HTTPS_ERR_INIT,        /* WSAStartup / TLS context build failed */
    CE_HTTPS_ERR_RESOLVE,     /* getaddrinfo failed */
    CE_HTTPS_ERR_CONNECT,     /* TCP connect failed */
    CE_HTTPS_ERR_TLS,         /* TLS handshake / cert verification failed */
    CE_HTTPS_ERR_SEND,        /* request write failed */
    CE_HTTPS_ERR_RECV,        /* response read failed */
    CE_HTTPS_ERR_PROTOCOL,    /* malformed status line */
    CE_HTTPS_ERR_NOMEM
};

/* A completed HTTP response. `body`/`headers` are heap buffers owned by the
 * struct; release with ce_https_response_free(). `body` is NUL-terminated for
 * convenience (JSON consumers), `body_len` is the byte length excluding the
 * terminator. `headers` is the raw header block (status line + header lines,
 * CRLF-separated), NUL-terminated. */
struct ce_https_response {
    int    status;            /* HTTP status code, e.g. 200 */
    char  *headers;
    size_t headers_len;
    char  *body;
    size_t body_len;
};

/* Perform one HTTPS request.
 *
 *   host         hostname, e.g. "youtubei.googleapis.com"
 *   path         request target, e.g. "/youtubei/v1/search?key=..."
 *   method       "GET" or "POST"
 *   extra_hdrs   NULL, or extra header lines joined by CRLF with NO trailing
 *                CRLF, e.g. "X-Goog-Api-Format-Version: 2\r\nOrigin: https://..."
 *   body         request body (POST), or NULL
 *   body_len     length of body, or 0
 *   content_type Content-Type for the body, or NULL (defaults to JSON when
 *                body != NULL)
 *   out          filled on CE_HTTPS_OK; caller frees with
 *                ce_https_response_free()
 *
 * Requests Accept-Encoding: identity (no gzip) so the response body is plain.
 * Follows no redirects; the caller inspects out->status. */
enum ce_https_result ce_https_request(const char *host,
                                      const char *path,
                                      const char *method,
                                      const char *extra_hdrs,
                                      const void *body,
                                      size_t body_len,
                                      const char *content_type,
                                      struct ce_https_response *out);

void ce_https_response_free(struct ce_https_response *resp);

/* Phase timing (ms) of the most recent ce_https_request: TCP connect (incl. DNS),
 * TLS handshake (small when the session resumed), and response read. For latency
 * diagnosis; any pointer may be NULL. */
void ce_https_last_timing(int *connect_ms, int *tls_ms, int *recv_ms);

/* Stream a GET response body straight to a file (no full-RAM buffering, for
 * multi-MB media). Parses an absolute https:// URL, follows up to a few
 * redirects, and writes the body to out_path via the Win32 file API.
 *
 *   url         absolute "https://host/path?query"
 *   extra_hdrs  NULL or extra header lines (CRLF-joined, no trailing CRLF)
 *   out_path    wide filesystem path to create/overwrite
 *   max_bytes   stop after this many body bytes (0 = unlimited). itag-18 MP4 is
 *               faststart (moov at front), so a capped prefix still plays,
 *               useful to bound a synchronous fetch and measure throughput.
 *   out_status  filled with the final HTTP status
 *   out_bytes   filled with the number of body bytes written
 *
 * Returns CE_HTTPS_ERR_PROTOCOL if the response is chunked (not expected from
 * googlevideo; the streaming path handles Content-Length / connection-close). */
enum ce_https_result ce_https_download_url(const char *url,
                                           const char *extra_hdrs,
                                           const wchar_t *out_path,
                                           unsigned long max_bytes,
                                           int *out_status,
                                           unsigned long *out_bytes);

/* ── persistent keep-alive connection for ranged reads ─────────────────────
 *
 * A single TLS connection reused across many HTTP Range requests. Built for the
 * progressive YouTube Music pipeline: Phase 1 box-walks the fragmented MP4 by
 * fetching just the small moof boxes (skipping the big mdat payloads), then
 * Phase 2 streams the mdat payloads in windows, both over one socket so the
 * 20-odd small Phase-1 requests don't each pay a TLS handshake.
 *
 * The connection dials the host of the supplied URL directly (ANDROID_VR
 * googlevideo URLs are already final, no redirect). If the peer drops the
 * keep-alive socket, ce_https_conn_get reconnects once and retries. */
typedef struct ce_https_conn ce_https_conn;

/* Open a keep-alive connection to the host of `url`. `extra_hdrs` (e.g. the
 * client User-Agent) is sent on every request. Returns NULL on failure with the
 * reason in *err (if non-NULL). */
ce_https_conn *ce_https_conn_open(const char *url,
                                  const char *extra_hdrs,
                                  enum ce_https_result *err);

/* Read [offset, offset+length) of the resource into buf (length must be > 0 and
 * bufsz >= length). On a 206 response *out_total receives the full resource size
 * parsed from Content-Range; *out_status receives the HTTP status; *out_len the
 * number of body bytes written. Any of the out pointers may be NULL. */
enum ce_https_result ce_https_conn_get(ce_https_conn *c,
                                       unsigned long offset,
                                       unsigned long length,
                                       void *buf, size_t bufsz,
                                       size_t *out_len,
                                       unsigned long *out_total,
                                       int *out_status);

void ce_https_conn_close(ce_https_conn *c);

/* Human-readable name for a result code (for logging). */
const char *ce_https_result_str(enum ce_https_result r);

/* wolfSSL_get_error code of the most recent failed TLS handshake (for diagnosing
 * CE_HTTPS_ERR_TLS). 0 if none. */
int ce_https_last_tls_error(void);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CE_HTTPS_H */
