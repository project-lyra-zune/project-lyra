#include "mods_log.h"

#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32

#include "ce_log.h"

/* The pipeline's phase logger writes one file at a time (Phase 1 boot.log,
   Phase 2 phase2.log) in a fast burst during boot, from a single thread. That
   is a ce_log stream: the handle stays open, so each line is a seek + write
   with none of the per-line open/lock the shared logger does. */
static ce_log_stream g_stream = { INVALID_HANDLE_VALUE, 0, 0 };

void ModsLogOpen(const wchar_t* path) {
    if (g_stream.h != INVALID_HANDLE_VALUE) return;
    ce_log_stream_open(&g_stream, path, 0, CE_LOG_STAMP_CLOCK, 1);
}

void ModsLogOpenAppend(const wchar_t* path) {
    if (g_stream.h != INVALID_HANDLE_VALUE) return;
    ce_log_stream_open(&g_stream, path, 0, CE_LOG_STAMP_CLOCK, 0);
}

void ModsLogOpenRotating(const wchar_t* path) {
    wchar_t prev[MAX_PATH];
    int n = 0;
    if (g_stream.h != INVALID_HANDLE_VALUE) return;
    while (path[n] && n < MAX_PATH - 6) { prev[n] = path[n]; n++; }
    if (n < MAX_PATH - 6) {
        prev[n] = L'.'; prev[n+1] = L'p'; prev[n+2] = L'r';
        prev[n+3] = L'e'; prev[n+4] = L'v'; prev[n+5] = 0;
        DeleteFileW(prev);
        MoveFileW(path, prev);
    }
    ce_log_stream_open(&g_stream, path, 0, CE_LOG_STAMP_CLOCK, 1);
}

void ModsLogClose(void) {
    ce_log_stream_close(&g_stream);
}

void ModsLogf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ce_stream_vlogf(&g_stream, fmt, ap);
    va_end(ap);
}

#else  /* host build (Linux/macOS) - log to stderr */

void ModsLogOpen(const wchar_t* path) { (void)path; }
void ModsLogOpenAppend(const wchar_t* path) { (void)path; }
void ModsLogOpenRotating(const wchar_t* path) { (void)path; }
void ModsLogClose(void) {}

void ModsLogf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#endif
