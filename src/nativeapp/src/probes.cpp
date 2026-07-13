#include "probes.h"

static void ProbeWriteLineW(HANDLE f, const wchar_t* s) {
	DWORD bytes = (DWORD)(wcslen(s) * sizeof(wchar_t));
	DWORD written;
	WriteFile(f, s, bytes, &written, NULL);
	WriteFile(f, L"\r\n", 4, &written, NULL);
}

static void ProbeWriteLineFmt(HANDLE f, const wchar_t* fmt, ...) {
	wchar_t buf[640];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf(buf, sizeof(buf)/sizeof(buf[0]) - 1, fmt, ap);
	va_end(ap);
	buf[sizeof(buf)/sizeof(buf[0]) - 1] = 0;
	ProbeWriteLineW(f, buf);
}

// Walk a module's PE export directory under SEH. We're in nativeapp.exe
// (separate process from the UI host, kernel-privileged after hax()),
// so a fault here only kills the daemon - not compositor/gemstone.
static BOOL ProbeDumpExports(HANDLE f, BYTE* base) {
	if (base == NULL) return FALSE;
	__try {
		IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
		IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;
		IMAGE_DATA_DIRECTORY* dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (dd->Size == 0 || dd->VirtualAddress == 0) return FALSE;
		IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(base + dd->VirtualAddress);
		DWORD nname = exp->NumberOfNames;
		if (nname > 8192) nname = 8192;
		DWORD* names = (DWORD*)(base + exp->AddressOfNames);
		WORD*  ords  = (WORD*)(base + exp->AddressOfNameOrdinals);
		DWORD* funcs = (DWORD*)(base + exp->AddressOfFunctions);
		for (DWORD i = 0; i < nname; i++) {
			const char* name = (const char*)(base + names[i]);
			WORD ordinal = (WORD)(ords[i] + exp->Base);
			DWORD rva = funcs[ords[i]];
			ProbeWriteLineFmt(f, L"  ord=%u rva=0x%08lx name=%S", ordinal, rva, name);
		}
		return TRUE;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return FALSE;
	}
}

BOOL ProbePidToFile(DWORD target_pid, DWORD* out_err) {
	*out_err = 0;

	wchar_t path[MAX_PATH];
	_snwprintf(path, MAX_PATH - 1, L"\\flash2\\automation\\probe-%lu.log", target_pid);
	path[MAX_PATH - 1] = 0;

	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		*out_err = GetLastError();
		return FALSE;
	}

	ProbeWriteLineFmt(f, L"== nativeapp probe pid=%lu ticks=%lu ==", target_pid, GetTickCount());

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, target_pid);
	if (snap == INVALID_HANDLE_VALUE) {
		*out_err = GetLastError();
		ProbeWriteLineFmt(f, L"ERROR: CreateToolhelp32Snapshot failed err=%lu", *out_err);
		CloseHandle(f);
		return FALSE;
	}

	MODULEENTRY32 me;
	ZeroMemory(&me, sizeof(me));
	me.dwSize = sizeof(me);
	if (Module32First(snap, &me)) {
		do {
			ProbeWriteLineFmt(f, L"module base=0x%p size=%lu name=%s",
			                  me.modBaseAddr, me.modBaseSize, me.szModule);
			// Skip kernel-mode modules (k.*.dll), at kernel addresses
			// with layouts the user-mode PE walker isn't designed for.
			if (me.szModule[0] == L'k' && me.szModule[1] == L'.') {
				ProbeWriteLineW(f, L"  (skipped: kernel module)");
				continue;
			}
			BOOL ok = ProbeDumpExports(f, me.modBaseAddr);
			if (!ok) {
				ProbeWriteLineW(f, L"  (no exports / inaccessible / SEH skipped)");
			}
		} while (Module32Next(snap, &me));
	}
	CloseHandle(snap);

	CloseHandle(f);
	return TRUE;
}
