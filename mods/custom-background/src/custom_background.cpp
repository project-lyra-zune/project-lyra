/* Custom Background gemstone integration (load_module init CustomBackgroundInstall).
 * Two chained hooks on gemstone.exe, base 0x10000, static VA == live VA:
 *   0x67f94  command executor. Command 0x39 arrives from both the menu row and the
 *            photo-viewer button, and 0x656f4 has no other caller, so it is the only
 *            place the destination can be asked for.
 *   0x70fac  the converter's path compose, to change the filename and nothing else.
 * The picker is the platform's own list overlay, so it does not block: the answer
 * comes back on the channel and a worker resumes the conversion. */

#include <windows.h>
#include "lyra.h"
#include "ce_log.h"

#define GEM_EXECUTOR         0x00067f94u
#define GEM_APPLY_BACKGROUND 0x000656f4u
#define GEM_PATH_COMPOSE     0x00070facu
#define GEM_CURRENT_CTX      0x00097300u   /* the shell object the scene tree hangs off */
#define GEM_SCENE_BY_ID      0x0002ab60u   /* (src, scene_id, args, flags, &out) */
#define GEM_SET_SHOW         0x00058860u   /* gemstone-side; zhud has its own */
#define SCENE_VIDEO_MAIN     0x48u         /* NowPlayingVideoMain */
#define SHELL_PIVOT_SCENE    0x60u         /* [shell+0x60] = the Pivot scene handle */

#define CMD_APPLY_BACKGROUND 0x39u

#define PICK_KEY   "setting/custom-background/target"
#define PICK_LOCK  "lock"
#define PICK_BG    "background"

static const wchar_t LOCK_FILENAME[] = L"userimg.bmp";

/* A URI names fixed content: clearing the path, discarding the element's resources
   and freeing unused textures all return S_OK and still redraw the old pixels, so
   each applied background needs its own generation. The scene authors no path. */
#define BG_STEM      L"lyrabg-"
#define BG_NAME_CAP  32
#define BG_PATH_CAP  64

static unsigned g_gen  = 0;                 /* displayed; owned by the UI thread */
static unsigned g_next = 0;                 /* last handed out; owned by the worker */
static wchar_t  g_pending_name[BG_NAME_CAP]; /* filename the converter is writing */

static wchar_t* append_w(wchar_t* d, const wchar_t* s) {
    while (*s) *d++ = *s++;
    *d = 0;
    return d;
}

static wchar_t* append_u(wchar_t* d, unsigned v) {
    wchar_t tmp[12];
    int n = 0;
    if (!v) tmp[n++] = L'0';
    while (v) { tmp[n++] = (wchar_t)(L'0' + v % 10); v /= 10; }
    while (n) *d++ = tmp[--n];
    *d = 0;
    return d;
}

static void bg_name(wchar_t* out, unsigned gen) {
    out = append_w(out, BG_STEM);
    out = append_u(out, gen);
    append_w(out, L".bmp");
}

static void bg_path(wchar_t* out, unsigned gen) {
    wchar_t name[BG_NAME_CAP];
    bg_name(name, gen);
    out = append_w(out, L"\\Flash\\");
    append_w(out, name);
}

static void bg_uri(wchar_t* out, unsigned gen) {
    wchar_t name[BG_NAME_CAP];
    bg_name(name, gen);
    out = append_w(out, L"file://\\Flash\\");
    append_w(out, name);
}

/* Highest generation already on flash, 0 if none. Survives a reboot without a
   state file: the filenames are the state. */
static unsigned newest_gen(void) {
    WIN32_FIND_DATAW fd;
    HANDLE h;
    unsigned best = 0;
    wchar_t pattern[BG_PATH_CAP];
    wchar_t* p = pattern;
    p = append_w(p, L"\\Flash\\");
    p = append_w(p, BG_STEM);
    append_w(p, L"*.bmp");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const wchar_t* d = fd.cFileName;
        unsigned v = 0;
        int i = 0, any = 0;
        while (BG_STEM[i] && d[i] == BG_STEM[i]) i++;
        if (BG_STEM[i]) continue;
        for (; d[i] >= L'0' && d[i] <= L'9'; i++) { v = v * 10 + (unsigned)(d[i] - L'0'); any = 1; }
        if (any && v > best) best = v;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

CE_LOGGER(L, L"\\flash2\\automation\\custom-background.log")

typedef int   (*ComposeFn)(DWORD dir_id, const wchar_t* name, wchar_t* out, DWORD cap);
typedef DWORD (*ApplyFn)(DWORD item);
typedef DWORD (*ExecFn)(DWORD cmd, DWORD target, DWORD ctx);
typedef HRESULT (*SceneByIdFn)(const wchar_t* src, DWORD id, void* args, DWORD flags, void** out);
typedef int (*SetShowFn)(void* elem, int show);

static ExecFn      g_next_exec  = 0;   /* rest of the executor chain */
static SceneByIdFn g_next_scene = 0;   /* rest of the scene-navigate chain */
static int    g_to_background = 0;  /* sink for the conversion in flight */
static DWORD  g_pending_target = 0; /* picture the user is applying */

/* Hooked over the path compose the converter shares with other callers, so it
   matches on the lock-screen filename as well as our own in-flight flag: the flag
   alone would catch an unrelated compose racing us from the UI thread. */
static ComposeFn g_next_compose = 0;

static int name_is(const wchar_t* a, const wchar_t* b) {
    int i = 0;
    if (!a) return 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

extern "C" int CustomBackground_ComposePath(DWORD dir_id, const wchar_t* name,
                                            wchar_t* out, DWORD cap) {
    if (g_to_background && name_is(name, LOCK_FILENAME))
        name = g_pending_name;
    return g_next_compose ? g_next_compose(dir_id, name, out, cap)
                          : ((ComposeFn)GEM_PATH_COMPOSE)(dir_id, name, out, cap);
}

static void publish_rows(void) {
    lyra_channel_stage_row(PICK_KEY, 0, L"Lock screen", L"", PICK_LOCK);
    lyra_channel_stage_row(PICK_KEY, 1, L"Background",  L"", PICK_BG);
    lyra_channel_commit(PICK_KEY, 2);
}

static int value_is(const char* a, const char* b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

/* The backdrop must not be drawn while a video is on screen: the video is a hardware
   plane composited under the UI, and NowPlayingVideoMain carries no surface of its own,
   so it shows through only where the UI draws nothing. An opaque full-screen image is
   exactly what that scene cannot have beneath it. */
static void* find_background(void) {
    HMODULE x = GetModuleHandleW(L"xuidll.dll");
    typedef HRESULT (*GetDescFn)(void* parent, const wchar_t* id, void** out, int flags);
    GetDescFn get_desc;
    void* elem = NULL;
    DWORD shell, pivot;
    if (!x) return NULL;
    get_desc = (GetDescFn)GetProcAddress(x, L"XuiElementGetDescendantById");
    if (!get_desc) return NULL;
    __try {
        shell = *(volatile DWORD*)GEM_CURRENT_CTX;
        if (!shell) return NULL;
        pivot = *(volatile DWORD*)(shell + SHELL_PIVOT_SCENE);
        if (!pivot) return NULL;
        if (get_desc((void*)pivot, L"lyraBackground", &elem, 0) < 0) return NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return elem;
}

/* Runs on the UI thread: every id-based navigation passes through here. */
extern "C" HRESULT CustomBackground_SceneById(const wchar_t* src, DWORD id, void* args,
                                              DWORD flags, void** out) {
    void* elem = find_background();
    if (elem) {
        __try { ((SetShowFn)GEM_SET_SHOW)(elem, id == SCENE_VIDEO_MAIN ? 0 : 1); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_next_scene ? g_next_scene(src, id, args, flags, out)
                        : ((SceneByIdFn)GEM_SCENE_BY_ID)(src, id, args, flags, out);
}

/* UI thread: point the element at a generation and drop the one it replaced. */
static void show_generation(void* ctx) {
    HMODULE x = GetModuleHandleW(L"xuidll.dll");
    typedef HRESULT (*SetPathFn)(void* elem, const wchar_t* path);
    SetPathFn set_path;
    unsigned gen = (unsigned)(DWORD)ctx;
    void* elem = find_background();
    wchar_t uri[BG_PATH_CAP];

    if (!x || !elem) return;
    set_path = (SetPathFn)GetProcAddress(x, L"XuiImageElementSetImagePath");
    if (!set_path) return;

    bg_uri(uri, gen);
    __try {
        if (set_path(elem, uri) < 0) { L("show gen %u: set path failed", gen); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (g_gen && g_gen != gen) {
        wchar_t old_path[BG_PATH_CAP];
        bg_path(old_path, g_gen);
        DeleteFileW(old_path);      /* nothing points at it now */
    }
    g_gen = gen;
    L("showing generation %u", gen);
}

static void run_apply(DWORD target, int to_background) {
    if (!to_background) {
        ((ApplyFn)GEM_APPLY_BACKGROUND)(target);
        L("applied target=%08x -> lock screen", target);
        return;
    }
    {
        unsigned next = ++g_next;   /* not g_gen + 1: the UI thread updates that
                                       later, so two quick applies would collide */
        bg_name(g_pending_name, next);
        g_to_background = 1;
        ((ApplyFn)GEM_APPLY_BACKGROUND)(target);
        g_to_background = 0;
        L("applied target=%08x -> background generation %u", target, next);
        lyra_ui_post(show_generation, (void*)(DWORD)next);
    }
}

/* The picker answers on the channel, so a waiter resumes the work off the UI
   thread. The converter touches imaging.dll and the filesystem, never XUI. */
static DWORD WINAPI selection_worker(LPVOID unused) {
    HANDLE evt = lyra_state_change_event(L"zune-mod-state-evt-custombg");
    char sel[LYRA_CHANNEL_VALUE_MAX];
    (void)unused;
    if (!evt) { L("no wake event; picker answers will be missed"); return 0; }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        WaitForSingleObject(evt, INFINITE);
        if (!g_pending_target) continue;
        if (!lyra_channel_get_selection(PICK_KEY, sel, sizeof sel)) continue;
        {
            DWORD target = g_pending_target;
            g_pending_target = 0;
            lyra_channel_set_selection(PICK_KEY, "");   /* consume, so a stale pick cannot re-fire */
            run_apply(target, value_is(sel, PICK_BG));
        }
    }
}

extern "C" DWORD CustomBackground_Exec(DWORD command_id, DWORD target, DWORD ctx) {
    if (command_id != CMD_APPLY_BACKGROUND)
        return g_next_exec ? g_next_exec(command_id, target, ctx) : 0;

    g_pending_target = target;
    publish_rows();
    lyra_channel_open(PICK_KEY);
    L("asked for destination, target=%08x", target);
    return 0;   /* S_OK: the work resumes when the user answers */
}

extern "C" __declspec(dllexport) int CustomBackgroundInstall(void) {
    int rc, rc2, rc3;
    HANDLE th;

    rc  = lyra_hook_install(GEM_EXECUTOR, (void*)&CustomBackground_Exec,
                            (void**)&g_next_exec);
    rc2 = lyra_hook_install(GEM_PATH_COMPOSE, (void*)&CustomBackground_ComposePath,
                            (void**)&g_next_compose);
    rc3 = lyra_hook_install(GEM_SCENE_BY_ID, (void*)&CustomBackground_SceneById,
                            (void**)&g_next_scene);
    {
        unsigned gen = newest_gen();
        g_next = gen;
        if (gen) lyra_ui_post(show_generation, (void*)(DWORD)gen);
    }
    th  = CreateThread(NULL, 0, selection_worker, NULL, 0, NULL);
    if (th) CloseHandle(th);
    L("CustomBackgroundInstall: executor rc=%d path rc=%d scene rc=%d worker=%d",
      rc, rc2, rc3, th != NULL);
    return (rc == 0 && rc2 == 0 && rc3 == 0 && th != NULL) ? 0 : -1;
}

extern "C" BOOL WINAPI DllMain(HANDLE h, DWORD r, LPVOID l) { (void)h; (void)r; (void)l; return TRUE; }
