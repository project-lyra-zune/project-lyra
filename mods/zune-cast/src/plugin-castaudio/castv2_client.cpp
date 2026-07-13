#include "castaudio.h"
#include "cast_keys.h"   // CAST_STATUS_KEY + status states
#include "cast_channel.h" // cast_channel_set_sublabel / name lookup (row sub-label)
#include "zdk.h"
#include "zme.h"
#include "ce_image.h"
#include <wolfssl/ssl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// CASTV2 namespaces (Google Cast control protocol).
#define NS_CONNECTION  "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT   "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECEIVER    "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA       "urn:x-cast:com.google.cast.media"
#define SENDER_ID      "sender-0"
#define RECEIVER_ID    "receiver-0"

// ── protobuf (proto2) helpers for the CastMessage wire format ──────────────
// CastMessage fields: 1 protocol_version (varint), 2 source_id (string),
// 3 destination_id (string), 4 namespace (string), 5 payload_type (varint),
// 6 payload_utf8 (string). Framed on the socket with a 4-byte big-endian
// length prefix.

static int put_varint(unsigned char* p, unsigned int v)
{
    int n = 0;
    do {
        unsigned char b = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v) b |= 0x80;
        p[n++] = b;
    } while (v);
    return n;
}

static int get_varint(const unsigned char* p, int len, int* pos, unsigned int* out)
{
    unsigned int v = 0;
    int shift = 0;
    while (*pos < len) {
        unsigned char b = p[(*pos)++];
        v |= (unsigned int)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = v; return 1; }
        shift += 7;
        if (shift > 35) return 0;
    }
    return 0;
}

static int enc_string_field(unsigned char* out, int field, const char* s)
{
    int slen = (int)strlen(s);
    int n = 0;
    out[n++] = (unsigned char)((field << 3) | 2);   // wire type 2 = length-delimited
    n += put_varint(out + n, (unsigned int)slen);
    memcpy(out + n, s, slen);
    n += slen;
    return n;
}

static int enc_cast_message(unsigned char* out, const char* src, const char* dst,
                            const char* ns, const char* payload)
{
    int n = 0;
    out[n++] = 0x08; out[n++] = 0x00;               // field 1 protocol_version = 0
    n += enc_string_field(out + n, 2, src);
    n += enc_string_field(out + n, 3, dst);
    n += enc_string_field(out + n, 4, ns);
    out[n++] = 0x28; out[n++] = 0x00;               // field 5 payload_type = STRING
    n += enc_string_field(out + n, 6, payload);
    return n;
}

// Extracts payload_utf8 (field 6) into a NUL-terminated buffer.
static int dec_payload(const unsigned char* p, int len, char* out, int outsz)
{
    int pos = 0;
    out[0] = '\0';
    while (pos < len) {
        unsigned int tag;
        if (!get_varint(p, len, &pos, &tag)) return 0;
        int field = (int)(tag >> 3);
        int wt    = (int)(tag & 7);
        if (wt == 0) {
            unsigned int dummy;
            if (!get_varint(p, len, &pos, &dummy)) return 0;
        } else if (wt == 2) {
            unsigned int l;
            if (!get_varint(p, len, &pos, &l)) return 0;
            if (pos + (int)l > len) return 0;
            if (field == 6) {
                int cp = (int)l < outsz - 1 ? (int)l : outsz - 1;
                memcpy(out, p + pos, cp);
                out[cp] = '\0';
            }
            pos += (int)l;
        } else {
            return 0;   // unsupported wire type
        }
    }
    return 1;
}

// Pulls "key":"value" out of a flat JSON payload. Enough to scrape the
// application transportId from RECEIVER_STATUS.
static int json_get_string(const char* json, const char* key, char* out, int outsz)
{
    char needle[64];
    _snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* p = strstr(json, needle);
    if (p == NULL) return 0;
    p += strlen(needle);
    int n = 0;
    while (*p && *p != '"' && n < outsz - 1) out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
}

// Pulls "key":<number> out of a flat JSON payload (mediaSessionId, which the
// receiver assigns and PAUSE/PLAY/SEEK commands must echo back).
static int json_get_int(const char* json, const char* key, int* out)
{
    char needle[64];
    _snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (p == NULL) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    int neg = (*p == '-');
    if (neg) p++;
    if (*p < '0' || *p > '9') return 0;
    int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *out = neg ? -v : v;
    return 1;
}

// Parse the receiver volume level (`"volume":{"level":0.5,...}`) as x1000 milli,
// scoped to the volume object so a stray "level" elsewhere can't match. Returns
// -1 if absent/unparseable.
static int json_get_volume_milli(const char* json)
{
    const char* vp = strstr(json, "\"volume\":");
    if (vp == NULL) return -1;
    const char* p = strstr(vp, "\"level\":");
    if (p == NULL) return -1;
    p += 8;                                  // strlen("\"level\":")
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1;     // level is 0.0..1.0, never negative
    int whole = 0;
    while (*p >= '0' && *p <= '9') whole = whole * 10 + (*p++ - '0');
    int milli = whole * 1000;                // 1.0 -> 1000
    if (*p == '.') {
        p++;
        int scale = 100;                     // first fractional digit = hundreds of milli
        while (scale > 0 && *p >= '0' && *p <= '9') { milli += (*p++ - '0') * scale; scale /= 10; }
    }
    if (milli > 1000) milli = 1000;
    return milli;
}

// ── socket / TLS plumbing ──────────────────────────────────────────────────

static int connect_with_timeout(SOCKET s, sockaddr_in* a, int ms)
{
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, (sockaddr*)a, sizeof(*a)) == 0) return 0;
    if (WSAGetLastError() != WSAEWOULDBLOCK) return -1;

    fd_set wf, ef;
    FD_ZERO(&wf); FD_SET(s, &wf);
    FD_ZERO(&ef); FD_SET(s, &ef);
    timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (select(0, NULL, &wf, &ef, &tv) <= 0) return -1;
    if (FD_ISSET(s, &ef)) return -1;

    int err = 0, el = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
    return err == 0 ? 0 : -1;
}

static void local_ip(SOCKET s, char* out, int outsz)
{
    sockaddr_in la;
    int ll = sizeof(la);
    if (getsockname(s, (sockaddr*)&la, &ll) == 0) {
        unsigned char* p = (unsigned char*)&la.sin_addr;
        _snprintf(out, outsz, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    } else {
        _snprintf(out, outsz, "0.0.0.0");
    }
}

static int tls_handshake(WOLFSSL* ssl, SOCKET s, HANDLE stop_event)
{
    DWORD start = GetTickCount();
    for (;;) {
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) return -1;
        if (GetTickCount() - start > 10000) return -1;
        int rc = wolfSSL_connect(ssl);
        if (rc == WOLFSSL_SUCCESS) return 0;
        int e = wolfSSL_get_error(ssl, rc);
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            cast_log("TLS handshake err wolfSSL=%d", e);
            return -1;
        }
        fd_set fs;
        FD_ZERO(&fs); FD_SET(s, &fs);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
        if (e == WOLFSSL_ERROR_WANT_WRITE) select(0, NULL, &fs, NULL, &tv);
        else                                select(0, &fs, NULL, NULL, &tv);
    }
}

static int ssl_write_all(WOLFSSL* ssl, const unsigned char* p, int len)
{
    int sent = 0;
    while (sent < len) {
        int rc = wolfSSL_write(ssl, p + sent, len - sent);
        if (rc > 0) { sent += rc; continue; }
        int e = wolfSSL_get_error(ssl, rc);
        if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE) {
            Sleep(5);
            continue;
        }
        return -1;
    }
    return 0;
}

static int send_message(WOLFSSL* ssl, const char* src, const char* dst,
                        const char* ns, const char* payload)
{
    // Heap-sized to the payload: a QUEUE_LOAD carrying a base64 album-art data:
    // URI runs to tens of KB, past any fixed stack buffer.
    int cap = (int)strlen(payload) + (int)strlen(src) + (int)strlen(dst) +
              (int)strlen(ns) + 64;
    unsigned char* body  = (unsigned char*)malloc(cap);
    unsigned char* frame = (unsigned char*)malloc(cap + 4);
    if (body == NULL || frame == NULL) { free(body); free(frame); return -1; }
    int bl = enc_cast_message(body, src, dst, ns, payload);
    frame[0] = (unsigned char)((unsigned)bl >> 24);
    frame[1] = (unsigned char)((unsigned)bl >> 16);
    frame[2] = (unsigned char)((unsigned)bl >> 8);
    frame[3] = (unsigned char)((unsigned)bl);
    memcpy(frame + 4, body, bl);
    int rc = ssl_write_all(ssl, frame, bl + 4);
    free(body);
    free(frame);
    return rc;
}

// Escape " and \ for embedding a string in JSON (wide->ascii already removed
// control chars, mapping them to '?').
static void json_escape(char* dst, int dstsz, const char* src)
{
    int j = 0;
    for (int i = 0; src[i] && j < dstsz - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') dst[j++] = '\\';
        dst[j++] = c;
    }
    dst[j] = 0;
}

// Album art is delivered as a base64 data: URI on the now-playing item (the
// only path that works for a distributed mod: http image URLs get upgraded to
// https, a self-signed on-device cert is rejected, and a real cert can't be
// shipped). The full art (34-446 KB) exceeds the receiver's data-URI limit, so
// it is first downscaled + re-encoded to a small JPEG on-device via the native
// imaging.dll (ce_image_thumbnail_jpeg).
#define CAST_ART_THUMB_DIM 384

// Reads the whole file at `path` into a malloc'd buffer. Returns the buffer
// (caller frees) and sets *out_len, or NULL on failure / oversize.
static unsigned char* read_whole_file(const wchar_t* path, unsigned int* out_len)
{
    *out_len = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD hi = 0, sz = GetFileSize(f, &hi);
    if (hi != 0 || sz == 0 || sz > 2000000) { CloseHandle(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc(sz);
    if (buf == NULL) { CloseHandle(f); return NULL; }
    DWORD got = 0;
    BOOL ok = ReadFile(f, buf, sz, &got, NULL);
    CloseHandle(f);
    if (!ok || got != sz) { free(buf); return NULL; }
    *out_len = sz;
    return buf;
}

// Base64-encodes in[0..n) into out (NUL-terminated). Returns length, 0 if it
// would not fit in out_max.
static int b64_encode(const unsigned char* in, int n, char* out, int out_max)
{
    if ((n + 2) / 3 * 4 + 1 > out_max) return 0;
    static const char* B = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int rem = n - i;
        unsigned int v = (unsigned int)in[i] << 16;
        if (rem > 1) v |= (unsigned int)in[i + 1] << 8;
        if (rem > 2) v |= (unsigned int)in[i + 2];
        out[o++] = B[(v >> 18) & 63];
        out[o++] = B[(v >> 12) & 63];
        out[o++] = (rem > 1) ? B[(v >> 6) & 63] : '=';
        out[o++] = (rem > 2) ? B[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int g_load_req = 100;   // QUEUE_LOAD requestId
static int g_req_id   = 1;     // PAUSE / PLAY requestId (reconcile thread only)

// ── QUEUE_LOAD payload builder (reconcile thread; owns ZDKMedia + imaging) ───
// Builds the QUEUE_LOAD JSON into a malloc'd buffer the caller/queue owns (freed
// after the Cast I/O thread sends it). Records the window base/count/durations
// and anchors the continuous cursor into the shared CastQueue. The expensive
// work (ZDKMedia enumeration, album-art decode/scale/encode) runs here, off the
// I/O thread's heartbeat path. Returns NULL on alloc failure.
static char* build_queue_load(const char* myip, unsigned short port,
                              CastQueue* q, CaptureRing* ring)
{
    int count  = zdk_queue_count();
    int active = zdk_active_index();
    if (count <= 0 || active < 0) { active = 0; count = 1; }   // degrade gracefully

    int base = active - 3; if (base < 0) base = 0;
    int last = base + CAST_MAX_Q - 1; if (last > count - 1) last = count - 1;
    int nitems = last - base + 1;
    q->base = base;
    q->count = nitems;
    LONG gen = InterlockedIncrement(&q->generation);   // new generation; a GET for a stale gen (leftover session) is 404'd

    const int IMG_SZ = 24576, ITEMS_SZ = 32768, MSG_SZ = 36864;
    char* items   = (char*)malloc(ITEMS_SZ);
    char* msg     = (char*)malloc(MSG_SZ);
    char* imgfrag = (char*)malloc(IMG_SZ);
    if (items == NULL || msg == NULL || imgfrag == NULL) {
        free(items); free(msg); free(imgfrag); cast_log("QUEUE_LOAD alloc fail"); return NULL;
    }
    int n = 0;
    int art_n = 0;
    LONG anchor = ring ? ring->produced : -1;   // fallback if there's no active item
    for (int i = base; i <= last; i++) {
        char title[48] = { 0 }, artist[48] = { 0 }, album[48] = { 0 };
        int dur_ms = 0;
        zdk_queue_track(i, title, sizeof(title), artist, sizeof(artist),
                        album, sizeof(album), &dur_ms);
        if (i == active) {                          // current: prefer live ZME0 duration
            unsigned int hr = 0, dms = 0;
            if (zme_read(0x2f, &hr, &dms) && hr == 0 && dms > 0) dur_ms = (int)dms;
            // Joining the active track mid-play: size it to the REMAINING audio so
            // the continuous cursor lands on the next track's head at the boundary.
            // Capture the cursor anchor at the SAME instant as the position read,
            // BEFORE the album-art encode below, so the encode's latency doesn't
            // push the anchor past where `remaining` assumes it is (that gap was a
            // ~0.5 s join overshoot).
            int dur_full = dur_ms;
            anchor = ring ? ring->produced : -1;
            int pos = zdk_play_position_ms();
            unsigned int zhr = 0, zpos = 0;
            int zok = (zme_read(0x2b, &zhr, &zpos) && zhr == 0);
            if (pos > 0 && pos < dur_ms) dur_ms -= pos;
            cast_log("JOIN active=%d zdk_pos=%d zme0_pos=%d(hr=%u ok=%d) dur_full=%d remaining=%d anchor=%ld",
                     i, pos, (int)zpos, zhr, zok, dur_full, dur_ms, (long)anchor);
        }
        q->dur_ms[i - base] = dur_ms;
        char et[100], ea[100], eal[100];
        json_escape(et, sizeof(et), title);
        json_escape(ea, sizeof(ea), artist);
        json_escape(eal, sizeof(eal), album);
        int got_art = zdk_album_art_path(i, q->art[i - base], 128);
        if (got_art) art_n++;
        imgfrag[0] = 0;
        if (i == active && got_art) {
            unsigned int flen = 0;
            unsigned char* fbuf = read_whole_file(q->art[i - base], &flen);
            if (fbuf) {
                unsigned char* jpg = NULL; unsigned int jlen = 0;
                ce_image_status cs = ce_image_thumbnail_jpeg(fbuf, flen, CAST_ART_THUMB_DIM, &jpg, &jlen);
                if (cs == CE_IMAGE_OK && jpg) {
                    int pfx = _snprintf(imgfrag, IMG_SZ, ",\"images\":[{\"url\":\"data:image/jpeg;base64,");
                    int bl  = b64_encode(jpg, (int)jlen, imgfrag + pfx, IMG_SZ - pfx - 8);
                    if (bl > 0) _snprintf(imgfrag + pfx + bl, IMG_SZ - pfx - bl, "\"}]");
                    else        imgfrag[0] = 0;
                    cast_log("ART thumb active=%d jpg=%u b64=%d", i, jlen, bl);
                    ce_image_free(jpg);
                } else {
                    cast_log("ART thumb fail active=%d ce=%d", i, (int)cs);
                }
                free(fbuf);
            }
        }
        n += _snprintf(items + n, ITEMS_SZ - n,
                       "%s{\"media\":{\"contentId\":\"http://%s:%u/g%ld/q%d.wav\","
                       "\"contentType\":\"audio/wav\",\"streamType\":\"BUFFERED\",\"duration\":%.3f,"
                       "\"metadata\":{\"metadataType\":3,\"title\":\"%s\",\"artist\":\"%s\","
                       "\"albumName\":\"%s\"%s}},"
                       "\"autoplay\":true}",
                       (i > base) ? "," : "", myip, port, (long)gen, i, (double)dur_ms / 1000.0, et, ea, eal, imgfrag);
    }

    _snprintf(msg, MSG_SZ,
              "{\"type\":\"QUEUE_LOAD\",\"requestId\":%d,\"startIndex\":%d,"
              "\"repeatMode\":\"REPEAT_OFF\",\"items\":[%s]}",
              g_load_req++, active - base, items);
    q->serving_index = active;
    // Anchor the continuous cursor to the live tap position *now* (build time),
    // the same instant the active item's remaining duration was measured, so the
    // join stream ends exactly at the next track's head (no GET-gap overshoot).
    q->consume_cursor = anchor;
    // Anchor is aligned by definition (we joined this track here on purpose). Stamp
    // the epoch so the self-correct doesn't mistake the mid-track join for drift on
    // the first serve.
    q->aligned_epoch  = ring ? ring->track_epoch : 0;
    cast_log("QUEUE_LOAD base=%d n=%d active=%d art=%d/%d", base, nitems, active, art_n, nitems);
    free(items);
    free(imgfrag);
    return msg;     // caller / outbound queue owns; freed after send
}

// ── Cast I/O <-> reconcile link ─────────────────────────────────────────────
// Two threads so the latency-critical Cast heartbeat is never starved by blocking
// ZDKMedia/ZME0 IOCTLs or the album-art encode: the I/O thread owns the
// (single-threaded) wolfSSL socket and answers PING/PONG promptly; the reconcile
// thread owns ZDKMedia/ZME0, builds outbound messages, and hands them over this
// queue. wolfSSL is touched only by the I/O thread. Shared scalars are
// single-writer volatiles; the queue has a lock.
typedef struct OutMsg {
    struct OutMsg* next;
    const char*    ns;        // namespace literal (not owned)
    char*          payload;   // malloc'd JSON (owned; freed after send)
} OutMsg;

typedef struct {
    HANDLE           stop;        // per-connection stop (I/O thread sets on exit)
    CRITICAL_SECTION lock;        // guards the outbound queue
    OutMsg*          out_head;
    OutMsg*          out_tail;
    volatile LONG    connected;   // I/O->reconcile: transportId known + app CONNECTed
    volatile LONG    msid;        // I/O->reconcile: receiver mediaSessionId (0 = none)
    volatile LONG    cast_ps;     // I/O->reconcile: receiver playerState (-1 unk / 0 play / 1 pause)
    volatile LONG    cast_idle;   // I/O->reconcile: receiver IDLE/FINISHED (its window played out)
    volatile LONG    cast_vol_milli; // I/O->reconcile: receiver volume level x1000 (-1 = unknown)
    const char*      myip;
    unsigned short   media_port;
    CastQueue*       q;
    CaptureRing*     ring;
    const char*      target_ip;    // receiver address, for the row sub-label name lookup
    unsigned short   target_port;
} CastLink;

static void link_enqueue(CastLink* L, const char* ns, char* payload /*owned*/)
{
    if (payload == NULL) return;
    OutMsg* m = (OutMsg*)malloc(sizeof(OutMsg));
    if (m == NULL) { free(payload); return; }
    m->next = NULL; m->ns = ns; m->payload = payload;
    EnterCriticalSection(&L->lock);
    if (L->out_tail) L->out_tail->next = m; else L->out_head = m;
    L->out_tail = m;
    LeaveCriticalSection(&L->lock);
}

static OutMsg* link_dequeue(CastLink* L)
{
    EnterCriticalSection(&L->lock);
    OutMsg* m = L->out_head;
    if (m) { L->out_head = m->next; if (L->out_head == NULL) L->out_tail = NULL; }
    LeaveCriticalSection(&L->lock);
    return m;
}

// Small JSON command payloads (PAUSE / PLAY) the reconcile thread builds and
// enqueues. Returns a malloc'd buffer or NULL.
static char* fmt_cmd(const char* fmt, int msid)
{
    char* p = (char*)malloc(160);
    if (p == NULL) return NULL;
    _snprintf(p, 160, fmt, msid, g_req_id++);
    return p;
}

// Build + enqueue a receiver SET_VOLUME for the device's current level. Same
// pipeline as PAUSE/PLAY; the drain routes NS_RECEIVER to receiver-0.
static void enqueue_set_volume(CastLink* L, unsigned long vol, unsigned long max)
{
    unsigned long milli = (max == 0) ? 0 : (vol >= max ? 1000 : (vol * 1000) / max);
    char* p = (char*)malloc(160);
    if (p == NULL) return;
    _snprintf(p, 160,
        "{\"type\":\"SET_VOLUME\",\"requestId\":%d,\"volume\":{\"level\":%lu.%03lu}}",
        g_req_id++, milli / 1000, milli % 1000);
    link_enqueue(L, NS_RECEIVER, p);
    cast_log("zune vol %lu/%lu -> cast SET_VOLUME %lu.%03lu", vol, max, milli / 1000, milli % 1000);
}

// ── reconcile thread (owns ZDKMedia / ZME0; may block) ──────────────────────
// Single decision point that keeps the receiver's queue mirroring the device's.
// Every device→receiver index change is a full QUEUE_LOAD reload: the receiver
// ignores a relative QUEUE_UPDATE jump while mid-streaming a BUFFERED item, so a
// reload is the only primitive that propagates an in-flight skip. The reload
// re-windows around the new active track (base = active-3), so forward skip,
// backward skip, out-of-window jump, and the sliding window are one path. A
// receiver-initiated jump the device isn't naturally heading toward drives the
// device via MoveTo; an idle receiver (window played out) while the device still
// plays is recovered by a reload.
static DWORD WINAPI reconcile_thread(LPVOID arg)
{
    CastLink*  L = (CastLink*)arg;
    CastQueue* q = L->q;

    // On-screen device volume: a shared buffer the servicesd ZAM-writer detour
    // publishes into. Pulled on the tick below and mirrored to the receiver like
    // play/pause, but to receiver-0. CE6 CreateFileMappingW by name opens the
    // producer's section regardless of which side starts first.
    HANDLE        vol_sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                               0, sizeof(VolumeStateBlock), VOLUME_STATE_SECTION);
    const VolumeStateBlock* vstate = vol_sec ? (const VolumeStateBlock*)
                                     MapViewOfFile(vol_sec, FILE_MAP_READ, 0, 0, 0) : NULL;
    unsigned long last_vol_seq = (unsigned long)-1;
    // Single reconciled volume in device steps (0..max). Both directions act only
    // on a real difference from it, so each side's confirming echo is a no-op (the
    // 31 device steps are the natural deadband). Same pattern as play/pause.
    int recon_vol = -1;
    LONG last_cast_milli = -1;       // last receiver level acted on (gates cast->device)
    DWORD suppress_cast_until = 0;   // ignore receiver echoes of our own SET_VOLUME until this tick

    int          loaded         = 0;
    int          play_paused    = -1;   // reconciled play/pause (0 playing, 1 paused)
    unsigned int rec_qid        = 0;    // live queue-object id (0x0e); changes on queue replacement
    int          prev_ci        = -1;   // device index at the previous poll (skip debounce)
    int          last_synced    = -1;   // device index the receiver currently mirrors
    int          last_pos_ms    = 0;    // synced track's latest position; frozen once the device leaves it
    int          last_dur_ms    = 0;    // synced track's duration
    int          idle_recovered = 0;    // one recovery reload per receiver-idle episode

    while (WaitForSingleObject(L->stop, 0) != WAIT_OBJECT_0) {
        if (!L->connected) { WaitForSingleObject(L->stop, 50); continue; }

        // Effective status from the live link: an active media session (msid>0)
        // is "casting" a queue; connected with no session is "connected" (idle).
        // mod_state_set_status dedups, so the per-iteration call is cheap.
        mod_state_set_status(CAST_STATUS_KEY,
                             L->msid > 0 ? CAST_STATUS_CASTING : CAST_STATUS_CONNECTED);
        // Row sub-label follows the same live link state (set dedups internally):
        // CASTING - {name} while a queue plays, CONNECTED TO {name} when idle.
        // Name is looked up each pass (not precomposed) so it self-heals from the
        // bare ip to the friendly name once discovery publishes the device.
        {
            wchar_t nm[MODLISTCH_NAME_LEN], sl[MODLISTCH_SUBLABEL_LEN];
            if (!cast_channel_name_for_target(L->target_ip, L->target_port, nm, MODLISTCH_NAME_LEN))
                _snwprintf(nm, MODLISTCH_NAME_LEN, L"%S", L->target_ip);
            nm[MODLISTCH_NAME_LEN - 1] = 0;
            _snwprintf(sl, MODLISTCH_SUBLABEL_LEN,
                       L->msid > 0 ? L"CASTING - %s" : L"CONNECTED TO %s", nm);
            sl[MODLISTCH_SUBLABEL_LEN - 1] = 0;
            cast_channel_set_sublabel(sl);
        }

        if (!loaded) {
            link_enqueue(L, NS_MEDIA, build_queue_load(L->myip, L->media_port, q, L->ring));
            loaded = 1;
            // Initial volume sync: one-shot device read so the receiver matches
            // on connect (device is authoritative here). Later changes arrive via
            // the push section (device side) and RECEIVER_STATUS (cast side).
            { unsigned int iv = 0, im = 0;
              if (zam_volume(&iv, &im) && im) { enqueue_set_volume(L, iv, im); recon_vol = (int)iv; } }
            if (vstate) last_vol_seq = vstate->seq;
            // Device is authoritative on connect: treat the receiver's pre-existing
            // level as already-seen so it doesn't override; only a later cast-side
            // change reacts (our initial SET_VOLUME settles the receiver to recon_vol).
            last_cast_milli = L->cast_vol_milli;
            { unsigned int hr0 = 0, q0 = 0;
              if (zme_read(0x0e, &hr0, &q0) && hr0 == 0) rec_qid = q0; }
            prev_ci = last_synced = zdk_active_index();
            q->device_index = last_synced;
            WaitForSingleObject(L->stop, 200);
            continue;
        }

        int ci   = zdk_active_index();
        int pos  = zdk_play_position_ms();
        int msid = (int)L->msid;
        int dev_playing = (zme_play_state() == ZME_PS_PLAYING);
        q->device_index = ci;

        unsigned int hr = 0, qid = 0;
        if (!zme_read(0x0e, &hr, &qid) || hr != 0) qid = 0;

        // Synced track's progress for the natural-advance test: kept current while
        // the device sits on the synced track, frozen the moment it moves off, so
        // the classification below sees the just-finished track's final position.
        if (ci == last_synced) {
            if (pos >= 0) last_pos_ms = pos;
            if (ci >= q->base && ci < q->base + q->count && q->dur_ms[ci - q->base] > 0)
                last_dur_ms = q->dur_ms[ci - q->base];
        }

        if (qid != 0 && qid != rec_qid) {
            // Queue replaced (new album/playlist): full reload around the new active.
            rec_qid = qid;
            link_enqueue(L, NS_MEDIA, build_queue_load(L->myip, L->media_port, q, L->ring));
            last_synced = ci;
            cast_log("zune queue replaced qid=%08x -> reload (active=%d)", qid, ci);
        } else if (L->cast_idle && dev_playing && !idle_recovered) {
            // Receiver played past the end of its loaded window while the device keeps
            // playing: reload to re-window and resume (window slides here).
            link_enqueue(L, NS_MEDIA, build_queue_load(L->myip, L->media_port, q, L->ring));
            last_synced = ci;
            idle_recovered = 1;
            cast_log("cast idle while device playing -> reload (active=%d)", ci);
        } else if (ci >= 0 && ci != last_synced && ci == prev_ci && msid > 0) {
            // Device index changed and held for one poll (a burst of skips debounces
            // to its final index). A +1 step off a track at its end is the natural
            // advance the receiver makes itself on the per-item WAV's EOF: bookkeep
            // only. Anything else (forward/backward skip, jump, out-of-window) reloads.
            int natural = (ci == last_synced + 1) && (last_dur_ms > 0) &&
                          (last_pos_ms >= last_dur_ms - CAST_NATURAL_END_GAP_MS);
            if (natural) {
                cast_log("zune natural advance %d->%d (pos=%d/%d) - receiver self-advances",
                         last_synced, ci, last_pos_ms, last_dur_ms);
            } else {
                link_enqueue(L, NS_MEDIA, build_queue_load(L->myip, L->media_port, q, L->ring));
                cast_log("zune skip %d->%d -> reload", last_synced, ci);
            }
            last_synced = ci;
        } else if (msid > 0 && ci == last_synced &&
                   ((int)q->serving_index <= last_synced - 2 || (int)q->serving_index >= last_synced + 2)) {
            // The receiver moved to an item the device is NOT naturally heading toward:
            // a >= 2 jump in either direction. A +/-1 difference is ambiguous and left
            // for the device to drive: across a track boundary the receiver self-advances
            // +1 on the BUFFERED item's EOF while its serving_index lags the device by 1,
            // so treating a -1 as a backward skip would MoveTo the device back onto the
            // just-finished track (the replay-the-first-track bug). Mirror it onto the
            // device. MoveTo is synchronous; re-read the index so bookkeeping sees it.
            int si = (int)q->serving_index;
            zdk_move_to(si);
            cast_log("cast skip -> MoveTo(%d) (device was at %d)", si, ci);
            ci = zdk_active_index();
            q->device_index = ci;
            last_synced = si;
        }

        if (!L->cast_idle) idle_recovered = 0;
        if (ci != prev_ci) {
            if (L->ring) L->ring->rate_dirty = 1;   // device track changed: capture re-reads CAR
            prev_ci = ci;
        }

        // Reconciled play/pause across both controllers (acts only on a real
        // difference, so each side's confirming echo is a no-op). cast_ps is set by
        // the I/O thread from MEDIA_STATUS; zme_play_state is device truth, read
        // fresh here so a cast→device change above isn't re-pushed back.
        int cps = (int)L->cast_ps;
        if (cps == 1 && play_paused != 1) {
            zdk_set_play_state(2); play_paused = 1; cast_log("cast PAUSED -> zune pause");
        } else if (cps == 0 && play_paused != 0) {
            zdk_set_play_state(1); play_paused = 0; cast_log("cast PLAYING -> zune resume");
        }
        if (msid > 0) {
            int zps = zme_play_state();
            if (zps == ZME_PS_PAUSED && play_paused != 1) {
                play_paused = 1;
                link_enqueue(L, NS_MEDIA, fmt_cmd(
                    "{\"type\":\"PAUSE\",\"mediaSessionId\":%d,\"requestId\":%d}", msid));
                cast_log("zune pause -> cast PAUSE");
            } else if (zps == ZME_PS_PLAYING && play_paused != 0) {
                play_paused = 0;
                link_enqueue(L, NS_MEDIA, fmt_cmd(
                    "{\"type\":\"PLAY\",\"mediaSessionId\":%d,\"requestId\":%d}", msid));
                cast_log("zune play -> cast PLAY");
            }
        }

        // device -> cast. The push section reflects every on-screen change (incl.
        // the ones we set below from the cast; flag=1 publishes). Act only when the
        // device value differs from the reconciled step, so a cast->device set's
        // echo through the section is a no-op.
        if (vstate != NULL && vstate->seq != last_vol_seq && vstate->max != 0) {
            last_vol_seq = vstate->seq;
            if ((int)vstate->vol != recon_vol) {
                recon_vol = (int)vstate->vol;
                enqueue_set_volume(L, vstate->vol, vstate->max);
                // Hold off cast->device so the receiver's lagging echo of THIS
                // SET_VOLUME can't bounce the device back a step mid-change. Rapid
                // device changes keep refreshing the window; it ends ~CAST_VOL_ECHO_MS
                // after the last change, by when the echo has settled to recon_vol.
                suppress_cast_until = GetTickCount() + CAST_VOL_ECHO_MS;
            }
        }

        // cast -> device. Act only on a genuine receiver-side change (cast_vol_milli
        // moved), not merely a difference from recon_vol; otherwise a stale receiver
        // level would fight a device change until the receiver echoes our SET_VOLUME.
        // The receiver's level (0..1) quantizes to a device step; the device-side echo
        // (push section updating to this step via flag=1) then matches recon_vol above
        // and does not bounce back.
        if (L->cast_vol_milli >= 0 && L->cast_vol_milli != last_cast_milli &&
            (long)(GetTickCount() - suppress_cast_until) >= 0) {
            int mx = (vstate && vstate->max) ? (int)vstate->max : 30;
            int step;
            last_cast_milli = L->cast_vol_milli;     // consume only once the window is open
            step = ((int)last_cast_milli * mx + 500) / 1000;   // round level*max
            if (step < 0) step = 0; else if (step > mx) step = mx;
            if (step != recon_vol) {
                recon_vol = step;
                zam_set_volume((unsigned int)step, (unsigned int)mx);
                cast_log("cast vol %ld/1000 -> zune SET %d/%d", (long)last_cast_milli, step, mx);
            }
        }

        WaitForSingleObject(L->stop, 200);
    }
    if (vstate)  UnmapViewOfFile((void*)vstate);
    if (vol_sec) CloseHandle(vol_sec);
    cast_log("RECONCILE exit");
    return 0;
}

// ── Cast I/O thread (this function; owns wolfSSL, never blocks on the device) ─
int castv2_run(HANDLE stop_event, const char* target_ip,
               unsigned short control_port, unsigned short media_port,
               CastQueue* q, CaptureRing* ring)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { cast_log("CTRL socket fail %d", WSAGetLastError()); return -1; }

    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(control_port);
    a.sin_addr.s_addr = inet_addr(target_ip);
    if (connect_with_timeout(s, &a, 4000) != 0) {
        cast_log("CTRL connect fail %s:%d err=%d", target_ip, control_port, WSAGetLastError());
        closesocket(s);
        return -2;
    }

    char myip[32];
    local_ip(s, myip, sizeof(myip));

    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (ctx == NULL) { cast_log("CTX-NEW-FAIL"); closesocket(s); return -3; }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);

    WOLFSSL* ssl = wolfSSL_new(ctx);
    if (ssl == NULL) { cast_log("SSL-NEW-FAIL"); wolfSSL_CTX_free(ctx); closesocket(s); return -3; }
    wolfSSL_set_fd(ssl, (int)s);
    wolfSSL_set_using_nonblock(ssl, 1);

    if (tls_handshake(ssl, s, stop_event) != 0) {
        wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); closesocket(s);
        return -4;
    }
    cast_log("TLS up local=%s", myip);
    // Status stays "connecting" until the app CONNECTs (reconcile_thread promotes
    // it to connected/casting from the live link state).

    CastLink link;
    memset(&link, 0, sizeof(link));
    InitializeCriticalSection(&link.lock);
    link.stop       = CreateEvent(NULL, TRUE, FALSE, NULL);   // manual-reset
    link.connected  = 0;
    link.msid       = 0;
    link.cast_ps    = -1;
    link.cast_idle  = 0;
    link.cast_vol_milli = -1;
    link.myip       = myip;
    link.media_port = media_port;
    link.q          = q;
    link.ring       = ring;
    link.target_ip  = target_ip;
    link.target_port = control_port;
    HANDLE hrec = CreateThread(NULL, 0, reconcile_thread, &link, 0, NULL);

    int  req_id = 1;
    char buf[640];
    char tid[64] = { 0 };           // app transportId: I/O thread only (it fills dst)
    int  connected_app = 0;
    int  launched = 0;

    // CONNECT + GET_STATUS, not a blind LAUNCH: the CC1AD845 app outlives our process,
    // so on a reconnect/restart it is usually still running with its old queue. JOIN it
    // on the RECEIVER_STATUS instead of re-LAUNCHing; a relaunch makes that stale session
    // re-fetch its pre-restart item and double-serve it.
    send_message(ssl, SENDER_ID, RECEIVER_ID, NS_CONNECTION, "{\"type\":\"CONNECT\"}");
    _snprintf(buf, sizeof(buf), "{\"type\":\"GET_STATUS\",\"requestId\":%d}", req_id++);
    send_message(ssl, SENDER_ID, RECEIVER_ID, NS_RECEIVER, buf);
    cast_log("GET_STATUS sent");

    unsigned char rx[16384];
    int   rxn = 0;
    DWORD last_ping = GetTickCount();
    DWORD last_rx   = GetTickCount();   // last inbound byte/PONG: a half-open power-save socket stops delivering these

    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        fd_set rf;
        FD_ZERO(&rf); FD_SET(s, &rf);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
        if (select(0, &rf, NULL, NULL, &tv) > 0) {
            unsigned char tmp[4096];
            int rc = wolfSSL_read(ssl, tmp, sizeof(tmp));
            if (rc > 0) {
                last_rx = GetTickCount();
                if (rxn + rc <= (int)sizeof(rx)) { memcpy(rx + rxn, tmp, rc); rxn += rc; }
                else { rxn = 0; }
            } else {
                int e = wolfSSL_get_error(ssl, rc);
                if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
                    cast_log("CTRL read err wolfSSL=%d", e);
                    break;
                }
            }
        }

        while (rxn >= 4) {
            unsigned int flen = ((unsigned)rx[0] << 24) | ((unsigned)rx[1] << 16) |
                                ((unsigned)rx[2] << 8) | (unsigned)rx[3];
            if (flen > sizeof(rx) - 4) { rxn = 0; break; }
            if (rxn < 4 + (int)flen) break;

            char pay[4096];
            if (dec_payload(rx + 4, (int)flen, pay, sizeof(pay))) {
                // Receiver volume rides RECEIVER_STATUS (broadcast on every cast-side
                // volume change); capture it regardless of connected_app so the
                // reconcile thread can mirror cast->device.
                if (strstr(pay, "RECEIVER_STATUS") != NULL) {
                    int mv = json_get_volume_milli(pay);
                    if (mv >= 0) link.cast_vol_milli = mv;
                }
                if (strstr(pay, "\"PING\"") != NULL) {
                    send_message(ssl, SENDER_ID, RECEIVER_ID, NS_HEARTBEAT, "{\"type\":\"PONG\"}");
                } else if (!connected_app && strstr(pay, "RECEIVER_STATUS") != NULL) {
                    int app_running = (strstr(pay, "\"" CAST_RECEIVER_APP_ID "\"") != NULL);
                    if (app_running && json_get_string(pay, "transportId", tid, sizeof(tid))) {
                        // CC1AD845 is already running: JOIN it (no LAUNCH) so we don't
                        // restart its session and double-serve the in-flight item.
                        send_message(ssl, SENDER_ID, tid, NS_CONNECTION, "{\"type\":\"CONNECT\"}");
                        cast_log("JOIN running app tid=%s", tid);
                        connected_app = 1;
                        link.connected = 1;     // reconcile thread builds the first QUEUE_LOAD
                    } else if (!launched) {
                        // Not running: launch it; the next RECEIVER_STATUS carries the
                        // new app's transportId and we JOIN that.
                        _snprintf(buf, sizeof(buf),
                                  "{\"type\":\"LAUNCH\",\"requestId\":%d,\"appId\":\"%s\"}",
                                  req_id++, CAST_RECEIVER_APP_ID);
                        send_message(ssl, SENDER_ID, RECEIVER_ID, NS_RECEIVER, buf);
                        launched = 1;
                        cast_log("LAUNCH sent appId=%s (not running)", CAST_RECEIVER_APP_ID);
                    }
                } else if (strstr(pay, "MEDIA_STATUS") != NULL) {
                    int v;
                    if (json_get_int(pay, "mediaSessionId", &v)) link.msid = v;
                    char pstate[24];
                    if (json_get_string(pay, "playerState", pstate, sizeof(pstate))) {
                        if (!strcmp(pstate, "PAUSED"))       { link.cast_ps = 1; link.cast_idle = 0; }
                        else if (!strcmp(pstate, "PLAYING")) { link.cast_ps = 0; link.cast_idle = 0; }
                        else if (!strcmp(pstate, "IDLE")) {
                            // Receiver played to the end of its loaded window
                            // (idleReason FINISHED) while the device keeps playing;
                            // the reconcile thread reloads to re-window and resume. A
                            // stop/cancel (other idleReasons) is not that recovery case.
                            char reason[24];
                            if (json_get_string(pay, "idleReason", reason, sizeof(reason)) &&
                                !strcmp(reason, "FINISHED"))
                                link.cast_idle = 1;
                        }
                        // BUFFERING: transient; leave the last known state untouched.
                    }
                }
            }

            int consumed = 4 + (int)flen;
            memmove(rx, rx + consumed, rxn - consumed);
            rxn -= consumed;
        }

        if (GetTickCount() - last_ping > 5000) {
            send_message(ssl, SENDER_ID, RECEIVER_ID, NS_HEARTBEAT, "{\"type\":\"PING\"}");
            last_ping = GetTickCount();
        }
        // Liveness deadline: a WiFi power-save half-open socket can swallow our PINGs
        // without ever erroring wolfSSL_read, wedging for tens of seconds. Reconnect if
        // no inbound byte (incl. PONG) has arrived for 3 ping intervals.
        if (GetTickCount() - last_rx > 15000) {
            cast_log("CTRL liveness timeout (no rx) -> reconnect");
            break;
        }

        // Send whatever the reconcile thread queued. Media commands go to the app
        // transportId; receiver-namespace commands (SET_VOLUME) go to receiver-0.
        if (connected_app) {
            OutMsg* m;
            while ((m = link_dequeue(&link)) != NULL) {
                const char* dst = (strcmp(m->ns, NS_RECEIVER) == 0) ? RECEIVER_ID : tid;
                send_message(ssl, SENDER_ID, dst, m->ns, m->payload);
                free(m->payload);
                free(m);
            }
        }
    }

    // Stop the receiver's playback before dropping the link. Without this a
    // session end leaves the old receiver playing: it keeps our app running and
    // re-fetches the (restarted) HTTP stream, so a device re-selection would stream
    // to BOTH the old and new device at once. Only meaningful with a live media
    // session (msid>0); the connection is still up on a toggle-off/reselect stop,
    // and this is a no-op if the link already dropped.
    if (connected_app && tid[0] && link.msid > 0) {
        char stopmsg[96];
        _snprintf(stopmsg, sizeof(stopmsg),
                  "{\"type\":\"STOP\",\"mediaSessionId\":%ld,\"requestId\":%d}",
                  (long)link.msid, req_id++);
        send_message(ssl, SENDER_ID, tid, NS_MEDIA, stopmsg);
        cast_log("STOP sent to receiver (msid=%ld) on session end", (long)link.msid);
    }

    // Tear down the reconcile thread before touching wolfSSL state.
    SetEvent(link.stop);
    if (hrec) { WaitForSingleObject(hrec, 3000); CloseHandle(hrec); }
    { OutMsg* m; while ((m = link_dequeue(&link)) != NULL) { free(m->payload); free(m); } }
    DeleteCriticalSection(&link.lock);
    CloseHandle(link.stop);

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(ctx);
    closesocket(s);
    cast_log("CTRL exit connected=%d", connected_app);
    return 0;
}
