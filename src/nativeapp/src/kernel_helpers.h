#pragma once

#include "nativeapp_common.h"

// CreateThread entry that calls ExitThread((DWORD)x) immediately. Paired
// with the syscall-param-validation patch from hax(), GetExitCodeThread()
// becomes an arbitrary kernel write via the OutBuf parameter.
DWORD WINAPI thread_exit_with_value(void* x);

// Arbitrary kernel u32 write via the GetExitCodeThread exit-code path.
// This is the bootstrap primitive hax() uses to PLANT the gadget, so it
// must not depend on the gadget (chicken-and-egg). Gadget-based kernel
// read/write/memcpy live in kerncore (kerncore_kreadu32 / kerncore_kread
// / kerncore_kwriteb / kerncore_kmemcpy). Slow: ~20ms per call.
void kwr(DWORD kptr, DWORD val);
