#include "zlog.h"

void zlog(const char* tag, DWORD a, DWORD b, DWORD c) {
	HANDLE h = CreateFileW(L"\\flash2\\zpod-wk.log", GENERIC_WRITE,
	                       FILE_SHARE_READ, NULL, OPEN_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return;
	SetFilePointer(h, 0, NULL, FILE_END);
	char buf[112];
	int n = _snprintf(buf, sizeof(buf), "%s t=%u a=%08x b=%08x c=%08x\r\n",
	                  tag, (unsigned)GetTickCount(), a, b, c);
	if (n < 0 || n > (int)sizeof(buf)) n = (int)sizeof(buf);
	DWORD w;
	WriteFile(h, buf, (DWORD)n, &w, NULL);
	CloseHandle(h);
}
