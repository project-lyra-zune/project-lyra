#include "mods_phase2.h"
#include "mods.h"
#include "mods_log.h"
#include "mods_arena.h"
#include "mods_manifest.h"
#include "mods_json.h"
#include "kerncore.h"
#include "mods_xui_registry.h"
#include "mods_icon_host.h"   /* ModsHudMenuButtonTap (HUD menu-button trampoline target) */
#include "mods_wifi_awake.h"  /* WifiAwake_EnsureActive (subsystem) + WifiAwake_Notify */
#include "mods_volume_state.h" /* VolumeStateInstall (subsystem activator) */
#include "mods_settings.h"    /* ModSettingsLoad (restore persisted toggle state) */
#include "mods_resolve.h"     /* ModsResolve + ModsCapabilityDemanded (dependency resolution) */
#include "mods_toggles.h"     /* register_setting declared-settings registry */
#include "mods_icons.h"       /* add_status_icon registry */
#include "mods_state_block.h" /* ModStateSeed (boot default), ModStateReapDeadOwners */
#include "mods_state_event.h" /* ModStateEventPublish (status reaper wakes the UI) */

static void ModStateReaperStart(void);   /* defined below; used in the servicesd Phase 2 init */
#include "mods_scene_suppress.h" /* ModSceneSuppressAdd (suppress_scene cap) */
#include "mods_phase2_internal.h"
#include "mod_scanner.h"      /* ModScanBuild - build the mods-tab row set on this
                                 boot thread (reusing our resolved ModSet) so the
                                 UI-thread data-source pull is a resident memory
                                 read, never a scan. */

#include <windows.h>
#include <string.h>

/* xuidll runtime class registry (base, buckets, RegistryEntry, walk_registry)
   lives in mods_xui_registry.{c,h}, shared with the compositor pass. */

/* Sentinel: gemstone bulk-registers its built-in classes during init;
   GemStartListScene (the start-menu driver) appears in xuidll's
   registry once that phase completes. Use it as our "registry is
   primed" signal. */
static const wchar_t SENTINEL_CLASS[] = L"GemStartListScene";


/* Scratch (gemstone's pre-committed RW regions, verified by virt_query).
   Bump-allocator state is process-wide; resets to base per gemstone
   process. */
#define META_BASE   0x000FF000u
#define META_END    0x0010F000u



/* ── scratch allocator ───────────────────────────────────────────────
   gemstone: the fixed, pre-committed (RWX) heap region 0xFF000..0x10F000
   (its low VA keeps menu/trampoline `bl`s in range). servicesd: that low
   VA is gemstone-private and NOT mapped here, so we VirtualAlloc a fresh
   PAGE_EXECUTE_READWRITE page (CE6 user-mode pages are executable, no
   ARM-side W^X). The blob's factory is position-independent (indirect
   allocator), so the page may sit anywhere. */
static DWORD g_meta_base = META_BASE;
static DWORD g_meta_next = META_BASE;
static DWORD g_meta_end  = META_END;

/* Repoint the scratch allocator at a freshly VirtualAlloc'd RWX region.
   Used in non-gemstone hosts where the fixed low-VA region is absent.
   Allocate once per process; the region lives for zuxhook's lifetime
   (the engine holds class name/factory pointers into it). */
int scratch_use_virtualalloc(int size) {
    DWORD pages = ((DWORD)size + 0xfffu) & ~0xfffu;
    void* p = VirtualAlloc(NULL, pages, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    if (p == NULL) return -1;
    g_meta_base = g_meta_next = (DWORD)p;
    g_meta_end  = (DWORD)p + pages;
    return 0;
}

/* Bump-allocate `size` bytes from scratch, 8-byte aligned. Returns the
   VA, or 0 on exhaustion. Writes are plain memcpy (region is RW[X]). */
DWORD scratch_alloc(int size) {
    DWORD va = (g_meta_next + 7u) & ~7u;
    if (va + (DWORD)size > g_meta_end) return 0;
    g_meta_next = va + size;
    return va;
}

/* SEH-wrapped write to a gemstone VA. Returns 0 on success, -1 on
   access violation. */
int scratch_write(DWORD va, const void* data, int size) {
    int ok = -1;
    __try {
        memcpy((void*)va, data, size);
        ok = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = -1;
    }
    return ok;
}

/* ── code-page patching ──────────────────────────────────────────────── */

/* User-mode VirtualProtect cannot make XIP-shadowed RO code pages
   writable. Code patches route through kerncore_kwriteb in PL1 via
   nativeapp's gadget. See patch_kernel_dword below. */



/* Code patches require PL1 access; user-mode VirtualProtect rejects
   modification of XIP-shadowed RO pages. We use kerncore's primitives
   (which route through nativeapp's kernel-mode gadget) directly:
   kerncore_kwriteb writes one byte at a time in PL1, bypassing the
   L2 PT AP bits entirely. After bytes land, FlushInstructionCache to
   force I-cache re-fetch. */

/* The host process's own proc-struct VA (gemstone or servicesd,
   whichever we're injected into), looked up once at first call. */
static DWORD g_gem_proc = 0;

DWORD get_gem_proc(void) {
    if (g_gem_proc == 0)
        g_gem_proc = kerncore_find_proc_struct(GetCurrentProcessId());
    return g_gem_proc;
}

int patch_kernel_dword(DWORD va, DWORD value, const wchar_t* label) {
    DWORD cur;
    DWORD gem_proc;
    /* Idempotency check via plain in-process read. */
    __try {
        cur = *(volatile DWORD*)va;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ModsLogf(L"    patch %s @0x%08x: in-process read faulted", label, va);
        return -1;
    }
    if (cur == value) {
        ModsLogf(L"    %s @0x%08x already 0x%08x - skip", label, va, value);
        return 0;
    }
    /* Get our own proc-struct VA for the PT-flip. */
    gem_proc = get_gem_proc();
    if (gem_proc == 0) {
        ModsLogf(L"    patch %s: cannot find gemstone proc-struct", label);
        return -1;
    }
    /* PT-flip write: temporarily clear AP[2] on the L2 entry, write, restore. */
    {
        DWORD bytes[1] = { value };
        if (kerncore_patch_code(gem_proc, va, bytes, 4) != 0) {
            ModsLogf(L"    patch %s @0x%08x: kerncore_patch_code failed",
                     label, va);
            return -1;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), (void*)va, 4);
    /* Verify. */
    __try {
        cur = *(volatile DWORD*)va;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ModsLogf(L"    patch %s @0x%08x: verify read faulted", label, va);
        return -1;
    }
    if (cur != value) {
        ModsLogf(L"    patch %s @0x%08x: write didn't stick (got 0x%08x)",
                 label, va, cur);
        return -1;
    }
    ModsLogf(L"    patched %s @0x%08x → 0x%08x", label, va, value);
    return 0;
}


/* ── HUD menu-button dispatch (servicesd / zhud_serv) ─────────────────────────
   Interim + hardcoded, until a manifest capability owns it. The injected
   quick-settings "•••" button (added to the MediaControllerMusic scene root in
   the XuiSceneCreateEx proxy, mods_icon_host.c) is not one of the scene's
   OnInit-slot children, so the MUSIC HUD's per-button command dispatcher
   (0x419c8b50, the 0x18000005 handler), which matches only
   VolUp/VolDown/Play/Pause/Exit, drops its tap.

   We detour the dispatcher's entry: a trampoline calls ModsHudMenuButtonTap first
   with (scene, tapped). On a hit on our injected button it launches the
   quick-toggle menu and returns "handled" (set *out=1, return to the dispatcher's
   caller); otherwise it replays the two stolen prologue instructions and resumes
   native dispatch; every stock button is untouched. */

#define ZHUD_DBG_DISP_VA    0x419c8b50u  /* music HUD button dispatcher entry (push{r4,lr};mov r4,r2) */
#define ZHUD_DBG_RESUME_VA  0x419c8b58u  /* dispatcher body after the 2 stolen prologue instrs */
#define ARM_LDR_PC_NEG4     0xe51ff004u  /* ldr pc,[pc,#-4] - absolute jump via next word */

static int enable_debug_button_launch(void) {
    DWORD proc, tramp_va, cur, patch[2];
    DWORD tramp[17];

    proc = get_gem_proc();   /* process-agnostic: our own proc-struct */
    if (proc == 0) { ModsLogf(L"  debug-enable: no proc-struct"); return -1; }

    /* Idempotency: the dispatcher entry already holds our detour jump. */
    __try { cur = *(volatile DWORD*)ZHUD_DBG_DISP_VA; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ModsLogf(L"  debug-enable: dispatcher read faulted"); return -1;
    }
    if (cur == ARM_LDR_PC_NEG4) { ModsLogf(L"  debug-enable: already patched - skip"); return 0; }

    /* Entry detour for 0x419c8b50(r0=scene, r1=msg-payload, r2=&out, lr=caller).
       Stolen prologue: push {r4,lr}; mov r4,r2. The C call preserves r4-r11; we
       save/restore r0-r3 + lr around it so both the handled-return and the native
       resume see the original argument registers and caller lr. */
    tramp[0]  = 0xe92d400fu; /* push {r0,r1,r2,r3,lr} */
    tramp[1]  = 0xe5911000u; /* ldr  r1,[r1]          r1 = tapped handle = *msg-payload */
    tramp[2]  = 0xe59fc030u; /* ldr  r12,[pc,#0x30]   -> idx16 &ModsHudMenuButtonTap */
    tramp[3]  = 0xe12fff3cu; /* blx  r12              ModsHudMenuButtonTap(scene, tapped) */
    tramp[4]  = 0xe3500000u; /* cmp  r0,#0 */
    tramp[5]  = 0x1a000004u; /* bne  idx11 (not Debug -> resume native) */
    /* handled (DebugButton): restore, mark *out=1, return to dispatcher caller. */
    tramp[6]  = 0xe8bd400fu; /* pop  {r0,r1,r2,r3,lr} */
    tramp[7]  = 0xe3a03001u; /* mov  r3,#1 */
    tramp[8]  = 0xe5823000u; /* str  r3,[r2]          *out = 1 (handled) */
    tramp[9]  = 0xe3a00000u; /* mov  r0,#0 */
    tramp[10] = 0xe12fff1eu; /* bx   lr */
    /* not Debug: restore, replay stolen prologue, resume native dispatch. */
    tramp[11] = 0xe8bd400fu; /* pop  {r0,r1,r2,r3,lr} */
    tramp[12] = 0xe92d4010u; /* push {r4,lr}          (stolen instr 1) */
    tramp[13] = 0xe1a04002u; /* mov  r4,r2            (stolen instr 2) */
    tramp[14] = ARM_LDR_PC_NEG4; /* ldr pc,[pc,#-4]   -> idx15 */
    tramp[15] = ZHUD_DBG_RESUME_VA;
    tramp[16] = (DWORD)&ModsHudMenuButtonTap;

    tramp_va = scratch_alloc(sizeof(tramp));
    if (!tramp_va) { ModsLogf(L"  debug-enable: scratch exhausted (tramp)"); return -1; }
    if (scratch_write(tramp_va, tramp, sizeof(tramp)) < 0) {
        ModsLogf(L"  debug-enable: tramp write faulted @0x%08x", tramp_va); return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), (void*)tramp_va, sizeof(tramp));

    /* 8-byte detour at the dispatcher entry: ldr pc,[pc,#-4]; .word tramp_va. */
    patch[0] = ARM_LDR_PC_NEG4;
    patch[1] = tramp_va;
    if (kerncore_patch_code(proc, ZHUD_DBG_DISP_VA, patch, 8) != 0) {
        ModsLogf(L"  debug-enable: kerncore_patch_code failed @0x%08x", ZHUD_DBG_DISP_VA);
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), (void*)ZHUD_DBG_DISP_VA, 8);
    __try { cur = *(volatile DWORD*)ZHUD_DBG_DISP_VA; }
    __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
    ModsLogf(L"  debug-enable: tramp@0x%08x detour@0x%08x (now 0x%08x) helper@0x%08x",
             tramp_va, ZHUD_DBG_DISP_VA, cur, (DWORD)&ModsHudMenuButtonTap);
    return (cur == ARM_LDR_PC_NEG4) ? 0 : -1;
}


/* ── Platform subsystems ─────────────────────────────────────────────────

   Capabilities backed by zuxhook C, always available. A mod demands one via
   its top-level `requires`/`provides` (dependency metadata resolved by
   ModsResolve, never an action). The demanded subsystems are activated once
   each, in the subsystem's host, by activate_demanded_subsystems below. */

typedef void (*SubsystemActivateFn)(void);
static const struct { const char* name; const char* host; SubsystemActivateFn activate; }
SUBSYSTEMS[] = {
    { "wifi_awake",  "servicesd", WifiAwake_EnsureActive },
    { "volume_state", "servicesd", VolumeStateInstall },
};

/* Single source of truth for "is this a platform-provided capability?" Passed
   into ModsResolve as its platform-provides predicate so the resolver stays
   independent of this table (declared in mods_phase2.h). */
int ModsPlatformProvides(const char* name) {
    int i;
    if (!name) return 0;
    for (i = 0; i < (int)(sizeof(SUBSYSTEMS) / sizeof(SUBSYSTEMS[0])); i++)
        if (strcmp(name, SUBSYSTEMS[i].name) == 0) return 1;
    return 0;
}

/* Activate each platform subsystem demanded (via requires/provides) by an
   enabled mod, once, in its host. Replaces the former require_subsystem
   action; requires/provides are resolver metadata, not dispatched actions. */
static void activate_demanded_subsystems(const ModSet* set, const char* host) {
    int i;
    for (i = 0; i < (int)(sizeof(SUBSYSTEMS) / sizeof(SUBSYSTEMS[0])); i++) {
        if (strcmp(SUBSYSTEMS[i].host, host) != 0) continue;
        if (ModsCapabilityDemanded(set, SUBSYSTEMS[i].name)) {
            SUBSYSTEMS[i].activate();
            ModsLogf(L"  phase2: subsystem %S activated", SUBSYSTEMS[i].name);
        }
    }
}







static int dispatch_phase2_action(ModAction* a, ModsArena* arena) {
    const char* t = a->type;
    int phase = ModsCapabilityPhase(t);
    if (phase == MODS_CAP_PHASE_NONE) {
        ModsLogf(L"    unknown capability: %S", t);
        return -1;
    }
    /* A Phase 1 cap already ran in the compositor - skip it here. */
    if (phase != 2) return 1;
    /* Phase 2 capabilities. XUI / gemstone-side: */
    if (strcmp(t, "register_setting")       == 0) return apply_register_setting(a, arena);
    if (strcmp(t, "register_status")        == 0) return apply_register_status(a, arena);
    if (strcmp(t, "add_status_icon")        == 0) return apply_add_status_icon(a, arena);
    if (strcmp(t, "tint_element")           == 0) return apply_tint_element(a, arena);
    if (strcmp(t, "register_visuals")       == 0) return apply_register_visuals(a, arena);
    if (strcmp(t, "register_xui_class")     == 0) return apply_register_xui_class(a, arena);
    if (strcmp(t, "inject_menu_entry")      == 0) return apply_inject_menu_entry(a, arena);
    if (strcmp(t, "suppress_scene")         == 0) return apply_suppress_scene(a, arena);
    /* Kernel-state caps (kerncore-backed; deferred from Phase 1): */
    if (strcmp(t, "patch_bytes")            == 0) return apply_patch_bytes(a, arena);
    if (strcmp(t, "kcall")                  == 0) return apply_kcall(a, arena);
    if (strcmp(t, "require_kernel_value")   == 0) return apply_require_kernel_value(a, arena);
    if (strcmp(t, "read_kernel_va")         == 0) return apply_read_kernel_va(a, arena);
    if (strcmp(t, "require_back_ref_range") == 0) return apply_require_back_ref_range(a, arena);
    if (strcmp(t, "require_back_ref_equal") == 0) return apply_require_back_ref_equal(a, arena);
    if (strcmp(t, "install_function_hook")  == 0) return apply_install_function_hook(a, arena);
    if (strcmp(t, "load_module")            == 0) return apply_load_module(a, arena);
    /* Classified Phase 2 but no handler wired (e.g. inject_shellcode, a reserved
       Layer 1 primitive). Surface the gap rather than silently skip. */
    ModsLogf(L"    capability %S classified Phase 2 but not yet implemented", t);
    return -1;
}

/* Does this action run in `host`? target_proc "all" runs in every host, an
   absent target_proc means gemstone-only, otherwise it runs in the named host.
   Lets one manifest target gemstone (NowPlaying) and servicesd (HUD) with the
   right per-host class blob, while gemstone-only caps (menu, kernel) never fire
   in servicesd. */
static int action_target_matches(ModAction* a, const char* host,
                                  ModsArena* arena) {
    const char* tp = NULL;
    if (ModActionGetString(a, "target_proc", arena, &tp, NULL, 0) != 0 || tp == NULL)
        return strcmp(host, "gemstone") == 0;
    if (strcmp(tp, "all") == 0) return 1;
    return strcmp(tp, host) == 0;
}

/* Worker thread body. Returns when complete; nothing waits on it. */
static DWORD WINAPI Phase2Worker(LPVOID lpParam) {
    int attempts = 0;
    int total = 0;
    int ready = 0;
    const char* host = (const char*)lpParam;
    int is_sd;
    const wchar_t* sentinel;

    if (host == NULL) host = "gemstone";
    is_sd = (strcmp(host, "servicesd") == 0);
    /* Sentinel = a class the host registers once its xuidll registry is
       primed: GemStartListScene (gemstone) / HudMediaControllerMusicScene
       (servicesd HUD host). */
    sentinel = is_sd ? L"HudMediaControllerMusicScene" : SENTINEL_CLASS;

    ModsLogOpen(is_sd ? L"\\flash2\\automation\\mods\\phase2-servicesd.log"
                      : L"\\flash2\\automation\\mods\\phase2.log");
    ModsLogf(L"== ModsApplyPhase2 start (host=%S) ==", host);

    /* Non-gemstone hosts lack the fixed low-VA scratch region; plant into a
       VirtualAlloc'd RWX page instead (CE6 user pages are executable). */
    if (is_sd && scratch_use_virtualalloc(0x1000) < 0) {
        ModsLogf(L"  phase2: VirtualAlloc RWX scratch failed");
        ModsLogClose();
        return 0;
    }

    /* Wait for xuidll's class registry to be primed (host sentinel).
       Poll every 100ms up to 10s. */
    for (attempts = 0; attempts < 100; attempts++) {
        int rc = walk_registry(sentinel, &total);
        if (rc == 1) { ready = 1; break; }
        /* rc < 0 is an access violation, likely xuidll not loaded yet; keep waiting. */
        Sleep(100);
    }

    if (!ready) {
        ModsLogf(L"  xuidll registry never primed after %d attempts "
                 L"(last_total=%d)", attempts, total);
        ModsLogf(L"== ModsApplyPhase2 aborted ==");
        ModsLogClose();
        return 0;
    }

    /* Walk once more to capture the full registered-class count. */
    walk_registry(NULL, &total);
    ModsLogf(L"  xuidll registry primed after %d attempt(s) "
             L"(%dx 100ms); %d classes registered",
             attempts + 1, attempts, total);

    /* Apply Phase 2 capabilities from each applied mod's manifest
       (all system mods + enabled feature mods), in the loader's order. */
    {
        ModsArena arena;
        ModSet mods;
        int i, j;
        int p2_applied = 0, deferred = 0, failed = 0;

        /* The loader now parses every manifest on disk (to discover
           system mods), not just the enabled set, so the parse footprint
           scales with the whole mods/ directory. 4 MB leaves headroom for
           that plus each applied mod's Phase 2 class/blob allocations. */
        if (ModsArenaInit(&arena, 4 * 1024 * 1024) < 0) {
            ModsLogf(L"  phase2: arena init failed");
            ModsLogClose();
            return 0;
        }
        memset(&mods, 0, sizeof(mods));
        if (ModsManifestLoadAll(&arena, L"\\flash2\\automation\\mods", &mods) < 0) {
            ModsLogf(L"  phase2: manifest load failed");
            ModsArenaFree(&arena);
            ModsLogClose();
            return 0;
        }
        ModsLogf(L"  phase2: %d mod(s) loaded for capability application", mods.count);
        ModsResolve(&mods, &arena, ModsPlatformProvides);

        /* Wait for kerncore (nativeapp's hax + plant_helpers) to be ready
           before applying any kernel-state caps. Poll every 200 ms up to
           10 s. In normal boots nativeapp finishes hax well before Phase 2
           fires, so this almost always passes on the first check; the wait
           is a safety net for boot-order quirks. If kerncore never comes
           up the apply loop still runs but kernel reads will return 0 and
           the manifests' asserts will catch it. Gemstone only; servicesd's
           Phase 2 applies only register_xui_class/register_visuals, which
           use no kernel-state caps. */
        if (!is_sd) {
            int kc_attempts;
            int kc_ready = 0;
            for (kc_attempts = 0; kc_attempts < 50; kc_attempts++) {
                if (kerncore_is_ready() && kerncore_ensure_helpers()) {
                    kc_ready = 1;
                    break;
                }
                Sleep(200);
            }
            ModsLogf(L"  phase2: kerncore %s after %d attempt(s)",
                     kc_ready ? L"ready" : L"NOT ready",
                     kc_attempts + 1);
        }

        /* Activate platform subsystems demanded by the resolved mods, before
           applying their per-mod actions. */
        activate_demanded_subsystems(&mods, host);

        for (i = 0; i < mods.count; i++) {
            Mod* m = &mods.mods[i];
            if (m->disabled) {
                ModsLogf(L"  [%d/%d] %S - skipped (resolver disabled)",
                         i + 1, mods.count, m->mod_id);
                continue;
            }
            /* Load Phase 1 back-refs (from disk; Phase 1 ran in the
               compositor's process, so scope doesn't cross processes). */
            ModsLoadBackRefs(m, &arena);
            ModsLogf(L"  [%d/%d] %S v%S - %d action(s)",
                     i + 1, mods.count, m->mod_id, m->version, m->actions_count);
            for (j = 0; j < m->actions_count; j++) {
                int rc;
                if (!action_target_matches(&m->actions[j], host, &arena)) {
                    deferred++;
                    continue;
                }
                rc = dispatch_phase2_action(&m->actions[j], &arena);
                if (rc == 0)      p2_applied++;
                else if (rc == 1) deferred++;
                else { failed++;
                    ModsLogf(L"    action[%d] %S FAILED", j, m->actions[j].type);
                }
            }
        }
        ModsLogf(L"  phase2: %d applied / %d skipped / %d failed",
                 p2_applied, deferred, failed);

        /* Build the mods-tab row set on this boot thread, before the menu
           entry that makes the tab reachable is planted. We hand it the
           already-resolved `mods` set so the scanner reuses this boot's
           single ModsResolve result for held-back annotation instead of
           re-parsing + re-resolving every manifest. The FS walk for the row
           set still runs (it's the only pass that surfaces archived mods,
           which ModsManifestLoadAll skips). Doing it here means the gemstone
           UI thread's later data-source pull is a pure resident-memory read,
           matching native scenes. Ordering before flush_menu_entries() is
           load-bearing: the tab can't be tapped until its menu entry exists,
           so the UI thread can never reach ModScanGet before this populates
           it, no lock, no double-scan. Gemstone only (the scene classes and
           their data source live there). */
        if (!is_sd) ModScanBuild(&mods);

        if (flush_menu_entries() < 0)
            ModsLogf(L"  flush_menu_entries: failures detected");

        ModsArenaFree(&arena);
    }

    /* Restore persisted setting values over the just-seeded register_setting
       defaults. servicesd owns the toggle slots; ModStateBlock is shared, so
       gemstone's icons see the restored values too. Wake the wifi-awake
       authority afterwards so a persisted "Keep WiFi on" applies its patch set
       at boot rather than waiting for the next periodic sweep. */
    if (is_sd) { ModSettingsLoad(); WifiAwake_Notify(); ModStateReaperStart(); }

    /* Interim: enable the HUD Debug button (servicesd only). The code patch
       needs kerncore (PT-flip gadget); the servicesd apply loop above doesn't
       wait for it (it uses no kernel-state caps), so wait here before patching. */
    if (is_sd) {
        int kc, kc_ready = 0;
        for (kc = 0; kc < 50; kc++) {
            if (kerncore_is_ready() && kerncore_ensure_helpers()) { kc_ready = 1; break; }
            Sleep(200);
        }
        if (kc_ready) {
            if (enable_debug_button_launch() == 0) ModsLogf(L"  debug-enable: OK");
            /* The wifi-awake keepalive patch set is owned by the subsystem's
               authority thread (armed earlier this pass by
               activate_demanded_subsystems when a mod requires/provides
               wifi_awake) and applied/restored on demand, not installed here. */
        } else {
            ModsLogf(L"  debug-enable: kerncore NOT ready after %d attempt(s), skipped", kc);
        }
    }

    ModsLogf(L"== ModsApplyPhase2 done ==");
    ModsLogClose();
    return 0;
}

/* Status reaper (servicesd): resets any status/ slot whose writing daemon has
   died back to state 0, so a crashed daemon (e.g. castd) can't leave a stale
   "casting"/"error" icon lit. Generalises the wifi lease reap to all
   actor-written status. Wakes the UI to re-render on any reset. */
static DWORD WINAPI state_reaper_thread(LPVOID param) {
    (void)param;
    for (;;) {
        Sleep(4000);
        if (ModStateReapDeadOwners() > 0)
            ModStateEventPublish();
    }
    return 0;   /* not reached */
}

static void ModStateReaperStart(void) {
    static volatile LONG started = 0;
    if (InterlockedExchange(&started, 1) == 0) {
        HANDLE h = CreateThread(NULL, 0, state_reaper_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        ModsLogf(L"  status reaper started");
    }
}

int ModsApplyPhase2(const char* host) {
    static volatile LONG started = 0;
    HANDLE h;

    if (host == NULL) host = "gemstone";

    /* Atomic: only the first ZUxHookInit call in this process spawns
       the worker. Subsequent calls (if any) skip silently. The host string
       is a static literal, valid for the worker's lifetime. */
    if (InterlockedExchange(&started, 1) != 0) return 0;

    h = CreateThread(NULL, 0, Phase2Worker, (LPVOID)host, 0, NULL);
    if (h != NULL) CloseHandle(h);
    return 0;
}

/* Synchronous in-process apply for hosts that have no XUI class registry and
   no kernel-state prerequisites (zie.exe). Loads every manifest, resolves,
   and runs each enabled mod's Phase-2 actions whose target_proc matches
   `host` on the calling thread. The deferred Phase2Worker path is wrong for
   such hosts: the IAT/COM hooks a load_module init installs must be in place
   before the host creates its content control, and zie has no xuidll
   sentinel to wait on. */
int ModsApplyHostInline(const char* host) {
    ModsArena arena;
    ModSet mods;
    int i, j, applied = 0, failed = 0;
    if (host == NULL) return -1;

    ModsLogOpen(L"\\flash2\\automation\\mods\\phase2-inline.log");
    ModsLogf(L"== ModsApplyHostInline start (host=%S) ==", host);

    if (ModsArenaInit(&arena, 4 * 1024 * 1024) < 0) {
        ModsLogf(L"  inline: arena init failed");
        ModsLogClose();
        return -1;
    }
    memset(&mods, 0, sizeof(mods));
    if (ModsManifestLoadAll(&arena, L"\\flash2\\automation\\mods", &mods) < 0) {
        ModsLogf(L"  inline: manifest load failed");
        ModsArenaFree(&arena);
        ModsLogClose();
        return -1;
    }
    ModsResolve(&mods, &arena, ModsPlatformProvides);
    ModsLogf(L"  inline: %d mod(s) loaded", mods.count);

    for (i = 0; i < mods.count; i++) {
        Mod* m = &mods.mods[i];
        if (m->disabled) continue;
        for (j = 0; j < m->actions_count; j++) {
            ModAction* a = &m->actions[j];
            const char* tp = NULL;
            int rc;
            /* Explicit host match only. "all" means "all XUI hosts"
               (gemstone+servicesd); zie is a non-XUI content host, so an
               "all" XUI cap must not reach it; require target_proc == host. */
            if (ModActionGetString(a, "target_proc", &arena, &tp, NULL, 0) != 0
                || tp == NULL || strcmp(tp, host) != 0)
                continue;
            rc = dispatch_phase2_action(a, &arena);
            if (rc == 0) applied++;
            else if (rc != 1) {
                failed++;
                ModsLogf(L"    %S action[%d] %S FAILED", m->mod_id, j, a->type);
            }
        }
    }

    ModsLogf(L"  inline: %d applied / %d failed", applied, failed);
    ModsArenaFree(&arena);
    ModsLogClose();
    return failed == 0 ? 0 : -1;
}
