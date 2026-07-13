#ifndef MODS_RESOLVE_H
#define MODS_RESOLVE_H

#include "mods_manifest.h"
#include "mods_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Predicate: 1 if `name` is a platform-provided capability (a built-in
   subsystem the host backs) rather than one a mod's provides[] supplies. The
   host passes its own implementation into ModsResolve, so the resolver stays
   independent of the host's capability registry. */
typedef int (*ModsPlatformProvidesFn)(const char* name);

/* Cross-mod dependency resolution over a loaded set.

   Builds the capability map (platform subsystems + every enabled mod's
   `provides[]`), then disables any mod whose `requires[]` capability is
   unsatisfied, whose `depends_on[]` mod is absent/disabled, or whose
   `conflicts[]` mod is active, propagated to a fixpoint (disabling a provider
   cascades to its consumers). Sets Mod.disabled; logs each decision. Both
   Phase 1 and Phase 2 dispatch skip disabled mods.

   Deterministic across phases (same manifests in, same decisions out), so
   Phase 1 (boot) and Phase 2 (per gemstone) agree without sharing state.

   After disabling, the surviving mods are topologically reordered in `set`
   so that a mod's providers (mods whose `provides[]` satisfies its
   `requires[]`) and `depends_on[]` targets precede it. Disabled mods are
   moved to the end (dispatch skips them). A dependency cycle is logged and
   left in load order. */
void ModsResolve(ModSet* set, ModsArena* arena,
                 ModsPlatformProvidesFn platform_provides);

/* 1 if any enabled mod in `set` demands capability `cap` via its top-level
   `requires[]` or `provides[]`. Used to activate the platform subsystems the
   resolved mod set needs. */
int ModsCapabilityDemanded(const ModSet* set, const char* cap);

#ifdef __cplusplus
}
#endif

#endif
