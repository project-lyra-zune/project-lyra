#include "mods_icon_host.h"
#include "mods_state_block.h"
#include "mods_curation.h"
#include "mods_icons.h"
#include "mods_ui_tint.h"
#include "mods_scene_suppress.h"
#include "mods_context_list_scene.h"   /* ModContextListBind for the picker overlay */
#include "mods_list_channel.h"         /* ModListChannelSignalScan (re-scan while picker open) */
#include "mods_log.h"
#include "mods_xui_handle.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

typedef HRESULT (*XuiGetDescByIdFn)(void* parent, const wchar_t* id,
                                    void** out, int flags);
typedef int (*SetShowFn)(void* elem, int show);

/* This host runs in gemstone (NowPlaying) AND servicesd (HUD).
   XuiElementGetDescendantById is an xuidll export (XIP-fixed), resolved via
   GetProcAddress. The visibility setter (msg 0x12) is host-internal with no
   export, so its VA differs per host: gemstone 0x58860 / zhud_serv 0x419bd0c4.
   Both resolved in ModsIconHostInstall before the proxy can fire. */
static XuiGetDescByIdFn g_get_desc = NULL;
static SetShowFn        g_set_show = NULL;

/* Exported name-based scene loader. Wraps XuiSceneCreateEx via a direct internal
   branch; it does NOT pass through the hooked IAT, so calling it from our proxy
   can't re-enter us. Runs the full build pipeline (ctor chain + per-property
   apply + visual bind) on the parsed scene. */
typedef HRESULT (*XuiSceneCreateFn)(const wchar_t* base, const wchar_t* path,
                                    void* init, void** out);
#define XUI_SCENE_CREATE  ((XuiSceneCreateFn)0x418358d0)

/* arg2 = full L"gem://<Scene>.xur" URI; arg6 = &out_scene. */
typedef HRESULT (*XuiSceneCreateExFn)(DWORD a1, const wchar_t* uri,
                                      DWORD a3, DWORD a4, DWORD a5, void** out);

typedef HRESULT (*XuiSetPositionFn)(void* elem, void* xy);   /* XuiElementSetPosition */
typedef HRESULT (*XuiAddChildFn)(void* parent, void* child); /* XuiElementAddChild */
typedef HRESULT (*XuiDestroyFn)(DWORD handle);               /* XuiDestroyObject */

static XuiSceneCreateExFn g_orig     = NULL;
static XuiSetPositionFn   g_setpos   = NULL;
static XuiAddChildFn      g_addchild = NULL;
static XuiDestroyFn       g_destroy  = NULL;

/* Generic typed property set: the engine resolves the handle to its property
   block and dispatches to the owning class's registered setter. propIds are
   class-relative; the grid's Columns is propId 0x53. Value descriptor is
   {DWORD type; <data>} (type 5 = string). */
typedef HRESULT (*XuiObjectSetPropertyFn)(DWORD handle, DWORD propId, void* a3, void* a4);
#define XUI_OBJECT_SET_PROPERTY  ((XuiObjectSetPropertyFn)0x41822f90)
#define COLUMNS_PROP_ID  0x53u

typedef struct { DWORD type; const wchar_t* str; } PropValStr;

typedef HRESULT (*XuiHandleFn)(DWORD handle);
typedef HRESULT (*XuiGetParentFn)(DWORD handle, DWORD* outHandle);
#define XUI_GET_PARENT          ((XuiGetParentFn)0x4182f458)

/* Unlinks an element from its parent ([pb+0x18/+0x10/+0x14]=0). Required before
   re-parenting: AddChild rejects a child that still has a parent
   ([child+0x18]!=0). (RE: 0x4182f418 -> 0x41838910.) */
#define XUI_REMOVE_FROM_PARENT  ((XuiHandleFn)0x4182f418)

/* XuiSendMessage(handle, msg); msg = {u16 type 0x14 at +0, u32 id at +4,
   [+0x10]=out-size ptr}. Used to drive the grid's own measure (id 0x3d ->
   scoped LayoutTree -> arrange). */
typedef HRESULT (*XuiSendMessageFn)(DWORD handle, void* msg);
#define XUI_SEND_MESSAGE  ((XuiSendMessageFn)0x41823634)

static DWORD rd32(DWORD addr) {
    DWORD v = 0;
    __try { v = *(volatile DWORD*)addr; } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

static void wr32(DWORD addr, DWORD val) {
    __try { *(volatile DWORD*)addr = val; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Compose a wide string = wide prefix + widened ASCII suffix (e.g. L"gem://" +
   "ModIcon.xur", L"modicon_" + slot). Bounded; always NUL-terminated. */
static void build_w(wchar_t* out, int cap, const wchar_t* prefix, const char* ascii) {
    int i = 0, j;
    for (j = 0; prefix && prefix[j] && i + 1 < cap; j++) out[i++] = prefix[j];
    for (j = 0; ascii && ascii[j] && i + 1 < cap; j++) out[i++] = (wchar_t)(unsigned char)ascii[j];
    out[i] = 0;
}

static DWORD fbits(float f) { union { float f; DWORD d; } u; u.f = f; return u.d; }

#define ICON_HOST_LOG  L"\\flash2\\automation\\mods\\icon-host.log"

static void ilog(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    mods_vflashlog(ICON_HOST_LOG, fmt, ap);
    va_end(ap);
}

/* Set ONLY the Column byte (byte 0) of a child's tail-propblock packed cell word
   [pb+0x12c], preserving its Row/ColumnSpan/RowSpan. The stock icons carry real
   spans (e.g. wifi RowSpan 3 = tail word 0x03010000); a full-word write clobbers
   them and corrupts the grid (faulting the next scene entry). */
static void set_col_tail(DWORD handle, DWORD col) {
    DWORD tail = XuiResolveNode(handle);
    if (tail) {
        DWORD w = rd32(tail + 0x12c);
        wr32(tail + 0x12c, (w & 0xffffff00u) | (col & 0xffu));
    }
}

/* mod-handle -> front-grid-handle map. The column arrange lives on the FRONT
   object (the one XuiGetDescById(scene,"iconGrid") returns), but activation
   re-parents the mod into the parentless base subtree, so the controller (bound
   to the mod) can't always walk up to the front. Inject (which holds the front
   handle) registers it here, keyed by mod handle so concurrent scene instances
   don't alias. `phase` is the once-per-display gate (held here, in the host's own
   data segment, because the engine overwrites property-block scratch). */
#define FRONT_MAP_N 8
static struct { DWORD mod; DWORD front; DWORD phase; char key[MOD_STATE_ID_LEN + 1]; } g_front_map[FRONT_MAP_N];
static DWORD g_front_map_next = 0;

#define ZHUD_CTRL  0x419deeb8u

/* The live AddChild'd quick-settings scene, if one is open. The menu is no
   longer tied to HUD visibility; it persists across HUD auto-hide/show and is
   closed only by the scene's tap-off path or a stale-handle cleanup. */
static DWORD g_open_menu = 0;

/* The context sub-list (long-press picker) overlays the quick-settings menu and is
   tracked separately so it dismisses independently (a selection closes only it). */
static DWORD g_open_context = 0;

/* Teardown of the AddChild'd quick-settings overlay: unlink it from the HUD
   content host and destroy it. Called from the tick after the slide-out
   (TransBack) has had time to play, never from the scene's own message handler. */
static void dismiss_open_menu(void) {
    DWORD m = g_open_menu;
    g_open_menu = 0;
    if (!m) return;
    __try { XUI_REMOVE_FROM_PARENT(m); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_destroy) __try { g_destroy(m); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Key of the currently-open context picker, so the tick can keep asking that
   provider's daemon to re-scan while the list is up. Cleared on close. */
static char  g_open_context_key[MOD_STATE_ID_LEN + 1] = { 0 };
static DWORD g_ctx_scan_at = 0;
static int   g_ctx_rows    = 0;   /* row count the open overlay is currently sized for */
#define CTX_RESCAN_MS 2500   /* re-request discovery this often while the picker is open */

static void layout_list_overlay(DWORD overlay, int n);   /* defined below (with raise_hud_list_overlay) */

static void dismiss_open_context(void) {
    DWORD m = g_open_context;
    g_open_context = 0;
    g_open_context_key[0] = 0;
    g_ctx_rows = 0;
    if (!m) return;
    __try { XUI_REMOVE_FROM_PARENT(m); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_destroy) __try { g_destroy(m); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Tap-off (in the scene's OnMessage) calls ModsHudMenuRequestDismiss, which only
   sets a flag. The tick (on the next UI-loop iteration, off the scene's message
   stack) plays the slide-out then tears the overlay down. The menu is NOT tied to
   HUD visibility; it persists across HUD auto-hide/show until the user dismisses it.

   Slide-out = the scene's native back-exit transition. xuidll exports:
     0x41835aa4 XuiScenePlayBackFromTransition  (scene ctx +0x30)
     0x41835ad8 XuiScenePlayBackToTransition    (scene ctx +0x34)
   The previous code called BackTo, which is the "incoming on back" direction and
   produced the visible second slide-up on dismiss. BackFrom only animates, so the
   tick still destroys after the slide. */
typedef int (*PrimeTransFn)(DWORD scene, DWORD transition, DWORD mode);
#define QS_PRIME_TRANSITION  ((PrimeTransFn)0x4183550cu)
typedef int (*PlayBackFromFn)(DWORD scene);
#define QS_PLAY_BACKFROM     ((PlayBackFromFn)0x41835aa4u)
#define QS_SLIDE_OUT_MS      600u

static int   g_dismiss_requested = 0;
static int   g_dismissing        = 0;
static DWORD g_dismiss_at        = 0;

void ModsHudMenuRequestDismiss(void) { g_dismiss_requested = 1; }

static int   g_ctx_dismiss_requested = 0;
static int   g_ctx_dismissing        = 0;
static DWORD g_ctx_dismiss_at        = 0;

void ModsHudContextRequestDismiss(void) { g_ctx_dismiss_requested = 1; }

void ModsHudMenuTick(void) {
    if (g_dismiss_requested) {
        g_dismiss_requested = 0;
        if (g_open_menu && !g_dismissing) {
            __try {
                QS_PRIME_TRANSITION(g_open_menu, 0xffu, 1u);
                QS_PLAY_BACKFROM(g_open_menu);                  /* TransBackFrom: slide down */
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            g_dismissing = 1;
            g_dismiss_at = GetTickCount() + QS_SLIDE_OUT_MS;
        }
    }
    if (g_dismissing && (int)(GetTickCount() - g_dismiss_at) >= 0) {
        dismiss_open_menu();   /* teardown after the slide */
        g_dismissing = 0;
    }
    if (g_open_menu && XuiResolveObj(g_open_menu) == NULL) { g_open_menu = 0; g_dismissing = 0; }

    /* Context sub-list overlay: same deferred slide-out + teardown as the menu. */
    if (g_ctx_dismiss_requested) {
        g_ctx_dismiss_requested = 0;
        if (g_open_context && !g_ctx_dismissing) {
            __try {
                QS_PRIME_TRANSITION(g_open_context, 0xffu, 1u);
                QS_PLAY_BACKFROM(g_open_context);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            g_ctx_dismissing = 1;
            g_ctx_dismiss_at = GetTickCount() + QS_SLIDE_OUT_MS;
        }
    }
    if (g_ctx_dismissing && (int)(GetTickCount() - g_ctx_dismiss_at) >= 0) {
        dismiss_open_context();
        g_ctx_dismissing = 0;
    }
    if (g_open_context && XuiResolveObj(g_open_context) == NULL) { g_open_context = 0; g_open_context_key[0] = 0; g_ctx_dismissing = 0; }

    /* While a picker is open, keep asking its daemon to re-scan so newly-reachable
       devices appear in the list live (the publish path re-queries in place). The
       daemon's discovery worker coalesces back-to-back requests, so this only
       re-arms an auto-reset event, cheap. */
    if (g_open_context && !g_ctx_dismissing && g_open_context_key[0]
        && (int)(GetTickCount() - g_ctx_scan_at) >= 0) {
        ModListChannelSignalScan(g_open_context_key);
        g_ctx_scan_at = GetTickCount() + CTX_RESCAN_MS;
    }
}

/* Handles of the injected quick-settings "•••" buttons. The scene's tap dispatcher
   (0x419c8b50) matches only its OnInit-slot children, so an injected button is
   dropped unless we recognise its handle in the dispatcher detour. The dispatcher
   passes the scene INSTANCE pointer (not the HXUIOBJ scene handle), so we can't key
   by scene; the tapped element is a plain HXUIOBJ in the same space as our button
   handle, so we match that directly. A flat set (one button per HUD scene; a few
   live across re-creates) suffices. */
#define QS_BTN_N 8
static DWORD g_qs_buttons[QS_BTN_N];
static DWORD g_qs_buttons_next = 0;

static void qs_button_add(DWORD button) {
    DWORD i;
    for (i = 0; i < QS_BTN_N; i++) if (g_qs_buttons[i] == button) return;
    g_qs_buttons[g_qs_buttons_next % QS_BTN_N] = button;
    g_qs_buttons_next++;
}

static int qs_is_button(DWORD handle) {
    DWORD i;
    if (handle == 0) return 0;
    for (i = 0; i < QS_BTN_N; i++) if (g_qs_buttons[i] == handle) return 1;
    return 0;
}


static void front_map_put(DWORD mod, DWORD front, const char* key) {
    DWORD i, idx, k;
    for (i = 0; i < FRONT_MAP_N; i++) {
        if (g_front_map[i].mod == mod) { g_front_map[i].front = front; return; }
    }
    idx = g_front_map_next % FRONT_MAP_N;
    g_front_map[idx].mod   = mod;
    g_front_map[idx].front = front;
    g_front_map[idx].phase = 0;
    for (k = 0; k + 1 < sizeof(g_front_map[idx].key) && key && key[k]; k++)
        g_front_map[idx].key[k] = key[k];
    g_front_map[idx].key[k] = 0;
    g_front_map_next++;
}

/* Defined below; forward-declared for the platform-driven tick. */
void ModsIconRelayoutFront(DWORD front, DWORD mod);

/* Element tints (tint_element). Defined below get_child_either; the state-change
   path calls back into the tint refresh. */
static void tint_apply_front(void* scene, void* grid);
static void tint_on_state_changed(void);

DWORD ModsIconResolveFront(DWORD mod) {
    DWORD i;
    for (i = 0; i < FRONT_MAP_N; i++) if (g_front_map[i].mod == mod) return g_front_map[i].front;
    return 0;
}

/* The ModStateBlock owner/id key this icon reflects, recorded at inject time. Lets
   the one controller class serve any mod's icon instead of a hardcoded slot.
   Returns "" if the element isn't a known mod icon. */
const char* ModsIconKeyForElement(DWORD mod) {
    DWORD i;
    for (i = 0; i < FRONT_MAP_N; i++) if (g_front_map[i].mod == mod) return g_front_map[i].key;
    return "";
}

/* ── Status-icon visibility + once-per-display layout (platform-driven) ──────
   Replaces the per-element ModStatusIcon controller class. The UI-loop tick
   (ModsIconTick, run from mods_state_event.c's MsgWait hook in both gemstone and
   servicesd) resolves each injected icon's front and lays out the grid once it is
   on screen, and reflects each icon's frame visibility from its ModStateBlock
   slot. No XUI class, no class blob, no register_xui_class; the icon fragment is
   a plain control + frame children, driven entirely from here. */

#define ICON_FRAME_MAP_N 8
#define ICON_MAX_FRAMES  8   /* distinct frame visuals per icon (states map onto these) */
typedef struct {
    DWORD ctrl;                    /* control element handle (key) */
    DWORD frame[ICON_MAX_FRAMES];  /* child frame element handles, frame0.. */
    int   nframes;
    int   bound;
    DWORD cached;                  /* last encoded (active,variant); -1 = unknown */
} IconFrames;
static IconFrames g_icon_frames[ICON_FRAME_MAP_N];
static DWORD      g_icon_frames_next = 0;

/* A candidate is the front iff a stock icon resolves under it. Scene families
   differ in case (HUD/NowPlaying: wifiIcon; lockscreen: WifiIcon). */
static int icon_has_status_child(DWORD cand) {
    void* t = NULL;
    if (!g_get_desc) return 0;
    __try { g_get_desc((void*)cand, L"wifiIcon", &t, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { t = NULL; }
    if (t) return 1;
    t = NULL;
    __try { g_get_desc((void*)cand, L"WifiIcon", &t, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { t = NULL; }
    return t != NULL;
}

/* The front is the handle from which a stock icon resolves by name; try the live
   parent first, then the inject-registered map. Returns 0 until the icon is live
   in the real strip (so the tick simply retries next iteration). */
static DWORD find_icon_front(DWORD elem) {
    DWORD cand = 0;
    if (!g_get_desc) return 0;
    __try { XUI_GET_PARENT(elem, &cand); } __except (EXCEPTION_EXECUTE_HANDLER) { cand = 0; }
    if (cand && icon_has_status_child(cand)) return cand;
    cand = ModsIconResolveFront(elem);
    if (cand && icon_has_status_child(cand)) return cand;
    return 0;
}

static IconFrames* icon_frames_for(DWORD ctrl) {
    DWORD i;
    for (i = 0; i < ICON_FRAME_MAP_N; i++)
        if (g_icon_frames[i].ctrl == ctrl) return &g_icon_frames[i];
    return 0;
}

/* Resolve the control's child frame elements (frame0, frame1, …) once. Frames are
   authored hidden; render drives which one is visible. Returns 0 until at least
   one frame resolves (children aren't built during the control's construction). */
static IconFrames* bind_icon_frames(DWORD ctrl) {
    IconFrames* e = icon_frames_for(ctrl);
    int i;
    if (e && e->bound) return e;
    if (!g_get_desc) return 0;
    if (!e) {
        e = &g_icon_frames[g_icon_frames_next % ICON_FRAME_MAP_N];
        g_icon_frames_next++;
        e->ctrl = ctrl; e->nframes = 0; e->bound = 0; e->cached = 0xffffffffu;
        for (i = 0; i < ICON_MAX_FRAMES; i++) e->frame[i] = 0;
    }
    for (i = 0; i < ICON_MAX_FRAMES; i++) {
        wchar_t id[8];   /* "frameN" */
        void* f = 0;
        id[0]=L'f'; id[1]=L'r'; id[2]=L'a'; id[3]=L'm'; id[4]=L'e';
        id[5]=(wchar_t)(L'0'+i); id[6]=0;
        __try { g_get_desc((void*)ctrl, id, &f, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { f = 0; }
        /* A multistate icon omits the frame element for hidden states (e.g.
           "off"), so frame indices can have gaps; don't stop at the first
           missing one. frame[i] stays 0 (never shown); present ones still bind. */
        e->frame[i] = (DWORD)f;
        if (f) e->nframes = i + 1;
    }
    e->bound = (e->nframes > 0);
    return e->bound ? e : 0;
}

/* Reflect the slot's state onto the icon: the state maps (via the icon registry)
   to a frame element to show and a tint to recolour it. Several states can share
   one frame with different tints (recolour one image) or use distinct frames
   (different images). frame index -1 => hide all. Cache-guarded on state. */
static void render_icon(DWORD ctrl, const char* key) {
    IconFrames* e = bind_icon_frames(ctrl);
    int state, i, fidx;
    DWORD enc, tint;
    if (!e || !key) return;
    state = ModStateGetState(key);
    if (state < 0) state = 0;
    enc = 0x100u | (DWORD)(state & 0xff);
    if (enc == e->cached) return;
    e->cached = enc;
    if (!g_set_show) return;
    fidx = ModIconStateFrame(key, state);   /* frame to show; -1 = hidden */
    tint = ModIconStateTint(key, state);
    for (i = 0; i < e->nframes; i++) {
        int show = (i == fidx) ? 1 : 0;
        if (e->frame[i])
            __try { g_set_show((void*)e->frame[i], show); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    /* Recolour the shown frame's subtree (0xFFFFFFFF = identity = leave as-is). */
    if (fidx >= 0 && tint != 0xFFFFFFFFu) ModUiTintSet(ctrl, tint);
    else                                  ModUiTintClear(ctrl);
}

/* Once-per-display layout gate (the class controller calls this on every message
   it receives). Setup runs 0x09..0x29 once, then the engine pumps 0x10 (measure)
   while displayed. Eligible only on the first measure pump after setup-complete
   (built + on screen, before the steady storm) and stays eligible (retry if the
   front isn't resolvable yet) until icon_mark_applied latches phase to done. */
#define MSG_SETUP_DONE    0x29u
#define MSG_MEASURE_PUMP  0x10u
static int icon_advance(DWORD mod, DWORD msgId) {
    DWORD i;
    for (i = 0; i < FRONT_MAP_N; i++) {
        if (g_front_map[i].mod != mod) continue;
        if (g_front_map[i].phase >= 2) return 0;
        if (msgId == MSG_SETUP_DONE) { g_front_map[i].phase = 1; return 0; }
        return (msgId == MSG_MEASURE_PUMP && g_front_map[i].phase == 1) ? 1 : 0;
    }
    return 0;
}
static void icon_mark_applied(DWORD mod) {
    DWORD i;
    for (i = 0; i < FRONT_MAP_N; i++)
        if (g_front_map[i].mod == mod) { g_front_map[i].phase = 2; return; }
}

/* ── ModStatusIcon class - runtime C registration (no blob, no per-mod file) ──
   The element-controller class for status icons, registered once per host from
   ModsIconHostInstall. The engine routes the bound element's messages to
   ModStatusIcon_OnMessage (vtable slot 1), so the once-per-display relayout fires
   IN the element's own measure pass, the in-pass timing that makes the column
   shift authoritative on NowPlaying (a between-frames poll can't). The factory +
   vtable are built here in C, with the host VAs filled at register time (alloc
   0x84ee4 / parent 0x2aca0 / slot2 0x2acec / dtor 0x8678c on gemstone, the
   0x419b…/0x419d… set on servicesd). */
#define MSI_INSTANCE_SIZE  0x24u
#define MSI_CACHED_OFF     0x20u

typedef void* (*MsiAllocFn)(DWORD size);          /* host allocator: r0=size -> ptr */
typedef HRESULT (*XuiRegisterClassFn2)(void* arg0, void* desc);

static MsiAllocFn    g_msi_alloc = 0;
static DWORD         g_msi_vtable[4];              /* {parent_vtable, OnMessage, slot2_stub, ondestroy} */
static DWORD         g_msi_desc[11];               /* descriptor; static, the engine may hold it */
static const wchar_t g_msi_name[] = L"ModStatusIcon";
static int           g_msi_registered = 0;

/* Instance: +0 vtable, +4 bound element (set from the engine's caller ctx),
   +0x20 cached state (-1 = unknown). */
static HRESULT ModStatusIcon_OnMessage(void* self, void* msg) {
    DWORD id = 0, elem;
    if (!self) return 0;
    __try { id = ((DWORD*)msg)[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    elem = rd32((DWORD)self + 4);
    if (!elem) return 0;
    /* once per display: relayout on the first post-setup measure, in-pass, so our
       column layout is the authoritative one for this arrange. */
    if (icon_advance(elem, id)) {
        DWORD front = find_icon_front(elem);
        if (front) { icon_mark_applied(elem); ModsIconRelayoutFront(front, elem); }
    }
    /* reflect the slot onto the frames (cache-guarded; cheap on the measure storm) */
    render_icon(elem, ModsIconKeyForElement(elem));
    return 0;
}

/* The class factory the engine calls to instantiate ModStatusIcon.
   HRESULT(r0=caller ctx, r1=out). Alloc, inline the ctor (vtable + zero +
   cached=-1), bind element = ctx, *out = instance. */
static HRESULT ModStatusIcon_Factory(void* ctx, void** out) {
    void* inst;
    if (out) *out = 0;
    if (!g_msi_alloc) return (HRESULT)0x8007000EL;          /* E_OUTOFMEMORY */
    inst = g_msi_alloc(MSI_INSTANCE_SIZE);
    if (!inst) return (HRESULT)0x8007000EL;
    ZeroMemory(inst, MSI_INSTANCE_SIZE);
    *(DWORD*)inst                              = (DWORD)g_msi_vtable;
    *(DWORD*)((char*)inst + MSI_CACHED_OFF)    = 0xffffffffu;
    *(DWORD*)((char*)inst + 4)                 = (DWORD)ctx;
    if (out) *out = inst;
    return 0;
}

/* Register ModStatusIcon once for this host process (is_sd selects the VA set).
   Called from ModsIconHostInstall, after XuiRegisterClass's registry is primed by
   Phase 2 and after g_get_desc/g_set_show resolve. Idempotent across gemstone
   hot-restarts (the registry persists in the compositor → 0x80300005). */
static void ModStatusIconRegister(int is_sd) {
    HMODULE hxui;
    XuiRegisterClassFn2 fn;
    HRESULT hr = (HRESULT)-1;
    if (g_msi_registered) return;

    g_msi_alloc     = (MsiAllocFn)(is_sd ? 0x419d833cu : 0x00084ee4u);
    g_msi_vtable[0] =              is_sd ? 0x419b9dd0u : 0x0002aca0u;   /* parent_vtable */
    g_msi_vtable[1] = (DWORD)&ModStatusIcon_OnMessage;
    g_msi_vtable[2] =              is_sd ? 0x419b9e1cu : 0x0002acecu;   /* slot2 stub */
    g_msi_vtable[3] =              is_sd ? 0x419cddc4u : 0x0008678cu;   /* ondestroy */

    hxui = GetModuleHandleW(L"xuidll.dll");
    fn = hxui ? (XuiRegisterClassFn2)GetProcAddress(hxui, L"XuiRegisterClass") : 0;
    if (!fn) { ilog(L"  ModStatusIcon: XuiRegisterClass unresolved"); return; }

    ZeroMemory(g_msi_desc, sizeof(g_msi_desc));
    g_msi_desc[1] = (DWORD)g_msi_name;
    g_msi_desc[2] = is_sd ? 0x419b1fb0u : 0x000142d4u;   /* L"XuiControl" parent name */
    g_msi_desc[5] = is_sd ? 0x419b9e24u : 0x0002acf4u;   /* desc FIELD_14 */
    g_msi_desc[6] = (DWORD)&ModStatusIcon_Factory;
    g_msi_desc[7] = is_sd ? 0x419b9978u : 0x0002a7acu;   /* desc FIELD_1C */

    __try { hr = fn((void*)&g_msi_desc[1], (void*)&g_msi_desc[0]); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ilog(L"  ModStatusIcon: register faulted"); return; }
    if (hr == 0 || (unsigned)hr == 0x80300005u) {        /* 0x80300005 = already registered */
        g_msi_registered = 1;
        ilog(L"  ModStatusIcon registered (C, is_sd=%d, hr=0x%08x)", is_sd, (unsigned)hr);
    } else {
        ilog(L"  ModStatusIcon register FAILED hr=0x%08x", (unsigned)hr);
    }
}

/* Same-process immediate refresh on a ModStateBlock change (state-event drain). */
void ModsIconOnStateChanged(void) {
    DWORD i;
    for (i = 0; i < FRONT_MAP_N; i++)
        if (g_front_map[i].mod) render_icon(g_front_map[i].mod, g_front_map[i].key);
    tint_on_state_changed();
    /* An open quick-settings menu re-queries its rows so a status sub-label (e.g.
       Connecting -> Casting) updates live, the native list pattern. */
    if (g_open_menu) ModsQuickSettingsLiveRefresh();
    /* An open context picker re-queries so a daemon's freshly-published option
       list (e.g. newly discovered devices) appears without reopening. Re-size the
       overlay when the row count grew, or the extra rows fall off the screen edge
       (the overlay is bottom-flush sized to its row count). */
    if (g_open_context) {
        int rc;
        ModContextListLiveRefresh();
        rc = ModContextListRowCount();
        if (rc > 0 && rc != g_ctx_rows) { layout_list_overlay(g_open_context, rc); g_ctx_rows = rc; }
    }
}


/* The iconGrid Position timeline keyframes. The scene's authored Position
   timeline (MediaControllerMusic HUD, ScreenLock) re-asserts the authored x on
   every show, clobbering our construction-time re-pin. Mapped on-device (the
   runtime timeline object is built lazily at show, but the parsed keyframe data
   exists at construction): keyframe records are a contiguous 0x28-stride array,
   each carrying an Opacity prop record then a Position vec3 record at +0x14
   ({DWORD type=7; float x; float y=2.0; float z=0.0}). Authored x = 195.0
   (portrait) / 402.0 (landscape). Rewriting these to our shifted x (175 / 382)
   makes the timeline itself drive the grid to our position, with no per-frame heal. */
#define TL_X_195  0x43430000u   /* 195.0f authored portrait x */
#define TL_X_402  0x43c90000u   /* 402.0f authored landscape x */
#define TL_X_175  0x432f0000u   /* 175.0f shifted portrait x  */
#define TL_X_382  0x43bf0000u   /* 382.0f shifted landscape x */
/* ── Timeline keyframe rewrite ──────────────────────────────────────────────
   To drive an element to a runtime-chosen layout we rewrite its authored Position
   keyframes; the show-time timeline then animates to our values with no per-frame
   heal. The two surfaces we touch have genuinely different timeline layouts, so
   each gets a walk matched to its structure (one generic walk regressed the HUD):

   - iconGrid (gemstone NowPlaying/lockscreen + the music HUD): single-subtimeline
     timeline, Position vec3 at keyframe-record+0x18, reached by the deterministic
     subtimeline walk. The HUD is the surface that actually needs it. Its Position
     timeline clobbers the construction-time setpos back to the authored x at show
     time; NowPlaying/lockscreen have no such timeline (the direct setpos sticks).
   - MenuScene (zhud popup): multi-subtimeline timeline, Position vec3 inline at
     record+0, reached through a different tier the iconGrid walk does not hit. It
     is a shallow popup graph, so a bounded graph walk rooted at [tail_pb+0x124]
     (a known object, not a heap scan) reliably finds it; match by the exact
     authored (x,y,z) triple.
   Every write is gated on the authored triple/signature, so a wrong address is a
   no-op. The <Timeline> may be authored on an ancestor container, so both climb the
   parent chain and walk each ancestor's timeline. */
#define TL_AT_PB      0x124u    /* element propblock -> XUITimeline pointer */
#define SUBTL_STRIDE  0x44u
#define KF_HDR_STRIDE 0x1cu
#define KF_HDR_REC    0x18u
#define KF_REC_X      0x18u

static int looks_heap(DWORD p) { return (p >= 0x00010000u && p < 0x40000000u && (p & 3u) == 0u); }

/* iconGrid: `a` = a Position-x address (record+0x18): type 7 at [a-4], x in
   {195,402}, y==2.0, z==0.0. */
static int is_pos_kf(DWORD a) {
    DWORD x = rd32(a);
    return rd32(a - 4) == 7u && (x == TL_X_195 || x == TL_X_402)
        && rd32(a + 4) == 0x40000000u && rd32(a + 8) == 0u;
}

static int rewrite_subtimeline(DWORD S, DWORD gridHandle, DWORD newPort, DWORD newLand) {
    DWORD nkf, hdrb, k; int n = 0;
    if (!looks_heap(S) || rd32(S + 0x18) != gridHandle) return 0;
    nkf  = rd32(S + 0x1c);
    hdrb = rd32(S + 0x20);
    if (!looks_heap(hdrb)) return 0;
    if (nkf == 0 || nkf > 128) nkf = 128;
    for (k = 0; k < nkf; k++) {
        DWORD hdr = hdrb + k * KF_HDR_STRIDE;
        DWORD rec, xa;
        if (!looks_heap(hdr)) break;
        rec = rd32(hdr + KF_HDR_REC);
        if (!looks_heap(rec)) continue;
        xa = rec + KF_REC_X;
        if (is_pos_kf(xa)) {
            wr32(xa, rd32(xa) == TL_X_195 ? newPort : newLand);
            n++;
        }
    }
    return n;
}

/* Shift the iconGrid Position keyframes 195->175 / 402->382 so the show-time
   timeline drives the widened grid to our x instead of clobbering it to authored.
   The <Timeline> is authored on the grid's parent container, so walk the parent
   chain; rewrite whichever subtimeline targets the grid handle. */
static int rewrite_icongrid_x(DWORD gridHandle, int n) {
    DWORD cur = gridHandle; int lvl, rewrote = 0;
    DWORD newPort = fbits(195.0f - 20.0f * (float)n);   /* left edge shifts 20px per mod icon */
    DWORD newLand = fbits(402.0f - 20.0f * (float)n);
    for (lvl = 0; lvl < 8 && cur; lvl++) {
        DWORD tp = XuiResolveNode(cur);
        DWORD T  = tp ? rd32(tp + TL_AT_PB) : 0;
        DWORD parent = 0;
        if (looks_heap(T)) {
            DWORD nsub   = rd32(T + 0x1c);
            DWORD subarr = rd32(T + 0x20);
            rewrote += rewrite_subtimeline(T, gridHandle, newPort, newLand);   /* T itself may be the subtimeline */
            if (looks_heap(subarr) && nsub <= 64) {
                DWORD s;
                for (s = 0; s < nsub; s++)
                    rewrote += rewrite_subtimeline(subarr + s * SUBTL_STRIDE, gridHandle, newPort, newLand);
            }
        }
        __try { XUI_GET_PARENT(cur, &parent); } __except (EXCEPTION_EXECUTE_HANDLER) { parent = 0; }
        if (parent == cur) break;
        cur = parent;
    }
    return rewrote;
}

/* MenuScene Position-rest-keyframe rewrite: deterministic, via the same
   subtimeline structure the iconGrid walk above uses and that the engine's
   animation driver (sub_0x4181d450 / blend sub_0x4181cc30) reads each tick. Earlier
   walks failed by assuming one subtimeline per property; a subtimeline carries ALL
   properties that share a keyframe-time set, and MenuScene authors Position+Opacity
   on the same four times -> ONE subtimeline, TWO property blocks. Named-timeline set
   on the element:
     T   = [tail_pb + 0x124]
     arr = [T + 0x24], count [T + 0x28], stride 0x10; named timeline NT = [entry+4]
   The subtimeline (== NT for this scene) the driver runs:
     kfCount  = [NT + 0x1c]
     timeArr  = [NT + 0x20]  (stride 0x1c); value record V = [timeArr + k*0x1c + 0x18]
     propCount= [NT + 0x34]
   The value record (header+0x18) is the per-property blocks concatenated, each 0x14
   bytes in authored order: block p at V + p*0x14 = {type@+0, x@+4, y@+8, z@+0xc}; a
   vec3 has type 7. MenuScene authors [Position, Opacity] so Position is prop 0
   (rec+0), whereas iconGrid authors [Opacity, Position] so its Position is prop 1
   (rec+0x14, x at +0x18); same layout, different order, which the type-7 search
   handles either way.
   Unlike the iconGrid walk there is no handle gate: match the Position block by
   type==7 + the authored rest signature (x==-1.0, y==320.0) across the prop slots,
   so prop ordering is handled and a wrong address is a no-op. Rewrite y to
   480-measuredH; off-screen keyframes (y==480) don't match and are left as-is so the
   panel still rises from below. Structural iteration only, bounded by the driver's
   own counts, no heap scan. */
#define MENU_REST_X    0xbf800000u   /* -1.0f  authored Position.x      */
#define MENU_REST_Y    0x43a00000u   /* 320.0f authored rest Position.y */
/* Per-element rest sentinels: each slide element (fade/title/touch) authors its
   rest keyframe at a distinct Y so the launcher can retarget each independently
   without cross-rewriting the others' keyframes. */
#define MENU_SENT_FADE   0x43a00000u  /* 320.0f */
#define MENU_SENT_TITLE  0x43a50000u  /* 330.0f */
#define MENU_SENT_TOUCH  0x43aa0000u  /* 340.0f */
#define KF_PROP_STRIDE 0x14u
#define KF_PROP_MAX    4u

static int rewrite_menu_subtl(DWORD S, DWORD matchY, DWORD newRestY) {
    DWORD nkf, hdrb, k; int n = 0;
    if (!looks_heap(S)) return 0;
    nkf  = rd32(S + 0x1c);
    hdrb = rd32(S + 0x20);
    if (!looks_heap(hdrb)) return 0;
    if (nkf == 0 || nkf > 128) nkf = 128;
    for (k = 0; k < nkf; k++) {
        DWORD hdr = hdrb + k * KF_HDR_STRIDE;
        DWORD rec, p;
        if (!looks_heap(hdr)) break;
        rec = rd32(hdr + KF_HDR_REC);
        if (!looks_heap(rec)) continue;
        for (p = 0; p < KF_PROP_MAX; p++) {
            DWORD blk = rec + p * KF_PROP_STRIDE;
            if (rd32(blk) == 7u && rd32(blk + 4) == MENU_REST_X
                && rd32(blk + 8) == matchY) {
                wr32(blk + 8, newRestY);
                n++;
            }
        }
    }
    return n;
}

static int rewrite_menu_resty(DWORD elemHandle, DWORD matchY, DWORD newRestY) {
    DWORD cur = elemHandle; int lvl, rewrote = 0;
    for (lvl = 0; lvl < 8 && cur; lvl++) {
        DWORD tp = XuiResolveNode(cur);
        DWORD T  = tp ? rd32(tp + TL_AT_PB) : 0;
        DWORD parent = 0;
        if (looks_heap(T)) {
            DWORD nsub   = rd32(T + 0x1c);
            DWORD subarr = rd32(T + 0x20);
            rewrote += rewrite_menu_subtl(T, matchY, newRestY);   /* T itself may be the subtimeline */
            if (looks_heap(subarr) && nsub <= 64) {
                DWORD s;
                for (s = 0; s < nsub; s++)
                    rewrote += rewrite_menu_subtl(subarr + s * SUBTL_STRIDE, matchY, newRestY);
            }
        }
        __try { XUI_GET_PARENT(cur, &parent); } __except (EXCEPTION_EXECUTE_HANDLER) { parent = 0; }
        if (parent == cur) break;
        cur = parent;
    }
    return rewrote;
}

/* Resolve a descendant by id, trying two id spellings (GetDescendantById is
   case-sensitive and scene families differ in case). */
static void* get_child_either(void* front, const wchar_t* a, const wchar_t* b) {
    void* e = NULL;
    __try { g_get_desc(front, a, &e, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { e = NULL; }
    if (!e) __try { g_get_desc(front, b, &e, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { e = NULL; }
    return e;
}

/* ── Element tint (tint_element) ──────────────────────────────────────────────
   Recolor an authored scene element + its whole subtree while its state slot is
   active, via the general per-subtree tint primitive (mods_ui_tint): tag the
   element's handle with the tint colour and the render detour multiplies its
   subtree at draw time. Preserves detail (the wifi strength bars stay visible),
   needs no assets, and (applying at draw time) needs no wait for a lazily-built
   visual: tagging the handle is enough.

   Each element handle is transient (a scene rebuild yields a fresh one, device-
   confirmed), so every declared element is re-resolved + re-tagged per scene-
   create and re-evaluated on state change; handles whose scene was destroyed drop
   out (XuiResolveObj fails). */
#define TINT_TRACK_N 8
static struct { DWORD handle; int idx; } g_tint_track[TINT_TRACK_N];
static DWORD g_tint_track_next = 0;

/* Resolve an authored element by id from the scene (iconGrid front first, scene
   fallback). The sentinel "@scene" tags the scene root, tinting the whole scene
   subtree (a UI-wide wash). Stock-icon ids differ only in initial-letter case
   across scene families (wifiIcon / WifiIcon), so the case-flipped variant is
   tried too. */
static void* resolve_tint_elem(void* scene, void* grid, const char* id) {
    wchar_t a[MOD_ICON_TINT_ELEM_LEN + 1], b[MOD_ICON_TINT_ELEM_LEN + 1];
    void*   e = NULL;
    int     i;
    if (!id || !id[0]) return NULL;
    if (id[0] == '@' && id[1] == 's' && id[2] == 'c' && id[3] == 'e'
                     && id[4] == 'n' && id[5] == 'e' && id[6] == 0)
        return scene;   /* the scene root */
    for (i = 0; id[i] && i < MOD_ICON_TINT_ELEM_LEN; i++) a[i] = b[i] = (wchar_t)(unsigned char)id[i];
    a[i] = 0; b[i] = 0;
    if      (b[0] >= L'a' && b[0] <= L'z') b[0] = (wchar_t)(b[0] - 32);
    else if (b[0] >= L'A' && b[0] <= L'Z') b[0] = (wchar_t)(b[0] + 32);
    if (grid)           e = get_child_either(grid,  a, b);
    if (!e && scene)    e = get_child_either(scene, a, b);
    return e;
}

/* Reflect each tracked element's state: tint when its gating state is active,
   clear otherwise; drop handles whose scene has been destroyed. */
static void tint_eval(void) {
    DWORD i;
    for (i = 0; i < TINT_TRACK_N; i++) {
        DWORD h   = g_tint_track[i].handle;
        int   idx = g_tint_track[i].idx, active;
        if (!h) continue;
        if (!XuiResolveObj(h)) { ModUiTintClear(h); g_tint_track[i].handle = 0; continue; }
        if (idx < 0 || idx >= ModIconTintCount() || !ModIconTintKey(idx)) continue;
        active = ModStateGetState(ModIconTintKey(idx));
        if (active < 0) active = 0;
        if (active) ModUiTintSet(h, ModIconTintColor(idx));
        else        ModUiTintClear(h);
    }
}

static void tint_track_put(DWORD handle, int idx) {
    DWORD i, slot;
    for (i = 0; i < TINT_TRACK_N; i++)
        if (g_tint_track[i].handle == handle) { g_tint_track[i].idx = idx; return; }
    slot = g_tint_track_next % TINT_TRACK_N;
    if (g_tint_track[slot].handle) ModUiTintClear(g_tint_track[slot].handle);   /* evict oldest */
    g_tint_track[slot].handle = handle;
    g_tint_track[slot].idx    = idx;
    g_tint_track_next++;
}

/* The sentinel "@all" requests a UI-wide wash (every draw), driven directly
   through the colour funnel rather than per-scene tagging. */
static int tint_is_all(const char* id) {
    return id && id[0] == '@' && id[1] == 'a' && id[2] == 'l' && id[3] == 'l' && id[4] == 0;
}

/* Drive the global wash from every "@all" tint entry's state (set when active,
   clear otherwise). Per-process; runs in each render host. */
static void global_tint_eval(void) {
    int t, n = ModIconTintCount();
    for (t = 0; t < n; t++) {
        int active;
        if (!tint_is_all(ModIconTintElement(t)) || !ModIconTintKey(t)) continue;
        active = ModStateGetState(ModIconTintKey(t));
        if (active > 0) ModUiTintGlobalSet(ModIconTintColor(t));
        else            ModUiTintGlobalClear();
    }
}

/* Resolve every declared element tint reachable from this scene, tag it, and
   reflect current state. */
static void tint_apply_front(void* scene, void* grid) {
    int t, n;
    if (!g_get_desc) return;
    n = ModIconTintCount();
    for (t = 0; t < n; t++) {
        void* e;
        if (!ModIconTintKey(t) || tint_is_all(ModIconTintElement(t))) continue;
        e = resolve_tint_elem(scene, grid, ModIconTintElement(t));
        if (e) tint_track_put((DWORD)e, t);
    }
    tint_eval();
    global_tint_eval();
}

/* On a state change, re-reflect onto all tracked elements + the global wash. */
static void tint_on_state_changed(void) {
    tint_eval();
    global_tint_eval();
}

void ModsIconRelayoutFront(DWORD front, DWORD mod) {
    DWORD modParent = 0;
    void* wifi = NULL, *play = NULL, *batt = NULL;
    DWORD msg[8], measOut[4];
    int   i;


    /* The arrange only places children the front owns; activation can re-parent or
       orphan the mod. Re-adopt it into the front first. */
    __try { XUI_GET_PARENT(mod, &modParent); } __except (EXCEPTION_EXECUTE_HANDLER) { modParent = 0; }
    if (modParent != front) {
        if (modParent) { __try { XUI_REMOVE_FROM_PARENT(mod); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        __try { if (g_addchild) g_addchild((void*)front, (void*)mod); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    /* Each registered mod icon -> column 2*k; stock icons shift right to
       2*N/2*N+2/2*N+4 to make room. Stamp the whole strip on every heal (idempotent)
       so the layout is consistent no matter which icon's controller fired it. Scene
       families differ in case for the STOCK ids: HUD/NowPlaying use
       wifiIcon/playIcon/batteryIcon, the lockscreen uses Wifi/Play/Battery;
       GetDescendantById is case-sensitive, so try both. Mod-icon ids
       (modicon_<token>) are authored exact, no case variant. */
    {
        int n = ModIconsCount(), k;
        for (k = 0; k < n; k++) {
            wchar_t elemId[40];
            void*   e = NULL;
            build_w(elemId, 40, L"modicon_", ModIconGetToken(k));
            __try { g_get_desc((void*)front, elemId, &e, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { e = NULL; }
            if (e) set_col_tail((DWORD)e, (DWORD)(2 * k));
        }
        wifi = get_child_either((void*)front, L"wifiIcon",    L"WifiIcon");
        play = get_child_either((void*)front, L"playIcon",    L"PlayIcon");
        batt = get_child_either((void*)front, L"batteryIcon", L"BatteryIcon");
        set_col_tail((DWORD)wifi, (DWORD)(2 * n));
        set_col_tail((DWORD)play, (DWORD)(2 * n + 2));
        set_col_tail((DWORD)batt, (DWORD)(2 * n + 4));
    }

    /* Arrange via the grid's own msg 0x3d (Measure -> scoped LayoutTree). */
    for (i = 0; i < 8; i++) msg[i] = 0;
    for (i = 0; i < 4; i++) measOut[i] = 0;
    *(volatile WORD*)((void*)msg) = 0x14;
    msg[1] = 0x3du;
    msg[4] = (DWORD)&measOut[0];
    __try { XUI_SEND_MESSAGE(front, msg); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Hold the most recent ModIcon scenes alive so their per-object databuf pools
   aren't recycled while the extracted icon lives in the live grid (a recycled
   slot aliasing a grid ancestor is what made the old re-parent AddChild
   nondeterministic). */
#define ALIVE_SCENES_N 16
static void* g_alive_scenes[ALIVE_SCENES_N];
static DWORD g_alive_next = 0;
static void keep_scene_alive(void* scene) {
    g_alive_scenes[g_alive_next % ALIVE_SCENES_N] = scene;
    g_alive_next++;
}

/* Inject ONE registered mod icon into the grid. Build it the way the engine builds
   native icons: XuiSceneCreate parses the icon fragment and runs the full pipeline
   (ctor chain + per-property apply + visual bind). A bare XuiCreateObject skips the
   per-property apply, leaving the render machinery half-built so the draw pump
   faults. Extract the modicon_<token> element, detach it from the fragment root, then
   adopt it into the live grid as a sibling of wifi/play/battery. The owner/id key
   flows into the front map so the controller (ModsIconKeyForElement) reads the
   right state; the token is only the DOM locator. */
static void inject_one(void* grid, const char* token, const char* key, const char* sceneEntry) {
    void*   scene   = NULL;
    void*   modElem = NULL;
    DWORD   modParent = 0;
    HRESULT screate = (HRESULT)-1;
    wchar_t uri[64], elemId[40];

    if (!token || !key || !sceneEntry) return;
    build_w(uri,    64, L"gem://",   sceneEntry);
    build_w(elemId, 40, L"modicon_", token);

    __try { screate = XUI_SCENE_CREATE(L"", uri, NULL, &scene); }
    __except (EXCEPTION_EXECUTE_HANDLER) { scene = NULL; }
    if (!scene) { ilog(L"  inject: XuiSceneCreate(%s) failed hr=0x%08x", uri, (unsigned)screate); return; }
    keep_scene_alive(scene);

    __try { g_get_desc(scene, elemId, &modElem, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { modElem = NULL; }
    if (!modElem) { ilog(L"  inject: %s not found", elemId); return; }

    /* Detach from the fragment root so AddChild's [child+0x18]==0 gate passes. */
    __try { XUI_GET_PARENT((DWORD)modElem, &modParent); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (modParent) { __try { XUI_REMOVE_FROM_PARENT((DWORD)modElem); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

    /* Register the front + key BEFORE AddChild: AddChild re-entrantly pumps
       construction messages to the controller, which resolves its front + state key. */
    front_map_put((DWORD)modElem, (DWORD)grid, key);
    __try { if (g_addchild) g_addchild(grid, modElem); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { g_set_show(modElem, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ilog(L"  inject ok: token=%S key=%S elem=0x%p grid=0x%p", token, key, modElem, grid);
}

/* Expand + re-pin the grid for N injected mod icons, at CONSTRUCTION (before the
   first display arrange; mutating a live displayed grid's Columns faults). The
   stock iconGrid is authored Columns "16,4,16,4,24" (3 icons), Position [195,2,0],
   width 64. Each mod icon prepends a "16,4" column pair (+20px wide) and shifts the
   left edge 20px left so the right edge stays put and the stock icons keep their
   authored x. The show-time Position timeline is rewritten in step so it drives the
   widened grid to the shifted x instead of clobbering it back to authored. */
static void set_grid_for_n(void* grid, int n) {
    wchar_t cols[80];
    int     c = 0, k;
    float   gpos[3];
    PropValStr p;
    for (k = 0; k < n; k++) {
        cols[c++] = L'1'; cols[c++] = L'6'; cols[c++] = L','; cols[c++] = L'4'; cols[c++] = L',';
    }
    { const wchar_t* stock = L"16,4,16,4,24"; int j; for (j = 0; stock[j]; j++) cols[c++] = stock[j]; }
    cols[c] = 0;
    gpos[0] = 195.0f - 20.0f * (float)n; gpos[1] = 2.0f; gpos[2] = 0.0f;
    p.type = 5; p.str = cols;
    __try { XUI_OBJECT_SET_PROPERTY((DWORD)grid, COLUMNS_PROP_ID, &p, &p); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { if (g_setpos) g_setpos(grid, gpos); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ilog(L"  grid set N=%d cols=%s KFREWRITE=%d", n, cols, rewrite_icongrid_x((DWORD)grid, n));
}

/* Inject every registered mod icon (add_status_icon) into this scene's iconGrid,
   then size the grid for the count. */
static void inject_icons(void* grid) {
    int n = ModIconsCount(), i;
    if (n <= 0) return;
    if (n > MOD_ICON_MAX) n = MOD_ICON_MAX;
    /* Register the ModStatusIcon class lazily here; we are mid scene-create, so
       xuidll's registry + the XuiControl parent are live (registering at
       ModsIconHostInstall was too early: Phase 2 spawns its worker async, so the
       registry wasn't primed yet → XuiRegisterClass 0x80300006). Idempotent +
       retries on a prior failure; must precede inject_one's fragment create,
       which instantiates ClassOverride=ModStatusIcon. */
    ModStatusIconRegister(GetModuleHandleW(L"zhud_serv.dll") != NULL);
    for (i = 0; i < n; i++)
        inject_one(grid, ModIconGetToken(i), ModIconGetKey(i), ModIconGetScene(i));
    set_grid_for_n(grid, n);
}

/* Make the injected HUD button fade in/out with the rest of the HUD. The scene's
   master timeline (driven by the Show/Hide named frames) animates each element's
   Opacity through a per-element subtimeline; an injected element has none, so it
   pops instead of fading. The authored-but-hidden DebugButton already owns a fade
   subtimeline that nothing uses (we never show it), so retarget that subtimeline
   from the DebugButton handle to our button; the scene then fades our button
   identically. Same structure the icon keyframe walk uses: [tail_pb+0x124] ->
   XUITimeline; subtimeline array at +0x20 (count +0x1c, stride 0x44); each
   subtimeline's target handle at +0x18. */
static int retarget_fade_to_button(DWORD sceneHandle, DWORD fromH, DWORD toH) {
    DWORD tp = XuiResolveNode(sceneHandle);
    DWORD T  = tp ? rd32(tp + TL_AT_PB) : 0;
    DWORD nsub, subarr, s;
    int   n = 0;
    if (!looks_heap(T)) return 0;
    nsub   = rd32(T + 0x1c);
    subarr = rd32(T + 0x20);
    if (rd32(T + 0x18) == fromH) { wr32(T + 0x18, toH); n++; }   /* T itself may be a subtimeline */
    if (looks_heap(subarr) && nsub <= 64) {
        for (s = 0; s < nsub; s++) {
            DWORD S = subarr + s * SUBTL_STRIDE;
            if (looks_heap(S) && rd32(S + 0x18) == fromH) { wr32(S + 0x18, toH); n++; }
        }
    }
    return n;
}

/* Inject the quick-settings "•••" button onto the HUD scene root (servicesd). Built
   by the engine via XuiSceneCreate (full ctor + per-property apply + visual bind,
   like inject_icon), extracted, and AddChild'd onto the scene root LAST so it is
   topmost in the bottom-right and wins the hit-test over the full-width
   VolumeDownButton. Positioned to match the NowPlaying rate button. The handle is
   cached so the dispatcher detour can match the tap, and the HUD's DebugButton fade
   subtimeline is retargeted to it so it fades with the HUD. */
static void inject_qs_button(void* hudScene) {
    void*   frag = NULL;
    void*   btn  = NULL;
    DWORD   parent = 0;
    HRESULT screate = (HRESULT)-1;

    __try { screate = XUI_SCENE_CREATE(L"", L"gem://ModQuickSettings.xur", NULL, &frag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { frag = NULL; }
    if (!frag) { ilog(L"  qs: XuiSceneCreate(ModQuickSettings.xur) failed hr=0x%08x", (unsigned)screate); return; }
    keep_scene_alive(frag);

    __try { g_get_desc(frag, L"ModQuickSettings", &btn, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { btn = NULL; }
    if (!btn) { ilog(L"  qs: button element not found"); return; }

    /* Detach from the fragment root so AddChild's [child+0x18]==0 gate passes. */
    __try { XUI_GET_PARENT((DWORD)btn, &parent); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (parent) { __try { XUI_REMOVE_FROM_PARENT((DWORD)btn); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

    __try { if (g_addchild) g_addchild(hudScene, btn); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    { float pos[3] = { 204.0f, 416.0f, 0.0f };   /* aligns to the NowPlaying rate button (measured +4,+8 runtime offset vs authored 208,424) */
      __try { if (g_setpos) g_setpos(btn, pos); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    __try { if (g_set_show) g_set_show(btn, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}

    qs_button_add((DWORD)btn);

    /* Steal the hidden DebugButton's fade subtimeline so our button fades with the HUD. */
    {
        void* dbg = NULL;
        __try { g_get_desc(hudScene, L"DebugButton", &dbg, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { dbg = NULL; }
        if (dbg)
            ilog(L"  qs-button fade retarget rewrote=%d", retarget_fade_to_button((DWORD)hudScene, (DWORD)dbg, (DWORD)btn));
    }
    ilog(L"  qs-button injected: btn=0x%p scene=0x%p", btn, hudScene);
}

/* Overlay a left->right transparent->black gradient over the right edge of one
   metadata label's text. Parented under the LABEL (not the group): a child inherits
   the label's animated opacity and position, so the band fades in/out on HUD
   show/hide exactly as the text does and slides with it on track change. This is
   load-bearing; the per-label opacity subtimeline carries the show/hide fade (the
   group's opacity is constant during show-in, so a band under the group pops to full
   before the text appears). The label's ClipChildren (set by clip_label) also clips
   the band, so its black peak lands on the clip edge and the corner past it is clear. */
static void inject_meta_fade_one(void* hudScene, const wchar_t* parentId) {
    void*   frag   = NULL;
    void*   dst    = NULL;
    void*   fade   = NULL;
    DWORD   parent = 0;
    HRESULT screate = (HRESULT)-1;

    __try { if (g_get_desc(hudScene, parentId, &dst, 0) != 0) dst = NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { dst = NULL; }
    if (!dst) { ilog(L"  meta-fade: parent %s not found", parentId); return; }

    __try { screate = XUI_SCENE_CREATE(L"", L"gem://ModMetaFade.xur", NULL, &frag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { frag = NULL; }
    if (!frag) { ilog(L"  meta-fade: XuiSceneCreate(ModMetaFade.xur) failed hr=0x%08x", (unsigned)screate); return; }
    keep_scene_alive(frag);

    __try { g_get_desc(frag, L"ModMetaFade", &fade, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fade = NULL; }
    if (!fade) { ilog(L"  meta-fade: element not found"); return; }

    __try { XUI_GET_PARENT((DWORD)fade, &parent); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (parent) { __try { XUI_REMOVE_FROM_PARENT((DWORD)fade); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

    __try { if (g_addchild) g_addchild(dst, fade); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    { float pos[3] = { 158.0f, 0.0f, 0.0f };   /* label-local; 92px band, black peak at its midpoint = label-local 204 (the clip edge), right half clipped away by the label's ClipChildren so the corner stays clear */
      __try { if (g_setpos) g_setpos(fade, pos); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    __try { if (g_set_show) g_set_show(fade, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ilog(L"  meta-fade injected under %s: fade=0x%p", parentId, fade);
}

/* Bound a metadata label's width and clip it, so a long song/artist physically
   ends at the gradient band's black peak (~screen 212) instead of running on under
   the opaque mask. This is what makes the fade survive the HUD auto-hide: an opaque
   overlay only hides text at full opacity, so as the HUD fades the mask reveals any
   text behind it (worst at mid-fade). With no overflow behind the black there is
   nothing to reveal; the gradient becomes pure edge-softening. Statically identical
   (the clipped region was already under the gradient's black). Width = propId 1
   (float), ClipChildren = propId 11 (bool, set as int 1); both base XuiElement. */
typedef struct { DWORD type; DWORD val; } PropVal;
#define WIDTH_PROP_ID         0x1u
#define CLIPCHILDREN_PROP_ID  0xbu

static void clip_label(void* hudScene, const wchar_t* id, float width) {
    void*   lbl = NULL;
    PropVal w, c;
    __try { if (g_get_desc(hudScene, id, &lbl, 0) != 0) lbl = NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { lbl = NULL; }
    if (!lbl) { ilog(L"  meta-clip: label %s not found", id); return; }
    w.type = 4; w.val = fbits(width);
    __try { XUI_OBJECT_SET_PROPERTY((DWORD)lbl, WIDTH_PROP_ID, &w, &w); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    c.type = 3; c.val = 1;
    __try { XUI_OBJECT_SET_PROPERTY((DWORD)lbl, CLIPCHILDREN_PROP_ID, &c, &c); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ilog(L"  meta-clip applied to %s (w=%d)", id, (int)width);
}

static void inject_meta_fade(void* hudScene) {
    /* Clip each label first (also clips the band parented under it), then attach a
       band per label so it rides that label's show/hide fade + track-change slide. */
    clip_label(hudScene, L"CurrentSongText",   204.0f);
    clip_label(hudScene, L"CurrentArtistText", 204.0f);
    clip_label(hudScene, L"OtherSongText",     204.0f);
    clip_label(hudScene, L"OtherArtistText",   204.0f);
    inject_meta_fade_one(hudScene, L"CurrentSongText");
    inject_meta_fade_one(hudScene, L"CurrentArtistText");
    inject_meta_fade_one(hudScene, L"OtherSongText");
    inject_meta_fade_one(hudScene, L"OtherArtistText");
}

/* Runs on the gemstone UI thread for every content-scene create. Calls the original
   first (so the scene is built and its real result returned regardless of what
   injection does), then injects into scenes that carry an iconGrid. */
static HRESULT XuiSceneCreateEx_proxy(DWORD a1, const wchar_t* uri,
                                      DWORD a3, DWORD a4, DWORD a5, void** out) {
    HRESULT hr = (HRESULT)0x80004005L;
    void*   scene   = NULL;
    void*   grid    = NULL;
    HRESULT grid_hr = (HRESULT)-1;

    /* Generic scene suppression (mods_scene_suppress.c): a suppress_scene manifest
       action registers a scene URI gated by a setting. Fail the create with a null
       out-scene while that setting is on, so the host's scene navigator takes its
       create-failure path and silently skips the scene; no underlying state is
       touched. Returns 0/no-op for any unregistered or inactive scene. */
    if (ModSceneSuppressActive(uri)) {
        if (out) *out = NULL;
        return (HRESULT)0x80004005L;
    }

    __try { hr = g_orig(a1, uri, a3, a4, a5, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return hr; }

    __try {
        if (out) scene = *out;
        if (scene) grid_hr = g_get_desc(scene, L"iconGrid", &grid, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (grid_hr == 0 && grid) inject_icons(grid);

    /* Recolor stock icons (e.g. WifiIcon orange while "Keep WiFi on" is on). The
       wifi control resolves from the iconGrid front (with the scene as fallback),
       so this fires on every scene family that carries it: NowPlaying, HUD,
       lockscreen. */
    __try { if (scene) tint_apply_front(scene, grid); } __except (EXCEPTION_EXECUTE_HANDLER) {}

    /* Inject the quick-settings "•••" button on the playback HUD (servicesd). The
       DebugButton's presence marks the MediaController HUD scenes. On those, and
       only those, AddChild our button onto the scene root; its tap is delivered via
       the 0x419c8b50 dispatcher detour (mods_phase2.c -> ModsHudMenuButtonTap). */
    __try {
        void* dbg = NULL;
        if (scene && g_get_desc && g_get_desc(scene, L"DebugButton", &dbg, 0) == 0 && dbg) {
            /* The open menu is independent of HUD visibility: it persists
               across HUD re-creates and closes only on intentional dismiss, so
               we do NOT dismiss it here. Re-tapping the button re-opens (which
               dismisses any stale one first, in ModsHudMenuButtonTap). */
            inject_qs_button(scene);
            inject_meta_fade(scene);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return hr;
}

/* Slide the AddChild'd overlay in by running its TransTo, the same pair the
   native scene-push runs for the incoming scene: 0x4183550c primes the entry
   transition, then 0x41835b0c activates the scene and actually runs it (it
   tail-calls the transition builder with mode 0 = TransTo). Priming alone leaves
   the scene inert at the start keyframe (invisible). Both act only on our child
   scene, so the HUD (its parent) is untouched. */
typedef int (*PlaySceneTransitionFn)(DWORD scene, DWORD transition, DWORD mode);
#define PRIME_SCENE_TRANSITION  ((PlaySceneTransitionFn)0x4183550cu)
typedef int (*ActivateSceneFn)(DWORD scene);
#define ACTIVATE_SCENE  ((ActivateSceneFn)0x41835b0cu)

/* Raise gem://<uri> as an overlay child of the live HUD content host, sized for n
   rows and slid up from the bottom (the TransTo the native scene-push runs). Returns
   the scene handle, or 0 on failure. Shared by the quick-settings menu and the
   context sub-list; both are content-host overlays sized by the "8,+,10,+" grid +
   N*64 rows, differing only in URI and row count. */
/* Position a slide element at content-Y (its base) and retarget its TransTo rest
   keyframe to the same Y so the slide animates to it. Each element authors its rest
   keyframe at a distinct sentinel Y (matchY) so rewrites stay element-local. */
static int place_overlay_elem(void* out, const wchar_t* id, DWORD matchY, float y) {
    void* elem = NULL;
    union { float f; DWORD d; } yf; yf.f = y;
    __try { g_get_desc(out, id, &elem, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { elem = NULL; }
    if (!elem) return -1;
    {
        float pos[3]; pos[0] = -1.0f; pos[1] = y; pos[2] = 0.0f;
        if (g_setpos) __try { g_setpos(elem, pos); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return rewrite_menu_resty((DWORD)elem, matchY, yf.d);
}

/* Bottom-flush layout for an N-row list overlay: 8 top spacer + title(22) + 10
   spacer + N*64 rows, less the row's trailing slot pad. Re-runnable, called at
   open and again when a live refresh changes the row count, so the overlay grows
   upward to fit the rows instead of clipping them at the screen edge. */
static void layout_list_overlay(DWORD overlay, int n) {
    int   H, kf, kt, kl;
    float top;
    if (!overlay || n <= 0) return;
    H   = 8 + 22 + 10 + n * 64 - (64 - 46) + 6;
    top = 480.0f - (float)H;
    kf = place_overlay_elem((void*)overlay, L"fade",  MENU_SENT_FADE,  top);
    kt = place_overlay_elem((void*)overlay, L"title", MENU_SENT_TITLE, top + 8.0f);
    kl = place_overlay_elem((void*)overlay, L"touch", MENU_SENT_TOUCH, top + 40.0f);
    ilog(L"  layout overlay n=%d H=%d top=%d kf=%d/%d/%d", n, H, (int)top, kf, kt, kl);
}

static DWORD raise_hud_list_overlay(const wchar_t* uri, int n) {
    DWORD   content_host = rd32(ZHUD_CTRL + 8);
    void*   out  = NULL;
    HRESULT hr   = (HRESULT)-1;
    DWORD   ahr  = (DWORD)-1, slid = (DWORD)-1, act = (DWORD)-1;
    if (!content_host) { ilog(L"  raise: no content host"); return 0; }
    if (n <= 0)        { ilog(L"  raise: nothing to show (n=%d)", n); return 0; }

    __try { hr = XUI_SCENE_CREATE(L"", uri, NULL, &out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = (HRESULT)0xDEAD0001; }
    if (hr != 0 || !out) { ilog(L"  raise: create %s failed hr=0x%08x", uri, (unsigned)hr); return 0; }

    /* Overlay as a CHILD of the live HUD content host (*(ctrl+8)); the HUD stays put
       underneath, unlike NAVIGATE_TO which plays the HUD's leave transition. */
    __try { ahr = g_addchild ? (DWORD)g_addchild((void*)content_host, out) : 0xDEAD0003u; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ahr = 0xDEAD0002u; }

    /* No grid container: fade/title/touch are direct scene children (so the list's
       XuiTouch sits directly under the scene, matching the native lists' gesture
       hierarchy), positioned bottom-flush per row-count. */
    layout_list_overlay((DWORD)out, n);

    __try { slid = (DWORD)PRIME_SCENE_TRANSITION((DWORD)out, 0xffu, 1u); }
    __except (EXCEPTION_EXECUTE_HANDLER) { slid = 0xDEAD0003u; }
    __try { act = (DWORD)ACTIVATE_SCENE((DWORD)out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { act = 0xDEAD0004u; }

    ilog(L"  raise %s: ahr=0x%08x slid=0x%08x act=0x%08x n=%d",
         uri, ahr, slid, act, n);
    return (DWORD)out;
}

/* Launch the quick-toggle menu when the injected "•••" button is tapped. The
   0x419c8b50 dispatcher detour (mods_phase2.c) calls this with the engine's
   hit-tested element handle on every HUD button tap; the scene drops handles not
   in its OnInit slots, so our injected button only reaches an action here. Returns
   1 to resume native dispatch (a stock button, not ours), 0 once we've handled it. */
int ModsHudMenuButtonTap(unsigned int scene, unsigned int tapped) {
    (void)scene;
    if (!qs_is_button((DWORD)tapped)) return 1;

    /* Already open and not sliding out? Ignore - never stack a second menu. If it's
       mid-slide-out, fall through to force-close + reopen so re-tap feels instant. */
    if (g_open_menu && XuiResolveObj(g_open_menu) && !g_dismissing) return 0;
    dismiss_open_menu();   /* drop any stale / mid-dismiss tracker */
    g_dismissing = 0;

    g_open_menu = raise_hud_list_overlay(L"gem://QuickSettingsList.xur",
                                         ModCurationVisibleCount());
    return 0;
}

/* ── Context sub-list provider registry + launch ──────────────────────────────
   A mod registers a ModListSource for a quick-settings row key at boot; a long-press
   on that row opens a ModContextListScene overlay sourced by it. The engine owns the
   overlay lifecycle; the provider owns the rows + on-select. */
#define CTX_PROVIDER_N 8
static struct { char key[MOD_STATE_ID_LEN + 1]; const ModListSource* src; } g_ctx_providers[CTX_PROVIDER_N];
static int g_ctx_provider_n = 0;

static int ctx_key_eq(const char* a, const char* b) {
    int i = 0;
    if (!a || !b) return 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

void ModsHudContextRegister(const char* key, const ModListSource* src) {
    int i;
    if (!key || !src) return;
    for (i = 0; i < g_ctx_provider_n; i++)
        if (ctx_key_eq(g_ctx_providers[i].key, key)) { g_ctx_providers[i].src = src; return; }
    if (g_ctx_provider_n >= CTX_PROVIDER_N) return;
    {
        int n = 0; char* d = g_ctx_providers[g_ctx_provider_n].key;
        for (; n < MOD_STATE_ID_LEN && key[n]; n++) d[n] = key[n];
        d[n] = 0;
    }
    g_ctx_providers[g_ctx_provider_n].src = src;
    g_ctx_provider_n++;
}

int ModsHudContextOpenForKey(const char* key) {
    const ModListSource* src = NULL;
    int i, n;
    if (!key) return 0;
    for (i = 0; i < g_ctx_provider_n; i++)
        if (ctx_key_eq(g_ctx_providers[i].key, key)) { src = g_ctx_providers[i].src; break; }
    if (!src) return 0;

    /* Open-once: a re-fired hold while the picker is already up is a no-op. */
    if (g_open_context && XuiResolveObj(g_open_context) && !g_ctx_dismissing) return 1;
    dismiss_open_context();
    g_ctx_dismissing = 0;

    if (src->on_open) src->on_open(src->ctx);   /* dynamic sources refresh on open (e.g. scan) */
    ModContextListBind(src);   /* snapshot rows before the scene queries them */
    n = src->count ? src->count(src->ctx) : 0;
    g_open_context = raise_hud_list_overlay(L"gem://ContextList.xur", n);
    { int k = 0; for (; k < MOD_STATE_ID_LEN && key[k]; k++) g_open_context_key[k] = key[k]; g_open_context_key[k] = 0; }
    g_ctx_scan_at = GetTickCount() + CTX_RESCAN_MS;   /* on_open did the first scan; keep re-scanning while open */
    g_ctx_rows = n;   /* overlay is sized for n rows now; the tick re-sizes it as the list grows */
    return 1;
}

void ModsIconHostInstall(unsigned int iatSlot) {
    DWORD   slot = iatSlot;
    DWORD   original = 0, verify = 0;
    HMODULE x;

    x = GetModuleHandleW(L"xuidll.dll");
    if (x) {
        g_setpos   = (XuiSetPositionFn)GetProcAddress(x, L"XuiElementSetPosition");
        g_addchild = (XuiAddChildFn)   GetProcAddress(x, L"XuiElementAddChild");
        g_get_desc = (XuiGetDescByIdFn)GetProcAddress(x, L"XuiElementGetDescendantById");
        g_destroy  = (XuiDestroyFn)    GetProcAddress(x, L"XuiDestroyObject");
    }
    /* Host-internal SetShow (msg 0x12) has no export; pick by host. */
    g_set_show = (SetShowFn)(GetModuleHandleW(L"zhud_serv.dll")
                             ? 0x419bd0c4u : 0x00058860u);

    __try {
        original = *(volatile DWORD*)slot;
        *(volatile DWORD*)slot = (DWORD)&XuiSceneCreateEx_proxy;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ilog(L"==== ModsIconHostInstall: IAT patch faulted @0x%08x ====", slot);
        return;
    }
    g_orig = (XuiSceneCreateExFn)original;
    verify = *(volatile DWORD*)slot;
    ilog(L"==== ModsIconHostInstall (pid=%lu): IAT %s setpos=0x%p addchild=0x%p ====",
         GetCurrentProcessId(),
         verify == (DWORD)&XuiSceneCreateEx_proxy ? L"OK" : L"MISMATCH",
         g_setpos, g_addchild);
}
