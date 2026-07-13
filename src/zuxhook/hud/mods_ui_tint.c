#include "mods_ui_tint.h"
#include "mods_xui_handle.h"
#include "mods_log.h"
#include "kerncore.h"

/* ── How the tint works ──────────────────────────────────────────────────────
   XUI renders deferred: a scene-walk (per-element renderer 0x41844e14) records
   draw commands, a replay stage issues them. Every element's draw colour funnels
   through ONE function (XuiSetColorFactor's backend 0x4180fb14) which the
   content core calls (0x4183f414) with the element's colour×opacity, writing it
   to the device colour factor [device+0x10c] that the texture-stage combine
   samples. (Device-confirmed: that funnel is the live colour lever; the per-frame
   per-element overwrite is why a one-shot poke of +0x10c has no effect.)

   So a subtree tint is two detours:
     - 0x41844e14 (per-element render): the BRACKET. When the element being drawn
       is tagged, raise a subtree depth + compose the tint colour; lower on exit.
       Recursion re-enters here for children, so the whole subtree is bracketed.
     - 0x4180fb14 (colour funnel): while inside a bracketed subtree, multiply the
       colour every draw pushes by the active tint. Universal (text, nine-grids,
       figures, video): anything that draws sets a colour factor and is tinted,
       composing with the element's own colour×opacity.

   Identity: the renderer's element node carries its instance at [r1+0x18]; a tint
   is keyed by element handle, resolved to that instance (XuiResolveNode) at tag
   time and matched there in the hot path.

   xuidll lives in CE6's SHARED DLL region, so each .text redirect is GLOBAL the
   moment any process patches it, but zuxhook data is per-process, so the
   call-through trampolines are null in a process that never ran the install. Each
   wrapper self-heals its trampoline on first use (build_tramp), making the shared
   redirects safe in every render process (gemstone, servicesd). The side-table is
   per-process (element handles are process-local), so each process tints its own
   controls; an empty table makes both wrappers a cheap passthrough.

   Threading: XUI renders, and the tag/clear calls (from the icon-host scene-create
   proxy and the state-change drain), all run on the one UI thread, so the table
   and the depth/colour globals need no locking. */

/* Per-element renderer - the subtree bracket. r1 = element node; instance at
   [r1+0x18]. First two A32 instructions, relocated into the call-through
   trampoline (hardcoded so a self-healing process needn't read the patched
   .text). */
#define PER_ELEMENT_RENDER  0x41844e14u
#define RENDER_ORIG0        0xe92d4030u   /* push {r4, r5, lr}  */
#define RENDER_ORIG1        0xe24dd014u   /* sub  sp, sp, #0x14 */
#define ELEM_INSTANCE_OFF   0x18u

/* XuiSetColorFactor backend - the colour funnel. (device, argb). */
#define SET_COLOR_FACTOR    0x4180fb14u
#define SETCF_ORIG0         0xe92d4010u   /* push {r4, lr} */
#define SETCF_ORIG1         0xe1a04000u   /* mov  r4, r0   */

typedef DWORD (*RenderFn)(DWORD r0, DWORD r1, DWORD r2, DWORD r3);
typedef DWORD (*SetCfFn)(DWORD r0, DWORD r1, DWORD r2, DWORD r3);

static RenderFn g_orig_render = 0;   /* call-through to 0x41844e14 (per process) */
static SetCfFn  g_orig_setcf  = 0;   /* call-through to 0x4180fb14 (per process) */
static int      g_patch_done  = 0;

/* Active subtree tint: while g_tint_depth>0 we are drawing inside a tagged
   element, and the funnel multiplies by g_tint_argb. Single UI thread. */
static volatile int   g_tint_depth = 0;
static volatile DWORD g_tint_argb  = 0xFFFFFFFFu;

/* Global tint applied to every draw (0xFFFFFFFF = off); composes with subtree. */
static volatile DWORD g_global_argb = 0xFFFFFFFFu;

#define UI_TINT_MAX 16
static struct { DWORD handle; DWORD node; DWORD argb; } g_tints[UI_TINT_MAX];
static volatile int g_tint_count = 0;   /* non-empty entries; 0 => fast passthrough */

/* Componentwise ARGB multiply, /255 per channel, composes with the element's own
   colour and with ancestor tints (0xFFFFFFFF is identity). */
static DWORD tint_compose(DWORD a, DWORD b) {
    DWORD out = 0;
    int   s;
    for (s = 0; s < 32; s += 8) {
        DWORD ca = (a >> s) & 0xff, cb = (b >> s) & 0xff;
        out |= (((ca * cb) / 255u) & 0xff) << s;
    }
    return out;
}

static int tint_lookup_node(DWORD node, DWORD* argb) {
    int i;
    for (i = 0; i < UI_TINT_MAX; i++)
        if (g_tints[i].node == node) { *argb = g_tints[i].argb; return 1; }
    return 0;
}

/* Build this process's call-through trampoline from the known prologue (no
   kerncore; our own RWX page). The shared .text redirect may already be live, so
   the original instructions are passed in rather than read back.

     [0] orig insn 0   [1] orig insn 1   [2] ldr pc,[pc,#-4] -> [3]   [3] target+8 */
static DWORD build_tramp(DWORD target, DWORD orig0, DWORD orig1) {
    DWORD* t = (DWORD*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    t[0] = orig0;
    t[1] = orig1;
    t[2] = 0xe51ff004u;
    t[3] = target + 8;
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    return (DWORD)t;
}

/* The bracket: tag-match raises the subtree depth and composes the tint; the
   funnel does the colouring. Reentrant (saved state on this frame's stack). */
static DWORD render_wrap(DWORD r0, DWORD r1, DWORD r2, DWORD r3) {
    DWORD argb = 0, inst = 0, saved_argb, ret;
    int   matched = 0;
    if (!g_orig_render) g_orig_render = (RenderFn)build_tramp(PER_ELEMENT_RENDER, RENDER_ORIG0, RENDER_ORIG1);
    if (!g_orig_render) return 0;                 /* alloc failed (≈never) */
    if (g_tint_count == 0)                        /* nothing tinted: passthrough */
        return g_orig_render(r0, r1, r2, r3);

    saved_argb = g_tint_argb;
    if (r1) __try { inst = *(volatile DWORD*)(r1 + ELEM_INSTANCE_OFF); } __except (EXCEPTION_EXECUTE_HANDLER) { inst = 0; }
    if (inst && tint_lookup_node(inst, &argb)) {
        matched = 1;
        g_tint_argb = tint_compose(saved_argb, argb);
        g_tint_depth++;
    }
    ret = g_orig_render(r0, r1, r2, r3);
    if (matched) { g_tint_depth--; g_tint_argb = saved_argb; }
    return ret;
}

/* The funnel: every element pushes its draw colour here. Inside a bracketed
   subtree, fold the tint into it. */
static DWORD setcf_wrap(DWORD r0, DWORD r1, DWORD r2, DWORD r3) {
    DWORD t;
    if (!g_orig_setcf) g_orig_setcf = (SetCfFn)build_tramp(SET_COLOR_FACTOR, SETCF_ORIG0, SETCF_ORIG1);
    if (!g_orig_setcf) return 0;
    t = g_global_argb;                                       /* UI-wide wash (or identity) */
    if (g_tint_depth > 0) t = tint_compose(t, g_tint_argb);  /* compose the bracketed subtree */
    if (t != 0xFFFFFFFFu) r1 = tint_compose(r1, t);
    return g_orig_setcf(r0, r1, r2, r3);
}

/* Redirect a target's entry to `wrapper` (idempotent across processes; relies on
   the consistent shared-DLL VA of `wrapper`). Returns 0 if patched or already
   patched, -1 if kerncore isn't ready yet (retry next call). */
static int patch_entry(DWORD target, void* wrapper) {
    DWORD proc, entry[2], cur = 0;
    __try { cur = *(volatile DWORD*)target; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (cur == 0xe51ff004u) return 0;             /* already redirected */
    if (!kerncore_is_ready() || !kerncore_ensure_helpers()) return -1;
    proc = kerncore_find_proc_struct(GetCurrentProcessId());
    if (proc == 0) return -1;
    entry[0] = 0xe51ff004u;                        /* ldr pc, [pc, #-4] */
    entry[1] = (DWORD)wrapper;
    if (kerncore_patch_code(proc, target, entry, 8) != 0) return -1;
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 8);
    return 0;
}

/* Build both trampolines (no kerncore) and install both redirects once (needs
   kerncore; retried on the next tag if not yet ready). */
static void tint_ensure_installed(void) {
    if (!g_orig_render) g_orig_render = (RenderFn)build_tramp(PER_ELEMENT_RENDER, RENDER_ORIG0, RENDER_ORIG1);
    if (!g_orig_setcf)  g_orig_setcf  = (SetCfFn)build_tramp(SET_COLOR_FACTOR, SETCF_ORIG0, SETCF_ORIG1);
    if (g_patch_done) return;
    if (patch_entry(PER_ELEMENT_RENDER, (void*)&render_wrap) != 0) return;
    if (patch_entry(SET_COLOR_FACTOR,   (void*)&setcf_wrap)  != 0) return;
    g_patch_done = 1;
}

void ModUiTintSet(DWORD handle, DWORD argb) {
    DWORD node;
    int   i, slot = -1;
    if (!handle) return;
    tint_ensure_installed();
    node = XuiResolveNode(handle);
    if (!node) return;                            /* unresolvable -> nothing to match */
    for (i = 0; i < UI_TINT_MAX; i++) {
        if (g_tints[i].handle == handle) { g_tints[i].node = node; g_tints[i].argb = argb; return; }
        if (slot < 0 && g_tints[i].handle == 0) slot = i;
    }
    if (slot >= 0) {
        g_tints[slot].handle = handle;
        g_tints[slot].node   = node;
        g_tints[slot].argb   = argb;
        g_tint_count++;
    } else {
        ModsLogf(L"  ui_tint: table full (%d), dropped tint for handle 0x%08x",
                 UI_TINT_MAX, handle);
    }
}

void ModUiTintClear(DWORD handle) {
    int i;
    if (!handle) return;
    for (i = 0; i < UI_TINT_MAX; i++)
        if (g_tints[i].handle == handle) {
            g_tints[i].handle = 0;
            g_tints[i].node   = 0;
            g_tints[i].argb   = 0;
            if (g_tint_count > 0) g_tint_count--;
            return;
        }
}

void ModUiTintGlobalSet(DWORD argb) {
    tint_ensure_installed();   /* needs the funnel detour live */
    g_global_argb = argb;
}

void ModUiTintGlobalClear(void) {
    g_global_argb = 0xFFFFFFFFu;
}
