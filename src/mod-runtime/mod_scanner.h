#ifndef MOD_SCANNER_H
#define MOD_SCANNER_H

/* Mod-manager UI data source. Enumerates every mod directory under
   \flash2\automation\mods\ (not just those listed in enabled.json),
   parses the minimal manifest fields the UI needs, and exposes them
   as wchar_t* strings ready for the raw-wstring assign primitive at
   0x83914.

   The row set is a projection of disk state, rebuilt on Manage-scene
   entry (ModScanRebuild). reposd mutates enabled.json + the mods dirs live
   from the Browse UI. Treat the pointers ModScanGet returns as read-only and
   valid until the next ModScanRebuild. */

struct ModSet;   /* mods_manifest.h - the resolved set ModScanBuild consumes */

#ifdef __cplusplus
extern "C" {
#endif

/* Where a row comes from. Reserved for a future remote source - only
   LOCAL is populated today. */
typedef enum {
    MOD_SOURCE_LOCAL = 0
} ModRowSource;

typedef struct ModRow {
    const wchar_t* id;             /* mod_id, UTF-16, arena-owned          */
    const wchar_t* name;           /* "<name>" - render when enabled       */
    const wchar_t* name_disabled;  /* "<name> (disabled)" - render when disabled
                                       Composed at scan time so toggling
                                       only flips the enabled bit and the
                                       get-item handler just picks. */
    const wchar_t* version;        /* may be NULL                          */
    const wchar_t* author;         /* may be NULL                          */
    const wchar_t* description;    /* may be NULL                          */
    int            enabled;        /* 1 if listed in enabled.json (user intent) */
    int            held_back;      /* enabled by the user but ModsResolve can't
                                       satisfy it this boot (unmet requires /
                                       depends_on / conflict) - inert despite
                                       being enabled. */
    const wchar_t* name_held_back; /* "<name> (held back: <reason>)" - render
                                       when enabled && held_back. */
    int            is_platform;    /* the synthetic Lyra row; update-only, never removable */
    int            experimental;   /* author-declared unfinished (manifest experimental) */
    ModRowSource   source;
} ModRow;

typedef struct ModRowSet {
    const ModRow* rows;
    int           count;
} ModRowSet;

/* Build the held-back table from `resolved` (the resolver's single boot
   result; may be NULL for no annotation) and project the first row set.
   Call once on the Phase 2 worker thread, before the mods tab is
   reachable. The held-back table is boot-persistent; the row set is
   re-projected by ModScanRebuild thereafter. */
void ModScanBuild(const struct ModSet* resolved);

/* Re-project the row set from current disk state (FS walk + enabled_set),
   re-applying the boot-time held-back overlay. Call on Manage-scene entry,
   before any content list reads ModScanGet, so the UI reflects live
   install/uninstall done through the Browse UI without a reboot. */
void ModScanRebuild(void);

/* Returns the current projected row set. Before ModScanBuild runs, reports
   empty via .count == 0 / .rows == NULL. */
const ModRowSet* ModScanGet(void);

/* Flip rows[idx].enabled and persist through enabled_set (Add/Remove the
   one mod_id, so a concurrent Browse change to the file is preserved).
   Returns 1 if the row changed state, 0 if the row is a system-kind mod,
   idx is out of range, or the persist failed (state left unchanged). */
int ModScanToggleEnabled(int idx);

/* Recursively delete \flash2\automation\mods\<id>\ for the row at idx,
   remove it from the enabled set, and drop it from the in-process row
   array. Refuses to delete a system-kind mod. Returns 1 on success, 0 on
   refusal (out-of-range or system), -1 on filesystem failure. After a
   successful call, every row at index > idx shifts down by one. */
int ModScanDelete(int idx);

/* Boot-time cleanup of rename-aside leftovers: delete every *.old file under the
   mods root and remove any mod dir left empty as a result (a removed mod whose
   only remnant was its renamed-aside running binary). Call once at boot before
   mod daemons are spawned, when no daemon holds its binary. Never consults
   enabled.json and never removes a dir that still holds real files, so archived
   (disabled-but-present) mods are untouched. Not an orphan sweep. */
void ModScanSweepOld(void);

/* Boot-time cleanup of platform rename-aside leftovers: delete *.old files at the
   automation root (zuxhook.dll.old, nativeapp.exe.old from a Lyra update). The mods
   sweep only reaches under mods\; these live one level up. Non-recursive, files
   only. Call once at boot alongside ModScanSweepOld, before daemons spawn. */
void ModScanSweepPlatformOld(void);

/* Uninstall: a two-step flow shared by both entry points. The trigger calls
   ModScanUninstallArm() to drop a marker and reboots; at the top of the next boot
   ZUxHookInit calls ModScanUninstall() (gated on ModScanUninstallArmed()), before the
   daemon spawns, so every resident binary is still unstarted and deletes as a plain file.
   It wipes \flash2\automation\ (renaming the mapped zuxhook.dll to zuxhook.dll.old) and the
   stray root logs, and keeps the XNA installer app so a later launch reinstalls. */
int  ModScanUninstallArmed(void);
int  ModScanUninstallArm(void);
void ModScanUninstall(void);

#ifdef __cplusplus
}
#endif

#endif
