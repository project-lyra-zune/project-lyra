/* Read and permute servicesd's music-kind now-playing queue for the Play Next mod.
   The queue object (fixed VA 0x41a53c70) holds the play order as a std::vector<u32>
   at +0x6c/70/74 (begin/end/cap), a permutation over the id-storage vector at +0x60;
   queue length is the play-order count. Cross-process via OpenProcess +
   Read/WriteProcessMemory, which needs the kerncore privilege bootstrap. Device-proven;
   see notes/re-2026-07-14-queue-insertitem-stub/. */

#include "playnext_queue.h"
#include <tlhelp32.h>

#define QOBJ            0x41a53c70u
#define OFF_ORD_BEGIN   0x6c
#define OFF_ORD_END     0x70

#define MAX_QUEUE       512   /* vector cap is ~400 slots */

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
    return (WriteProcessMemory(h, (LPVOID)va, buf, len, &put) && put == len) ? 0 : -1;
}

int queue_count(void) {
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

int queue_move_tail_next(int n, int cur) {
    HANDLE h;
    DWORD pid, hdr[0x120 / 4], ord_begin, ord_end;
    int count, old, i, k;
    DWORD order[MAX_QUEUE], neworder[MAX_QUEUE];

    if (n <= 0) return 0;
    pid = find_servicesd();
    if (!pid) return -1;
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) return -1;
    if (rmem(h, QOBJ, hdr, sizeof(hdr)) != 0) { CloseHandle(h); return -4; }
    ord_begin = hdr[OFF_ORD_BEGIN / 4];
    ord_end   = hdr[OFF_ORD_END / 4];
    count = (ord_end >= ord_begin) ? (int)((ord_end - ord_begin) / 4) : 0;
    old = count - n;                       /* pre-append length */
    if (old < 0 || count > MAX_QUEUE || ord_begin == 0) { CloseHandle(h); return -2; }
    if (cur < 0) cur = -1;
    if (cur >= old) { CloseHandle(h); return 0; }   /* current at/after tail: already next */

    if (rmem(h, ord_begin, order, (DWORD)count * 4) != 0) { CloseHandle(h); return -4; }
    k = 0;
    for (i = 0; i <= cur; i++)      neworder[k++] = order[i];       /* head incl. current */
    for (i = old; i < count; i++)   neworder[k++] = order[i];       /* the appended tail   */
    for (i = cur + 1; i < old; i++) neworder[k++] = order[i];       /* original remainder  */
    if (wmem(h, ord_begin, neworder, (DWORD)k * 4) != 0) { CloseHandle(h); return -4; }
    CloseHandle(h);
    return 0;
}
