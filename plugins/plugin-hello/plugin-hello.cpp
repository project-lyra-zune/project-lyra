#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "ce_log.h"

// Sample plugin: proof of life for the opcode 20 / opcode 18
// plugin-loading mechanisms. Exports both entry shapes:
//
//   Run(arg, arg_len, out, out_max, out_used)  : opcode 20 (daemon side).
//                                                Reads optional arg bytes,
//                                                writes a UTF-8 message
//                                                to out, returns 0.
//
//   Activate()                                   : opcode 18 (gemstone side).
//                                                Appends a line to
//                                                \flash2\automation\plugin-result.log
//                                                so the host can read back.
//
// Both shapes are present so the same DLL can be deployed to either
// path and exercise the contract end-to-end.

CE_LOGGER(hello_log, L"\\flash2\\automation\\plugin-result.log")

extern "C" __declspec(dllexport) int Run(
    const void* arg, int arg_len,
    void* out, int out_max,
    int* out_used)
{
    if (out == NULL || out_used == NULL) return -1;

    char buf[256];
    DWORD pid = GetCurrentProcessId();
    int n = _snprintf(buf, sizeof(buf) - 1,
        "plugin-hello: Run() called in pid=%lu arg_len=%d ticks=%lu",
        pid, arg_len, GetTickCount());
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    buf[n] = 0;

    int copy = n;
    if (copy > out_max) copy = out_max;
    memcpy(out, buf, copy);
    *out_used = copy;
    return 0;
}

extern "C" __declspec(dllexport) int Activate(void)
{
    hello_log("plugin-hello: Activate() called in pid=%lu ticks=%lu",
              GetCurrentProcessId(), GetTickCount());
    return 42;
}

extern "C" BOOL WINAPI DllMain(HANDLE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    return TRUE;
}
