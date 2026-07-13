#ifndef MODS_UI_TINT_H
#define MODS_UI_TINT_H

#include <windows.h>

/* Arbitrary per-subtree colour tint for the XUI render tree.

   Tags an element by handle; while that element (and its whole subtree) draws, the
   colour every element pushes is multiplied by the tint, so the element renders
   in that colour, preserving its detail and compositing with its own colour and
   opacity. Works for any draw type (image, nine-grid, text, figure, video),
   because it folds the tint into the single colour funnel the renderer routes
   every draw through, bracketed by the per-element render walk. No assets, no
   texture work; it applies at draw time, so there is no wait for a lazily-built
   visual; tagging the handle is enough.

   Handles are transient (they change when a scene is rebuilt), so a consumer
   re-tags the current handle per scene-create and re-evaluates on state change;
   stale handles simply never match a live draw. Multiplicative: 0xFFFFFFFF is a
   no-op. Tints nest (a tagged subtree inside another composes).

   Implementation: two self-healing inline detours on xuidll (the per-element
   renderer and the colour funnel); see mods_ui_tint.c. Install is lazy on first
   use and per render process; an empty tag set is a cheap passthrough. All calls
   happen on the UI thread. */

#ifdef __cplusplus
extern "C" {
#endif

/* Tint `handle`'s element and its subtree by `argb` (0xAARRGGBB, multiplicative).
   Installs the render detours on first call. */
void ModUiTintSet(DWORD handle, DWORD argb);

/* Remove `handle`'s tint (its subtree renders normally again). */
void ModUiTintClear(DWORD handle);

/* Global tint: multiply EVERY draw in this process by `argb` until cleared, a
   UI-wide wash, independent of the per-element tags above and composing with
   them. Covers every scene uniformly (no per-scene tagging). 0xFFFFFFFF = off. */
void ModUiTintGlobalSet(DWORD argb);
void ModUiTintGlobalClear(void);

#ifdef __cplusplus
}
#endif

#endif /* MODS_UI_TINT_H */
