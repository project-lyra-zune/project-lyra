#ifndef MODS_LOG_H
#define MODS_LOG_H

#ifdef _WIN32
#  include <windows.h>
#else
#  include <wchar.h>
#endif

/* Log writer for the mods pipeline, over the shared ce_log.

   Each phase opens its own file (Phase 1: boot.log; Phase 2: phase2.log)
   so logs don't trample each other when both phases run in the same boot
   across different processes. ModsLogOpen starts the file fresh each boot;
   ModsLogOpenAppend keeps it. Formats are ASCII: a wide argument takes %S. */

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
/* Like ModsLogOpen, but keeps the previous boot's file as <path>.prev. The
   recovery boot otherwise truncates the only record of the boot that failed. */
void ModsLogOpenRotating(const wchar_t* path);
void ModsLogClose(void);

/* printf-style; %S is wchar_t*, %s is char*, %d/%x/%X/%lu as usual.
   Lines are CRLF-terminated automatically. */
void ModsLogf(const char* fmt, ...);


#ifdef __cplusplus
}
#endif

#endif
