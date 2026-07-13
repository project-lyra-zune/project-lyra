/* ce_extras.h: CE 6 / MSVC 2008 force-include shim.
 * CE ships some standard C functions in non-standard headers.
 * Force-include this via /FI in each dep's makefile as needed.
 * ASCII-only: MSVC 2008 CE preprocessor is not UTF-8-safe in comments.
 */
#ifndef _CE_EXTRAS_H
#define _CE_EXTRAS_H

/* bsearch, lfind: CE puts these in search.h, not stdlib.h */
#include <search.h>

/* MSVC 2008 C mode does not recognize the C99 'inline' keyword; map it to
 * the MSVC extension __inline. No-op in C++ mode where inline is a keyword. */
#ifndef __cplusplus
#ifndef inline
#define inline __inline
#endif
#endif

/* MSVC does not support the C99 'restrict' qualifier. Erase it. */
#ifndef restrict
#define restrict
#endif

/* POSIX strdup not provided by CE CRT; _strdup is the MSVC equivalent. */
#ifndef strdup
#define strdup _strdup
#endif

/* C99 names that MSVC 2008 ships under underscore-prefixed equivalents. */
#ifndef snprintf
#define snprintf _snprintf
#endif
#ifndef vsnprintf
#define vsnprintf _vsnprintf
#endif
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

/* CE has no <limits.h> entry for PATH_MAX. Use MAX_PATH from windows.h
 * (260 chars) as the canonical filesystem-path bound. */
#ifndef PATH_MAX
#define PATH_MAX 260
#endif

/* MSVC's <math.h> does not expose M_PI etc. unless _USE_MATH_DEFINES is
 * defined before the include. Force-included here so all NetSurf TUs see
 * the constants regardless of #include order. */
#ifndef M_PI
#define M_PI    3.14159265358979323846
#define M_PI_2  1.57079632679489661923
#define M_PI_4  0.78539816339744830962
#define M_E     2.7182818284590452354
#endif

/* CE / MSVC 2008 <math.h> does not declare some C99 / POSIX helpers.
 * Without a prototype, C4013 implicit declaration synthesises int(int,int):
 * call sites then pass floats as ints and reinterpret the float return as an
 * int, corrupting every result arithmetically. Forcing the prototypes here
 * makes every TU that /FI's this header see the correct ABI. */
#ifdef __cplusplus
extern "C" {
#endif
float powf(float base, float exp);
long  lroundf(float x);
int   isnan(double x);
#ifdef __cplusplus
}
#endif

/* MSVC does not support GCC-style attributes used by some NetSurf deps. */
#ifndef __attribute__
#define __attribute__(x)
#endif

/* CE 6's errno.h defines E* constants but no errno variable; the CRT
 * has no per-thread errno mechanism (callers use GetLastError instead).
 * Provide a single int so POSIX-style code that writes/reads errno links;
 * the variable lives in deps/compat/ce_posix.c. Not thread-safe, but
 * NetSurf core has no thread fan-out that would race on it. */
#ifdef __cplusplus
extern "C" {
#endif
extern int errno;
#ifdef __cplusplus
}
#endif

#endif /* _CE_EXTRAS_H */
