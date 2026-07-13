#ifndef MODS_LOG_H
#define MODS_LOG_H

#ifdef _WIN32
#  include <windows.h>
#else
#  include <wchar.h>
#endif

/* Log writer for the mods pipeline.

   Each phase opens its own file (Phase 1: boot.log; Phase 2: phase2.log)
   so logs don't trample each other when both phases run in the same boot
   across different processes.
   Format: UTF-16 LE, CRLF line endings, matching existing zuxhook log
           conventions. CREATE_ALWAYS so each boot starts fresh. */

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void ModsLogOpen(const wchar_t* path);
/* Like ModsLogOpen but appends to an existing file instead of truncating.
   Use for logs whose history within one boot matters (modmgr.log
   accumulates OnInit + per-tap entries; without this, every call wipes
   the prior content and only the most recent line survives). */
void ModsLogOpenAppend(const wchar_t* path);
void ModsLogClose(void);

/* printf-style; %s is wchar_t* (W variant), %d/%x/%X/%lu as usual.
   Lines are CRLF-terminated automatically. */
void ModsLogf(const wchar_t* fmt, ...);

/* Per-call open/append/close writer to an arbitrary flash path, independent of
   the single global handle above. Each subsystem that logs to its own file wraps
   this behind a thin module facade (its log path is the module's identity).
   Opens OPEN_ALWAYS with FILE_SHARE_READ|WRITE, seeks to end, writes one
   CRLF-terminated UTF-16 LE line, closes. Same format specifiers as ModsLogf. */
void mods_flashlog(const wchar_t* path, const wchar_t* fmt, ...);
void mods_vflashlog(const wchar_t* path, const wchar_t* fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif
