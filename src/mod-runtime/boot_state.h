#ifndef BOOT_STATE_H
#define BOOT_STATE_H

#include <stddef.h>

/* Boot commit protocol. \flash2\automation\boot.state records the level this
   device is pinned to and how many boots at that level began without reaching
   an interactive shell. A level that spends its retry budget demotes, so a mod
   that wedges the shell costs a power-cycle rather than a reflash. The level
   only descends on its own; climbing back is BootStateClear.

   boot_state.c is free of windows.h and builds with the host compiler;
   boot_state_file.c holds the flash I/O. */

typedef enum {
    BOOT_LEVEL_NORMAL = 0,   /* platform components + enabled feature mods */
    BOOT_LEVEL_SAFE   = 1,   /* platform components only; the Mods tab survives */
    BOOT_LEVEL_BARE   = 2    /* nothing applied; stock shell + the uninstall tile */
} BootLevel;

#define BOOT_MOD_ID_LEN       64    /* matches ENABLED_ID_LEN */
#define BOOT_STATE_BUF_BYTES  256

/* The boot ladder only. What was in flight when a boot died is already in the
   rotated boot log ("[i/n] <mod>" per mod, kept as .prev), and a fault that is
   caught belongs to the mod, in its own fault.json (mod_fault.h). Recording the
   suspect here too meant a flash write per mod per phase for something already
   written. */
typedef struct {
    BootLevel level;
    int       failures;                /* uncommitted boots begun at `level` */
    int       shell_started;           /* a shell instance has run this boot */
} BootState;

#ifdef __cplusplus
extern "C" {
#endif

/* The level a boot runs at, given the last boot's level and its uncommitted
   count. BARE is the floor. */
BootLevel BootLevelDemote(BootLevel current, int failures);

const char* BootLevelName(BootLevel level);

/* Anything unreadable yields SAFE: an unknown history must never resolve to a
   full apply. */
void BootStateParse(const char* buf, size_t len, BootState* out);

/* Bytes written excluding the NUL, or -1 when `cap` is too small. */
int BootStateFormat(const BootState* st, char* out, size_t cap);

/* An absent marker is a clean history, not an unreadable one: the first boot
   after an install runs NORMAL. */
void BootStateRead(BootState* out);

/* Once per boot, from the Phase-1 singleton. */
BootLevel BootStateBeginBoot(void);

/* Clears the failure count and holds the level, so a healthy demoted boot
   neither escalates nor returns to a full apply. */
void BootStateCommit(void);

/* This process has finished applying mods, from every exit of the Phase 2
   worker. Until it is called the shell's ticks do not count: Phase 2 waits
   seconds for the xuidll sentinel while the loop is already pumping, so
   without this the boot commits before load_module, install_function_hook and
   patch_bytes have run. */
void BootStateApplyComplete(void);

/* 1 once this process's apply pass has finished. */
int  BootStateApplyIsComplete(void);

/* One UI message-loop iteration. UI thread only. Only the boot's first shell
   instance may commit: gemstone restarts in place when it wedges, and a
   respawn that pumps briefly before dying again would otherwise clear the
   failure count forever. */
void BootStateTick(void);

void BootStateClear(void);

#ifdef __cplusplus
}
#endif

#endif
