#include "mods_log.h"

#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32

static HANDLE g_log = INVALID_HANDLE_VALUE;

void ModsLogOpen(const wchar_t* path) {
    if (g_log != INVALID_HANDLE_VALUE) return;
    g_log = CreateFileW(
        path,
        GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void ModsLogOpenAppend(const wchar_t* path) {
    if (g_log != INVALID_HANDLE_VALUE) return;
    g_log = CreateFileW(
        path,
        GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log != INVALID_HANDLE_VALUE) {
        SetFilePointer(g_log, 0, NULL, FILE_END);
    }
}

void ModsLogClose(void) {
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
}

void ModsLogf(const wchar_t* fmt, ...) {
    wchar_t buf[768];
    int n;
    va_list ap;
    DWORD written;
    if (g_log == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    n = _vsnwprintf(buf, (sizeof(buf)/sizeof(buf[0])) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = (sizeof(buf)/sizeof(buf[0])) - 2;
    buf[n]     = L'\r';
    buf[n + 1] = L'\n';
    WriteFile(g_log, buf, (DWORD)((n + 2) * sizeof(wchar_t)), &written, NULL);
}

void mods_vflashlog(const wchar_t* path, const wchar_t* fmt, va_list ap) {
    wchar_t buf[512];
    int cap = (sizeof(buf) / sizeof(buf[0])) - 2;
    int n;
    DWORD written;
    HANDLE f = CreateFileW(
        path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    n = _vsnwprintf(buf, cap, fmt, ap);
    if (n < 0 || n > cap) n = cap;
    buf[n]     = L'\r';
    buf[n + 1] = L'\n';
    WriteFile(f, buf, (DWORD)((n + 2) * sizeof(wchar_t)), &written, NULL);
    CloseHandle(f);
}

void mods_flashlog(const wchar_t* path, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mods_vflashlog(path, fmt, ap);
    va_end(ap);
}

#else  /* host build (Linux/macOS) - log to stderr */

#include <wchar.h>

void ModsLogOpen(const wchar_t* path) { (void)path; }
void ModsLogOpenAppend(const wchar_t* path) { (void)path; }
void ModsLogClose(void) {}

void ModsLogf(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(stderr, fmt, ap);
    va_end(ap);
    fputwc(L'\n', stderr);
}

void mods_vflashlog(const wchar_t* path, const wchar_t* fmt, va_list ap) {
    (void)path;
    vfwprintf(stderr, fmt, ap);
    fputwc(L'\n', stderr);
}

void mods_flashlog(const wchar_t* path, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mods_vflashlog(path, fmt, ap);
    va_end(ap);
}

#endif

