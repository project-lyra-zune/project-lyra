/* Set before any include so the file drops into a consumer whose mak does not
   already suppress the CRT deprecation warnings. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_SECURE_NO_DEPRECATE
#define _CRT_SECURE_NO_DEPRECATE
#endif

#include "ce_log.h"
#include <stdio.h>
#include <stdlib.h>

/* One lock per file, keyed by a hash of the path: CE6 kernel object names are
   system-wide, so the same path resolves to the same mutex across processes
   while different paths never contend. A collision only over-serializes.

   Handles are cached process-wide, so a burst of writes to one file pays the
   CreateMutexW/CloseHandle cost once, not per line. A named mutex refers to one
   kernel object however many handles exist, so a lost cache race is harmless. */
#define CE_LOCK_SLOTS   16
#define CE_LOCK_RESERVED ((LONG)-1)
static volatile LONG g_lk_key[CE_LOCK_SLOTS];   /* published hash key (>0); 0 empty; -1 reserving */
static HANDLE        g_lk_h[CE_LOCK_SLOTS];

/* Returns the mutex for `path`. *owned is 1 only when the cache is full and the
   caller must CloseHandle it; 0 for a shared cached handle it must not close. */
static HANDLE ce_log_lock(const wchar_t* path, int* owned)
{
    unsigned int h = 2166136261u;
    const wchar_t* p = path;
    wchar_t name[24];
    LONG key;
    int i;

    for (; *p; p++) h = (h ^ (unsigned int)*p) * 16777619u;
    key = (LONG)(h | 1u);
    _snwprintf(name, sizeof(name) / sizeof(name[0]), L"ce_log_%08x", h);
    *owned = 0;

    for (i = 0; i < CE_LOCK_SLOTS; i++)
        if (g_lk_key[i] == key) return g_lk_h[i];

    for (i = 0; i < CE_LOCK_SLOTS; i++) {
        if (g_lk_key[i] != 0) continue;
        if (InterlockedCompareExchange(&g_lk_key[i], CE_LOCK_RESERVED, 0) == 0) {
            HANDLE m = CreateMutexW(NULL, FALSE, name);
            g_lk_h[i] = m;              /* set handle before publishing the key */
            g_lk_key[i] = key;
            return m;
        }
        if (g_lk_key[i] == key) return g_lk_h[i];
    }

    *owned = 1;                         /* cache full (never expected): one-off handle */
    return CreateMutexW(NULL, FALSE, name);
}

/* Keep the most recent 3/4 and drop the rest. No syscall lops off a file's
   front, so the retained tail is copied down over the file; it starts at the
   first newline so the log never begins mid-line. Runs once per 1/4-cap of
   growth, not per write. */
static void keep_tail(HANDLE h, unsigned long max_bytes)
{
    DWORD size = GetFileSize(h, NULL);
    DWORD keep = max_bytes - max_bytes / 4;
    DWORD start, got = 0, off = 0, w;
    char* buf;

    if (keep >= size) return;
    start = size - keep;
    buf = (char*)malloc(keep);
    if (!buf) {                         /* can't copy down: wipe rather than grow */
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        SetEndOfFile(h);
        return;
    }
    SetFilePointer(h, start, NULL, FILE_BEGIN);
    ReadFile(h, buf, keep, &got, NULL);
    while (off < got && buf[off] != '\n') off++;
    if (off < got) off++;
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    WriteFile(h, buf + off, got - off, &w, NULL);
    SetEndOfFile(h);
    free(buf);
}

/* Bounded append to an already-open handle: the shared write core for both the
   open-per-write path and the held stream. Only touches the file pointer and
   one WriteFile, so on a held handle it is the cheap (no flash open) path. */
static void write_at_end(HANDLE h, unsigned long max_bytes,
                         const void* bytes, unsigned long len)
{
    DWORD w, sz;
    if (max_bytes) {
        sz = GetFileSize(h, NULL);
        if (sz != INVALID_FILE_SIZE && sz >= max_bytes) keep_tail(h, max_bytes);
    }
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, bytes, len, &w, NULL);
}

/* Formats one CRLF-terminated line into `line`; returns its length. */
static int format_line(char* line, int cap, int stamp, const char* fmt, va_list ap)
{
    int n = 0, b;
    if (stamp == CE_LOG_STAMP_CLOCK) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        n = _snprintf(line, cap, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                      t.wMilliseconds);
    }
    if (n < 0 || n > cap - 2) n = cap - 2;
    b = _vsnprintf(line + n, cap - n - 2, fmt, ap);   /* reserve 2 for CRLF */
    if (b < 0 || b > cap - n - 2) b = cap - n - 2;
    n += b;
    line[n++] = '\r';
    line[n++] = '\n';
    return n;
}

static void log_append(const wchar_t* path, unsigned long max_bytes,
                       const void* bytes, unsigned long len)
{
    int    owned;
    HANDLE mx = ce_log_lock(path, &owned);
    DWORD  wr = mx ? WaitForSingleObject(mx, 2000) : WAIT_FAILED;
    int    held = (wr == WAIT_OBJECT_0 || wr == WAIT_ABANDONED);

    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        write_at_end(h, max_bytes, bytes, len);
        CloseHandle(h);
    }

    if (mx) {
        if (held) ReleaseMutex(mx);
        if (owned) CloseHandle(mx);
    }
}

void ce_vlogf(const ce_log* lg, const char* fmt, va_list ap)
{
    char line[768];
    unsigned long max;
    int n;

    if (!lg || !lg->path) return;
    max = lg->max_bytes ? lg->max_bytes : CE_LOG_DEFAULT_MAX_BYTES;
    n = format_line(line, (int)sizeof(line), lg->stamp, fmt, ap);
    log_append(lg->path, max, line, (unsigned long)n);
}

void ce_logf(const ce_log* lg, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ce_vlogf(lg, fmt, ap);
    va_end(ap);
}

/* ── held-handle stream (single-writer, high-frequency) ─────────────────── */

int ce_log_stream_open(ce_log_stream* s, const wchar_t* path,
                       unsigned long max_bytes, int stamp, int fresh)
{
    if (!s) return 0;
    s->h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       fresh ? CREATE_ALWAYS : OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (s->h == INVALID_HANDLE_VALUE) return 0;
    s->max_bytes = max_bytes;
    s->stamp = stamp;
    return 1;
}

void ce_stream_vlogf(ce_log_stream* s, const char* fmt, va_list ap)
{
    char line[768];
    int n;
    if (!s || s->h == INVALID_HANDLE_VALUE) return;
    n = format_line(line, (int)sizeof(line), s->stamp, fmt, ap);
    write_at_end(s->h, s->max_bytes, line, (unsigned long)n);
}

void ce_stream_logf(ce_log_stream* s, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ce_stream_vlogf(s, fmt, ap);
    va_end(ap);
}

void ce_log_stream_close(ce_log_stream* s)
{
    if (s && s->h != INVALID_HANDLE_VALUE) {
        CloseHandle(s->h);
        s->h = INVALID_HANDLE_VALUE;
    }
}
