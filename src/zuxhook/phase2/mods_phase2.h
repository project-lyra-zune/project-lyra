#ifndef MODS_PHASE2_H
#define MODS_PHASE2_H

/* Phase 2 entrypoint. Called from ZUxHookInit in each XUI host process.
   Spawns a worker thread (so ZUxHookInit can return) that waits for the
   host's xuidll class registry to be primed, then applies each enabled
   mod's Phase-2 actions whose target_proc matches `host` (or "all").

   host = "gemstone" (NowPlaying scenes) or "servicesd" (zhud_serv HUD
   scenes). The two hosts have independent xuidll class/visual registries,
   so a class/visual that must appear in both is registered in each. */

#ifdef __cplusplus
extern "C" {
#endif

/* host: "gemstone" | "servicesd" (NULL → "gemstone"). */
int ModsApplyPhase2(const char* host);

/* Synchronous in-process apply for hosts without an xuidll class registry or
   kernel prerequisites (zie.exe). Runs on the calling thread: applies every
   enabled mod's Phase-2 actions whose target_proc matches `host`, then
   returns. Used so a load_module init installs its hooks before the host
   creates the control they intercept. */
int ModsApplyHostInline(const char* host);

/* 1 if this platform satisfies required point capability `required` (`name` or
   `name@r`), consulting the SUBSYSTEMS[] table here and the wired CAPS table (via
   ModsCapabilityProvidedRange). Both boot phases pass this into ModsResolve as its
   platform-provides predicate. */
int ModsPlatformProvides(const char* required);

/* The compatibility window [min_compat, cur] at which this platform provides `name`.
   Returns 1 and fills the bounds if advertised; 0 if not (unknown, or a
   classified-but-unwired primitive). The install gate compares a mod's used revision
   against this window. */
int ModsPlatformCapabilityRange(const char* name, int* cur, int* min_compat);

#ifdef __cplusplus
}
#endif

#endif
