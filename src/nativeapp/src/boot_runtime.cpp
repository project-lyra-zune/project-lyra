#include "boot_runtime.h"

FileRuntimeState g_file_runtime = {false, ERROR_NOT_READY, 0};

void refresh_file_runtime_state() {
	WIN32_FIND_DATA ffd;
	HANDLE hFind = FindFirstFile(L"\\Flash2\\*", &ffd);
	if (hFind != INVALID_HANDLE_VALUE) {
		g_file_runtime.flash2_ready = true;
		g_file_runtime.flash2_error = ERROR_SUCCESS;
		FindClose(hFind);
	} else {
		g_file_runtime.flash2_ready = false;
		g_file_runtime.flash2_error = GetLastError();
	}
	g_file_runtime.last_refresh_tick = GetTickCount();
}

HANDLE open_file_with_retry(LPCWSTR path, DWORD* out_error, DWORD timeout_ms) {
	DWORD start = GetTickCount();
	while (true) {
		HANDLE f = CreateFileW(
			path,
			GENERIC_READ,
			FILE_SHARE_READ,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL);
		if (f != INVALID_HANDLE_VALUE) {
			if (out_error) {
				*out_error = ERROR_SUCCESS;
			}
			return f;
		}
		DWORD err = GetLastError();
		if (out_error) {
			*out_error = err;
		}
		if (GetTickCount() - start >= timeout_ms) {
			return INVALID_HANDLE_VALUE;
		}
		Sleep(250);
	}
}

HANDLE find_first_with_retry(LPCWSTR path, WIN32_FIND_DATA* ffd, DWORD* out_error, DWORD timeout_ms) {
	DWORD start = GetTickCount();
	while (true) {
		HANDLE hFind = FindFirstFile(path, ffd);
		if (hFind != INVALID_HANDLE_VALUE) {
			if (out_error) {
				*out_error = ERROR_SUCCESS;
			}
			return hFind;
		}
		DWORD err = GetLastError();
		if (out_error) {
			*out_error = err;
		}
		// empty dir is definitive, not a retryable flash-not-ready transient
		if (err == ERROR_NO_MORE_FILES) {
			return INVALID_HANDLE_VALUE;
		}
		if (GetTickCount() - start >= timeout_ms) {
			return INVALID_HANDLE_VALUE;
		}
		Sleep(250);
	}
}
