#include "castaudio.h"
#include "zme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int send_all(SOCKET s, const char* p, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static void put_u32le(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char)(v); p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void put_u16le(unsigned char* p, unsigned short v)
{
    p[0] = (unsigned char)(v); p[1] = (unsigned char)(v >> 8);
}

void castaudio_build_wav_header(unsigned char* h, unsigned int data_len, unsigned int sample_rate)
{
    const unsigned int sr = sample_rate, ch = CAST_CHANNELS, bits = CAST_BITS;
    memcpy(h + 0, "RIFF", 4);
    put_u32le(h + 4, 36 + data_len);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_u32le(h + 16, 16);
    put_u16le(h + 20, 1);                                 // PCM
    put_u16le(h + 22, (unsigned short)ch);
    put_u32le(h + 24, sr);
    put_u32le(h + 28, sr * ch * (bits / 8));              // byte rate
    put_u16le(h + 32, (unsigned short)(ch * (bits / 8))); // block align
    put_u16le(h + 34, (unsigned short)bits);
    memcpy(h + 36, "data", 4);
    put_u32le(h + 40, data_len);
}

unsigned char* castaudio_build_test_wav(int* out_len)
{
    const int sr = CAST_SAMPLE_RATE, ch = CAST_CHANNELS, secs = 6;
    const int frames   = sr * secs;
    const int data_len = frames * ch * (CAST_BITS / 8);
    const int total    = 44 + data_len;

    unsigned char* buf = (unsigned char*)malloc(total);
    if (buf == NULL) return NULL;
    castaudio_build_wav_header(buf, (unsigned int)data_len, CAST_SAMPLE_RATE);

    short* s = (short*)(buf + 44);
    double phase = 0.0;
    const double inc = 2.0 * 3.14159265358979 * 440.0 / (double)sr;
    for (int i = 0; i < frames; i++) {
        short v = (short)(sin(phase) * 0.2 * 32767.0);
        s[i * 2] = v; s[i * 2 + 1] = v;
        phase += inc;
        if (phase > 6.283185307179586) phase -= 6.283185307179586;
    }
    *out_len = total;
    return buf;
}

// PoC 0 fallback: serve the finite WAV with a Content-Length.
static void serve_static_wav(SOCKET cs, HttpMediaCtx* c)
{
    char hdr[256];
    int hn = _snprintf(hdr, sizeof(hdr),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: audio/wav\r\n"
                       "Content-Length: %d\r\n"
                       "Accept-Ranges: none\r\n"
                       "Connection: close\r\n\r\n",
                       c->wav_len);
    if (hn > 0 && send_all(cs, hdr, hn) == 0)
        send_all(cs, (const char*)c->wav, c->wav_len);
}

// Serves queue item `idx` (Zune track index from /g<gen>/q<idx>.wav) as a finite,
// Content-Length'd WAV, so the receiver shows a progress bar, preloads the next
// item (preload keeps the boundary handoff gapless), and auto-advances on the last
// byte. A length-unbounded / chunked stream would end exactly at the
// boundary, but the receiver treats it as LIVE and rebuffers a multi-second gap at
// every boundary, so the Content-Length is mandatory. The item is sized to the
// track's plain metadata duration: the per-boundary re-anchor below corrects any
// residual at the true re-map head, so there is nothing for a learned trim to predict.
//
// The PCM is one continuous gapless stream; a track boundary is just where one
// HTTP response ends and the next begins. A single persistent cursor
// (`CastQueue.consume_cursor`) carries across items, so the next item begins at the
// next track's first sample. The cursor (re)initialises to "now" on the first serve
// after a QUEUE_LOAD (joining the active track mid-play; the control side sizes that
// item to the *remaining* time), and re-anchors to the new track's precise head at
// each track boundary (epoch flip). Skip decisions are owned entirely by the
// reconcile thread; the serve only reports the requested index in `serving_index`.
static void serve_live(SOCKET cs, HttpMediaCtx* c, int idx)
{
    CastQueue*   q = c->q;
    CaptureRing* r = c->ring;

    if (q && idx >= 0) q->serving_index = idx;
    LONG reseat = -1;
    if (q && q->consume_cursor < 0)
        reseat = r->produced;                // join before any load (no anchored cursor yet)

    // Self-correcting realign: on a real track boundary (epoch flip since we were
    // last aligned) where the continuous cursor has drifted far from the new
    // track's head (e.g. after an overrun dropped blocks), snap it back so a
    // glitch is bounded to one track instead of permanently offsetting the queue.
    int realigned = 0;
    LONG cur_epoch = r->track_epoch;
    if (reseat < 0 && q && cur_epoch != q->aligned_epoch) {
        // Re-anchor to the new track's precise head at EVERY track boundary (epoch
        // flip). epoch_produced is the re-map's measured true start of the track in
        // the ring (not a metadata prediction), so per-track sizing error self-
        // corrects each boundary instead of accumulating. The handoff is gapless when
        // the prior item ended at the true boundary; any residual is bounded to one
        // track.
        reseat = r->epoch_produced;
        realigned = 1;
        cast_log("serve q%d re-anchor: cursor=%ld -> head=%ld drift=%ld (epoch %ld->%ld)",
                 idx, (long)q->consume_cursor, (long)r->epoch_produced,
                 (long)(q->consume_cursor - r->epoch_produced),
                 (long)q->aligned_epoch, (long)cur_epoch);
    }
    if (q) q->aligned_epoch = cur_epoch;

    int dur_ms = 0;
    if (q && idx >= q->base && idx < q->base + q->count) dur_ms = q->dur_ms[idx - q->base];
    if (dur_ms <= 0) {
        // The queue metadata duration was unresolved: a newly-added or post-reconnect
        // track whose zdk_queue_track read returned 0 at QUEUE_LOAD. The served item is
        // the device's active track, whose true duration the live ZME0 broker always
        // knows; resolve from it rather than skipping the track with a bounded clip.
        unsigned int hr = 0, dms = 0;
        if (zme_read(0x2f, &hr, &dms) && hr == 0 && dms > 0) dur_ms = (int)dms;
    }
    // Size to the plain metadata duration; the per-boundary re-anchor above corrects
    // drift at the source (the true re-map head).
    int sized_ms = dur_ms;
    // The WAV is tagged at the track's per-track rate (44.1k or 48k, read from CAR),
    // and the header is unrevisable once sent: a 44.1k track tagged at the unmeasured
    // default plays ~8.8% fast for its whole duration. Wait for the capture's first
    // CAR read (sample_rate > 0) before tagging: span-acquire only succeeds with audio
    // flowing (CAR already reprogrammed), so the first read is the correct rate. This
    // blocks only the very first serve: steady state already has a rate; the timeout
    // fallback is hit only when no audio is playing (nothing to serve anyway).
    DWORD rate_wait_t0 = GetTickCount();
    while (r->sample_rate <= 0 && WaitForSingleObject(c->stop_event, 0) != WAIT_OBJECT_0 &&
           GetTickCount() - rate_wait_t0 < 1000) {
        Sleep(10);
    }
    const unsigned int sample_rate = (r->sample_rate > 0) ? (unsigned int)r->sample_rate : CAST_SAMPLE_RATE;
    const unsigned int byterate = sample_rate * CAST_CHANNELS * (CAST_BITS / 8);
    unsigned int data_len;
    if (sized_ms > 0) {
        data_len = (unsigned int)(((unsigned __int64)sized_ms * byterate / 1000) & ~3u);   // frame-aligned
    } else {
        // Fail closed: a track whose duration never resolved (e.g. a 0-duration read
        // after a device-node churn) must NOT become an unbounded stream the receiver
        // can never finish: that sticks the whole queue. Serve one bounded second so
        // the receiver advances past the bad item instead of wedging.
        cast_log("serve q%d: unresolved duration -> 1s bounded fallback", idx);
        data_len = byterate & ~3u;
    }

    char hdr[256];
    int hn = _snprintf(hdr, sizeof(hdr),
                       "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n"
                       "Content-Length: %u\r\nConnection: close\r\n\r\n",
                       (unsigned int)(44 + data_len));
    if (hn <= 0 || send_all(cs, hdr, hn) != 0) return;

    unsigned char wh[44];
    castaudio_build_wav_header(wh, data_len, sample_rate);
    if (send_all(cs, (const char*)wh, 44) != 0) return;

    // Continuous cursor: skip/join reseat it; otherwise resume the prior item.
    LONG consumed = (reseat >= 0 || !q) ? (reseat >= 0 ? reseat : r->produced) : q->consume_cursor;
    LONG p0 = r->produced;                                  // clamp if the cursor aged out of the ring
    LONG start_clamped = 0;
    if (p0 - consumed > (LONG)CAP_RING_SLOTS) { consumed = p0 - (LONG)CAP_RING_SLOTS; start_clamped = 1; }
    if (consumed < 0) consumed = 0;
    // backlog (produced - consumed) is the real cursor-health metric: a healthy
    // continuous cursor sits a few tens of blocks behind the producer.
    cast_log("serve q%d start: %s consumed=%ld produced=%ld backlog=%ld clamp=%ld epoch=%ld data_len=%u",
             idx, realigned ? "re-anchor" : (reseat >= 0 ? "join-now" : "continue"),
             consumed, p0, p0 - consumed, start_clamped, (long)cur_epoch, data_len);

    unsigned int sent = 0;
    int   overruns = 0;          // times the consumer fell >ring behind and dropped blocks
    LONG  dropped  = 0;          // total blocks skipped by the overrun guard
    while (sent < data_len && WaitForSingleObject(c->stop_event, 0) != WAIT_OBJECT_0) {
        LONG p = r->produced;
        if (p == consumed) { Sleep(5); continue; }
        if (p - consumed > (LONG)CAP_RING_SLOTS) {
            dropped += (p - (LONG)CAP_RING_SLOTS) - consumed;
            consumed = p - (LONG)CAP_RING_SLOTS;
            overruns++;
        }
        DWORD slot = (DWORD)consumed % CAP_RING_SLOTS;
        unsigned int n = CAP_SLOT_SIZE;
        if (n > data_len - sent) n = data_len - sent;
        if (send_all(cs, (const char*)(r->buf + slot * CAP_SLOT_SIZE), (int)n) != 0) {
            cast_log("serve q%d send-fail: sent=%u overruns=%d dropped=%ld", idx, sent, overruns, dropped);
            if (q) q->consume_cursor = consumed;   // let the next item resume from here
            return;
        }
        sent += n;
        consumed++;
        if (q) q->consume_cursor = consumed;       // continuous handoff to the next item
    }
    cast_log("serve q%d done: sent=%u overruns=%d dropped_blocks=%ld next_cursor=%ld",
             idx, sent, overruns, dropped, consumed);
}

// One detached worker per accepted connection so a long-lived audio stream
// (serve_live runs for a whole track) does not block a concurrent request for
// the next queue item.
typedef struct { SOCKET cs; HttpMediaCtx* c; } HttpConn;

static DWORD WINAPI http_conn_worker(LPVOID arg)
{
    HttpConn* hc = (HttpConn*)arg;
    SOCKET cs = hc->cs;
    HttpMediaCtx* c = hc->c;
    free(hc);

    char req[1024];
    int got = 0;
    while (got < (int)sizeof(req) - 1) {
        int n = recv(cs, req + got, (int)sizeof(req) - 1 - got, 0);
        if (n <= 0) break;
        got += n;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n") != NULL) break;
    }
    char path[64] = "?";
    if (strncmp(req, "GET ", 4) == 0) {
        const char* s = req + 4; int i = 0;
        while (s[i] && s[i] != ' ' && i < (int)sizeof(path) - 1) { path[i] = s[i]; i++; }
        path[i] = 0;
    }
    cast_log("HTTP GET %s (%d bytes)", path, got);

    // /g<gen>/q<idx>.wav: serve only the CURRENT generation. A GET for a stale
    // generation (a leftover receiver session re-fetching its pre-restart item, or a
    // legacy no-gen path) is 404'd, never served with a confused duration.
    int serve_idx = -1;
    if (c->ring && path[0] == '/' && path[1] == 'g') {
        long req_gen = atol(path + 2);
        const char* qp = strstr(path + 2, "/q");
        int idx = qp ? atoi(qp + 2) : -1;
        if (idx >= 0 && c->q && req_gen == c->q->generation) serve_idx = idx;
        else cast_log("HTTP stale-gen %s (cur %ld) -> 404", path, (long)(c->q ? c->q->generation : -1));
    } else if (c->ring && path[0] == '/' && path[1] == 'q') {
        cast_log("HTTP legacy no-gen %s -> 404", path);
    }

    if (serve_idx >= 0) {
        serve_live(cs, c, serve_idx);
    } else if (path[0] == '/' && !c->ring) {
        serve_static_wav(cs, c);
    } else {
        static const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(cs, nf, (int)strlen(nf));
    }
    closesocket(cs);
    return 0;
}

DWORD WINAPI http_media_thread(LPVOID arg)
{
    HttpMediaCtx* c = (HttpMediaCtx*)arg;

    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) { cast_log("HTTP socket fail %d", WSAGetLastError()); return 1; }
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(c->port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        cast_log("HTTP bind fail port=%u err=%d", c->port, WSAGetLastError());
        closesocket(ls);
        return 2;
    }
    listen(ls, 4);
    cast_log("HTTP listening port=%u mode=%s", c->port, c->ring ? "live" : "tone");

    while (WaitForSingleObject(c->stop_event, 0) != WAIT_OBJECT_0) {
        fd_set rf;
        FD_ZERO(&rf); FD_SET(ls, &rf);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
        if (select(0, &rf, NULL, NULL, &tv) <= 0) continue;

        SOCKET cs = accept(ls, NULL, NULL);
        if (cs == INVALID_SOCKET) continue;

        HttpConn* hc = (HttpConn*)malloc(sizeof(HttpConn));
        if (hc == NULL) { closesocket(cs); continue; }
        hc->cs = cs; hc->c = c;
        HANDLE t = CreateThread(NULL, 0, http_conn_worker, hc, 0, NULL);
        if (t) CloseHandle(t);                  // detached; worker closes cs + frees hc
        else  { closesocket(cs); free(hc); }
    }

    closesocket(ls);
    cast_log("HTTP exit");
    return 0;
}
