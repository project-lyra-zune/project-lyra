#include <windows.h>
#include <stdio.h>

// Sample plugin: proof of life for the opcode 20 / opcode 18
// plugin-loading mechanisms. Exports both entry shapes:
//
//   Run(arg, arg_len, out, out_max, out_used)  : opcode 20 (daemon side).
//                                                Reads optional arg bytes,
//                                                writes a UTF-8 message
//                                                to out, returns 0.
//
//   Activate()                                   : opcode 18 (gemstone side).
//                                                Appends a UTF-16 line to
//                                                \flash2\automation\plugin-result-<pid>.log
//                                                so the host can read back.
//
// Both shapes are present so the same DLL can be deployed to either
// path and exercise the contract end-to-end.

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
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH - 1,
               L"\\flash2\\automation\\plugin-result-%lu.log",
               GetCurrentProcessId());
    path[MAX_PATH - 1] = 0;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    SetFilePointer(f, 0, NULL, FILE_END);

    wchar_t line[256];
    int n = _snwprintf(line, sizeof(line)/sizeof(line[0]) - 2,
        L"plugin-hello: Activate() called in pid=%lu ticks=%lu",
        GetCurrentProcessId(), GetTickCount());
    if (n < 0) n = 0;
    if (n > (int)(sizeof(line)/sizeof(line[0]) - 2)) n = (int)(sizeof(line)/sizeof(line[0]) - 2);
    line[n] = L'\r';
    line[n + 1] = L'\n';

    DWORD written;
    WriteFile(f, line, (n + 2) * sizeof(wchar_t), &written, NULL);
    CloseHandle(f);
    return 42;
}

extern "C" BOOL WINAPI DllMain(HANDLE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    return TRUE;
}
