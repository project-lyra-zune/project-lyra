/* Append kind-tagged song ids to servicesd's runtime (URL-song) now-playing queue for
   the YouTube native queue. The now-playing queue is per-kind: 0x41a53c70 (vtable
   0x41a13290) is the library-music queue; the URL/runtime kind-0xfd queue that
   PlaySongFromURL populates is the index-6 per-kind singleton at 0x41a525d4 (vtable
   0x41a126b8), same layout: id storage std::vector<u32> at +0x60/64/68, play-order
   permutation at +0x6c/70/74; playing_id = ids[order[cur]]. Cross-process via
   OpenProcess + Read/WriteProcessMemory (gemstone is privilege-bootstrapped by nativeapp
   at boot). See mods/playnext/src/playnext_queue.c. 
*/

#include "yt_queue.h"
#include <windows.h>
#include <tlhelp32.h>

#define QOBJ           0x41a525d4u
#define OFF_ID_END     0x64
#define OFF_ID_CAP     0x68
#define OFF_ID_BEGIN   0x60
#define OFF_ORD_BEGIN  0x6c
#define OFF_ORD_END    0x70
#define OFF_ORD_CAP    0x74
#define OFF_CUR        0xa4

static DWORD find_servicesd(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    DWORD pid = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (wcsstr(pe.szExeFile, L"servicesd")) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseToolhelp32Snapshot(snap);
    return pid;
}

static int rmem(HANDLE h, DWORD va, void* buf, DWORD len) {
    DWORD got = 0;
    return (ReadProcessMemory(h, (LPCVOID)va, buf, len, &got) && got == len) ? 0 : -1;
}
static int wmem(HANDLE h, DWORD va, const void* buf, DWORD len) {
    DWORD put = 0;
    return (WriteProcessMemory(h, (LPVOID)va, (LPVOID)buf, len, &put) && put == len) ? 0 : -1;
}

int yt_queue_count(void) {
    HANDLE h;
    DWORD pid, begin, end;
    int n;
    pid = find_servicesd();
    if (!pid) return -1;
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) return -1;
    if (rmem(h, QOBJ + OFF_ORD_BEGIN, &begin, 4) != 0 ||
        rmem(h, QOBJ + OFF_ORD_END, &end, 4) != 0) { CloseHandle(h); return -1; }
    n = (end >= begin) ? (int)((end - begin) / 4) : -1;
    CloseHandle(h);
    return n;
}

int yt_queue_push_id(unsigned int id) {
    HANDLE h;
    DWORD pid, id_begin, id_end, id_cap, si, val;
    pid = find_servicesd();
    if (!pid) return -1;
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) return -1;
    if (rmem(h, QOBJ + OFF_ID_BEGIN, &id_begin, 4) != 0 ||
        rmem(h, QOBJ + OFF_ID_END,   &id_end,   4) != 0 ||
        rmem(h, QOBJ + OFF_ID_CAP,   &id_cap,   4) != 0) { CloseHandle(h); return -1; }
    if (id_begin == 0 || id_end < id_begin || id_end + 4 > id_cap) { CloseHandle(h); return -1; }
    si = (id_end - id_begin) / 4;
    val = id;
    if (wmem(h, id_end, &val, 4) != 0) { CloseHandle(h); return -1; }
    val = id_end + 4;
    if (wmem(h, QOBJ + OFF_ID_END, &val, 4) != 0) { CloseHandle(h); return -1; }
    CloseHandle(h);
    return (int)si;
}

int yt_queue_set_order(const unsigned int* order, int n, int cur) {
    HANDLE h;
    DWORD pid, ord_begin, ord_cap, val;
    int i;
    if (n <= 0) return -1;
    pid = find_servicesd();
    if (!pid) return -1;
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) return -1;
    if (rmem(h, QOBJ + OFF_ORD_BEGIN, &ord_begin, 4) != 0 ||
        rmem(h, QOBJ + OFF_ORD_CAP,   &ord_cap,   4) != 0) { CloseHandle(h); return -1; }
    if (ord_begin == 0 || ord_begin + (DWORD)n * 4 > ord_cap) { CloseHandle(h); return -1; }
    for (i = 0; i < n; i++) {
        val = order[i];
        if (wmem(h, ord_begin + (DWORD)i * 4, &val, 4) != 0) { CloseHandle(h); return -1; }
    }
    val = ord_begin + (DWORD)n * 4;
    if (wmem(h, QOBJ + OFF_ORD_END, &val, 4) != 0) { CloseHandle(h); return -1; }
    val = (DWORD)(cur < 0 ? 0 : cur);
    if (wmem(h, QOBJ + OFF_CUR, &val, 4) != 0) { CloseHandle(h); return -1; }
    CloseHandle(h);
    return 0;
}
