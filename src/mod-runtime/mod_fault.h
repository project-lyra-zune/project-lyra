#ifndef MOD_FAULT_H
#define MOD_FAULT_H

#include <stddef.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <wchar.h>
#endif

/* A capability that faulted while being applied, recorded in the mod's own
   directory beside backrefs.json. The fault belongs to the mod, not to the
   boot, and boot.state cannot hold it: a caught fault lets the boot succeed,
   and that success clears the marker's suspect.

   Scoped to the version that faulted, so an updated mod reads as no fault and
   the installer needs no explicit clear. */

#define MOD_FAULT_CAP_LEN  40    /* longest lyra.* capability plus slack */
#define MOD_FAULT_VER_LEN  32

typedef struct {
    int           phase;                   /* 1 or 2 */
    int           action;                  /* index within the mod */
    unsigned long code;                    /* exception code */
    char          cap[MOD_FAULT_CAP_LEN];
    char          version[MOD_FAULT_VER_LEN];
} ModFault;

#ifdef __cplusplus
extern "C" {
#endif

/* Codec (mod_fault.c), free of windows.h so the host build covers it. */
int  ModFaultFormat(const ModFault* f, char* out, size_t cap);
/* 1 when `buf` holds a fault for `installed_version`, else 0. */
int  ModFaultParse(const char* buf, size_t len, const char* installed_version,
                   ModFault* out);

/* Flash-backed (mod_fault_file.c). */
int  ModFaultRecord(const wchar_t* mod_dir, const char* mod_version,
                    int phase, int action, const char* cap, unsigned long code);

/* 1 when a fault is recorded against `installed_version`, else 0. */
int  ModFaultRead(const wchar_t* mod_dir, const char* installed_version,
                  ModFault* out);

void ModFaultClear(const wchar_t* mod_dir);

#ifdef __cplusplus
}
#endif

#endif
