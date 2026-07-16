#ifndef MODS_PHASE2_INTERNAL_H
#define MODS_PHASE2_INTERNAL_H

#include <windows.h>
#include "mods_manifest.h"   /* ModAction */
#include "mods_arena.h"      /* ModsArena */

/* Internal contract shared between mods_phase2.c (core: scratch allocator,
   kernel-patch helpers, dispatch table, worker) and the per-capability-family
   translation units (xui / menu / kernel / metadata) it was split from. Not a
   public header; mods.h exposes only ModsApplyPhase2. */

/* ── shared scratch allocator (state lives in the core TU) ──────────────── */
int   scratch_use_virtualalloc(int size);
DWORD scratch_alloc(int size);
int   scratch_write(DWORD va, const void* data, int size);

/* ── shared kernel-code patch helpers (state lives in the core TU) ──────── */
DWORD get_gem_proc(void);
int   patch_kernel_dword(DWORD va, DWORD value, const wchar_t* label);

/* Menu family: core's Phase-2 worker drains the pending-menu table at end. */
int   flush_menu_entries(void);

/* Settings-row family: same shape as the menu family. Apply records pending
   rows, the worker drains them into extended section arrays + detours at end. */
int   flush_settings_rows(void);

/* ── capability handlers (each defined in its family TU; dispatched from the
      core's dispatch_phase2_action table) ─────────────────────────────────── */
int apply_register_visuals(ModAction* a, ModsArena* arena);
int apply_register_xui_class(ModAction* a, ModsArena* arena);
int apply_inject_menu_entry(ModAction* a, ModsArena* arena);
int apply_inject_settings_row(ModAction* a, ModsArena* arena);
int apply_patch_bytes(ModAction* a, ModsArena* arena);
int apply_kcall(ModAction* a, ModsArena* arena);
int apply_require_kernel_value(ModAction* a, ModsArena* arena);
int apply_read_kernel_va(ModAction* a, ModsArena* arena);
int apply_require_back_ref_range(ModAction* a, ModsArena* arena);
int apply_require_back_ref_equal(ModAction* a, ModsArena* arena);
int apply_load_module(ModAction* a, ModsArena* arena);
int apply_install_function_hook(ModAction* a, ModsArena* arena);
int apply_register_setting(ModAction* a, ModsArena* arena);
int apply_register_status(ModAction* a, ModsArena* arena);
int apply_add_status_icon(ModAction* a, ModsArena* arena);
int apply_tint_element(ModAction* a, ModsArena* arena);
int apply_suppress_scene(ModAction* a, ModsArena* arena);

#endif
