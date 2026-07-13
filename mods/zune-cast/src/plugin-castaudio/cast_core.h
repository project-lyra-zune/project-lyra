#ifndef CAST_CORE_H
#define CAST_CORE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the cast-log critical section. Call once at process start before
// any thread (or cast_log call) exists. Both entries, the nativeapp plugin
// (RunDaemon) and the spawn_daemon exe (wWinMain), call it. Idempotent.
void cast_log_init(void);

// Run one cast session against `target`:`control_port`, serving live audio on
// `media_port`, until `session_stop` is signalled. Owns the capture + HTTP
// media threads and the wolfSSL CASTV2 control retry loop for the session's
// lifetime, then tears them down. wolfSSL_Init/Cleanup are the caller's (one
// wolfSSL lifetime spans many sessions). Returns 0 on clean stop, negative on
// a fatal setup failure (allocation).
int cast_run_session(const char* target, unsigned short control_port,
                     unsigned short media_port, HANDLE session_stop);

#ifdef __cplusplus
}
#endif

#endif // CAST_CORE_H
