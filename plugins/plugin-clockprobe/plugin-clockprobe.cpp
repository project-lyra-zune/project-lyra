#include <windows.h>
#include <stdio.h>

// Kernel-clock probe (opcode 20, runs in nativeapp.exe). Reports the shared
// kernel clock that reposd/TLS read (GetLocalTime, GetSystemTime) and can drive
// SetLocalTime / SetSystemTime to test whether a clock write persists across a
// reboot. It also drives the WM8350 PMU RTC (the battery-backed master the
// bootloader seeds the SoC clock from at boot) via the user-mode libnvrm.dll
// NvRmPmu RTC API, which forwards to the kernel PMU driver in a proper thread so
// the I2C transaction completes (a raw kcall from the debug daemon does not).
// arg selects the operation; results go back over the wire (out buffer).
//
//   arg empty          -> read only (system clock + PMU RTC state)
//   arg[0]='L'         -> SetLocalTime(*(SYSTEMTIME*)(arg+4))   then re-read
//   arg[0]='S'         -> SetSystemTime(*(SYSTEMTIME*)(arg+4))  then re-read
//   arg[0]='W'         -> NvRmPmuWriteRtc(*(NvU32*)(arg+4))     then re-read RTC
//   arg layout for L/S: byte0 mode, bytes 4..19 SYSTEMTIME (8 WORDs LE)
//   arg layout for W:   byte0 'W', bytes 4..7 unix seconds (u32 LE)

typedef void* NvRmDeviceHandle;
typedef int   NvError;   // 0 == NvSuccess
typedef unsigned char NvBool;
typedef unsigned int  NvU32;
typedef NvError (*pfnNvRmOpen)(NvRmDeviceHandle*, NvU32);
typedef void    (*pfnNvRmClose)(NvRmDeviceHandle);
typedef NvBool  (*pfnNvRmPmuReadRtc)(NvRmDeviceHandle, NvU32*);
typedef NvBool  (*pfnNvRmPmuWriteRtc)(NvRmDeviceHandle, NvU32);
typedef NvBool  (*pfnNvRmPmuIsRtcInitialized)(NvRmDeviceHandle);

static int probe_pmu_rtc(char* b, int cap, const char* tag,
                         int do_write, NvU32 write_count) {
    HMODULE m = LoadLibraryW(L"libnvrm.dll");
    if (m == NULL) m = LoadLibraryW(L"nvrm.dll");
    if (m == NULL)
        return _snprintf(b, cap, " %s=nvrm_load_fail", tag);

    pfnNvRmOpen open = (pfnNvRmOpen)GetProcAddress(m, L"NvRmOpen");
    pfnNvRmClose close = (pfnNvRmClose)GetProcAddress(m, L"NvRmClose");
    pfnNvRmPmuReadRtc rd = (pfnNvRmPmuReadRtc)GetProcAddress(m, L"NvRmPmuReadRtc");
    pfnNvRmPmuWriteRtc wr = (pfnNvRmPmuWriteRtc)GetProcAddress(m, L"NvRmPmuWriteRtc");
    pfnNvRmPmuIsRtcInitialized isinit =
        (pfnNvRmPmuIsRtcInitialized)GetProcAddress(m, L"NvRmPmuIsRtcInitialized");
    if (open == NULL || rd == NULL)
        return _snprintf(b, cap, " %s=nvrm_proc_fail", tag);

    NvRmDeviceHandle h = NULL;
    NvError e = open(&h, 0);
    if (e != 0 || h == NULL)
        return _snprintf(b, cap, " %s=open_fail(e=%d)", tag, (int)e);

    int n = 0;
    if (do_write && wr != NULL) {
        NvBool wok = wr(h, write_count);
        n += _snprintf(b + n, cap - n, " %s_write(count=%lu)=%d",
                       tag, (unsigned long)write_count, (int)wok);
    }
    NvU32 cnt = 0;
    NvBool rok = rd(h, &cnt);
    NvBool inited = (isinit != NULL) ? isinit(h) : (NvBool)0xFF;
    n += _snprintf(b + n, cap - n, " %s_read_ok=%d count=%lu init=%d",
                   tag, (int)rok, (unsigned long)cnt, (int)inited);
    if (close != NULL) close(h);
    return (n < 0) ? 0 : n;
}

static int fmt_time(char* b, int cap, const char* tag, const SYSTEMTIME* t) {
    int n = _snprintf(b, cap, "%s=%04d-%02d-%02d %02d:%02d:%02d",
                      tag, t->wYear, t->wMonth, t->wDay, t->wHour, t->wMinute, t->wSecond);
    return (n < 0) ? 0 : n;
}

extern "C" __declspec(dllexport) int Run(
    const void* arg, int arg_len,
    void* out, int out_max,
    int* out_used)
{
    if (out == NULL || out_used == NULL) return -1;

    char buf[512];
    int len = 0;

    SYSTEMTIME lb, sb;
    GetLocalTime(&lb);
    GetSystemTime(&sb);
    len += fmt_time(buf + len, (int)sizeof(buf) - len, "local_before", &lb);
    buf[len++] = ' ';
    len += fmt_time(buf + len, (int)sizeof(buf) - len, "sys_before", &sb);

    char mode = (arg_len >= 1) ? ((const char*)arg)[0] : 'r';
    if ((mode == 'L' || mode == 'S') && arg_len >= 20) {
        SYSTEMTIME tgt;
        memcpy(&tgt, (const BYTE*)arg + 4, sizeof(SYSTEMTIME));
        BOOL ok = (mode == 'L') ? SetLocalTime(&tgt) : SetSystemTime(&tgt);
        DWORD err = ok ? 0 : GetLastError();
        len += _snprintf(buf + len, (int)sizeof(buf) - len,
                         " mode=%c set_ok=%d set_err=0x%08lx", mode, (int)ok, err);
        SYSTEMTIME la, sa;
        GetLocalTime(&la);
        GetSystemTime(&sa);
        buf[len++] = ' ';
        len += fmt_time(buf + len, (int)sizeof(buf) - len, "local_after", &la);
        buf[len++] = ' ';
        len += fmt_time(buf + len, (int)sizeof(buf) - len, "sys_after", &sa);
        len += probe_pmu_rtc(buf + len, (int)sizeof(buf) - len, "pmu", 0, 0);
    } else if (mode == 'W' && arg_len >= 8) {
        NvU32 count;
        memcpy(&count, (const BYTE*)arg + 4, sizeof(count));
        len += _snprintf(buf + len, (int)sizeof(buf) - len, " mode=W");
        len += probe_pmu_rtc(buf + len, (int)sizeof(buf) - len, "pmu", 1, count);
    } else {
        len += _snprintf(buf + len, (int)sizeof(buf) - len, " mode=r");
        len += probe_pmu_rtc(buf + len, (int)sizeof(buf) - len, "pmu", 0, 0);
    }

    if (len < 0) len = 0;
    if (len > (int)sizeof(buf)) len = (int)sizeof(buf);
    int copy = len;
    if (copy > out_max) copy = out_max;
    memcpy(out, buf, copy);
    *out_used = copy;
    return 0;
}

extern "C" BOOL WINAPI DllMain(HANDLE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    return TRUE;
}
