#ifndef CE_LOG_H
#define CE_LOG_H

#include <windows.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flash logger shared by the platform and by mods. Nothing prunes a log on
   flash, so the writer owns its ceiling: at max_bytes the oldest lines are
   dropped and the most recent portion kept. A per-path named mutex serializes
   writes to each file, so separate processes may share one log path (e.g. a
   daemon and an in-process module) without interleaving.

   A module declares its logger in one line and calls it printf-style:
       CE_LOGGER(cast_log, L"\\flash2\\automation\\zune-cast.log")
       cast_log("SESSION-START target=%s", target); */

#define CE_LOG_DEFAULT_MAX_BYTES (256u * 1024u)

enum {
    CE_LOG_STAMP_NONE  = 0,
    CE_LOG_STAMP_CLOCK = 1    /* [YYYY-MM-DD HH:MM:SS.mmm] (device wall clock) */
};

typedef struct {
    const wchar_t* path;
    unsigned long  max_bytes;   /* 0 = CE_LOG_DEFAULT_MAX_BYTES */
    int            stamp;
} ce_log;

/* printf-style; emits one CRLF-terminated ASCII line. Opens the file per line
   and locks it, so it is safe for occasional and cross-process shared logs. For
   a high-frequency single-writer log (a boot phase) use a stream instead. */
void ce_logf(const ce_log* lg, const char* fmt, ...);
void ce_vlogf(const ce_log* lg, const char* fmt, va_list ap);

/* A held-handle logger for a high-frequency single-writer log. It keeps the
   file open, so each line is a seek + write with no per-line open or lock,
   the same cost the pipeline's boot logging needs. The one-writer contract is
   the caller's: a stream must not be shared across threads or processes. */
typedef struct {
    HANDLE        h;
    unsigned long max_bytes;
    int           stamp;
} ce_log_stream;

/* fresh=1 truncates (a log that starts over each boot); fresh=0 appends.
   max_bytes=0 is unbounded (fine for a boot-scoped log). Returns 0 on failure. */
int  ce_log_stream_open(ce_log_stream* s, const wchar_t* path,
                        unsigned long max_bytes, int stamp, int fresh);
void ce_stream_logf(ce_log_stream* s, const char* fmt, ...);
void ce_stream_vlogf(ce_log_stream* s, const char* fmt, va_list ap);
void ce_log_stream_close(ce_log_stream* s);

/* Defines a module's logger: its ce_log plus a printf-style entry point.
   CE_LOGGER_PUBLIC when the name is declared in a header and called from
   another translation unit. */
#define CE_LOGGER_(storage_, fn_, path_)                                  \
    static const ce_log fn_##_ce_log_ = { path_, 0, CE_LOG_STAMP_CLOCK }; \
    storage_ void fn_(const char* fmt, ...) {                             \
        va_list ap;                                                       \
        va_start(ap, fmt);                                                \
        ce_vlogf(&fn_##_ce_log_, fmt, ap);                                \
        va_end(ap);                                                       \
    }

#define CE_LOGGER(fn_, path_)        CE_LOGGER_(static, fn_, path_)
#define CE_LOGGER_PUBLIC(fn_, path_) CE_LOGGER_(, fn_, path_)

#ifdef __cplusplus
}
#endif

#endif
