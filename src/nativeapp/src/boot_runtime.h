#pragma once

#include "nativeapp_common.h"

struct FileRuntimeState {
	bool flash2_ready;
	DWORD flash2_error;
	DWORD last_refresh_tick;
};

extern FileRuntimeState g_file_runtime;

void refresh_file_runtime_state();

// Retrying CreateFileW / FindFirstFile. Both poll every 250ms until the
// filesystem responds or timeout_ms elapses. *out_error gets the last
// GetLastError() seen on a failed attempt (ERROR_SUCCESS on success).
HANDLE open_file_with_retry(LPCWSTR path, DWORD* out_error, DWORD timeout_ms);
HANDLE find_first_with_retry(LPCWSTR path, WIN32_FIND_DATA* ffd, DWORD* out_error, DWORD timeout_ms);
