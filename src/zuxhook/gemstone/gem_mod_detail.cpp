/* gem_mod_detail.cpp - the unified per-mod detail scene (ManageModDetail.xur).
 *
 * Keyed by mod_id: a list sets the target (GemModDetailSetTarget*) then navigates
 * here. The scene looks the id up in both ModScan (local disk: enabled/archived
 * state, enable/disable/delete) and the reposd feed (catalog: install/update), and
 * shows the actions the mod's actual state allows. Reached identically from the
 * Browse lists, the installed/archived lists, and the Updates list.
 *
 * Instance layout (breadcrumb at +0x08 so XuiScene base's msg=0xe/sub=1 dispatch
 * finds it; class-private state past +0x28 to survive base writes to +0x0c..+0x18):
 *
 *   +0x00 vtable   +0x04 scene_handle   +0x08 breadcrumb
 *   +0x0c..+0x24 reserved (base writes)
 *   +0x28 title   +0x2c status   +0x30 version   +0x34 author   +0x38 description
 *   +0x3c enable  +0x40 disable  +0x44 delete    +0x48 install  +0x4c update
 *   +0x50 local_idx (ModScan index or -1)   +0x54 info   +0x58 infoTitle
 *   +0x5c changelog   +0x60 uninstall (platform only); class-blob allocates 0x64. */

extern "C" {
#include "mod_scanner.h"
}

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "gem_scene_common.h"
#include "gem_mod_detail.h"
#include "repo_client.h"
#include "repo_version.h"
#include "title_name.h"
#include "device_reboot.h"

typedef HRESULT (*SetLabelTextFn)(void* elem, const wchar_t* text);
#define SET_LABEL_TEXT  ((SetLabelTextFn)0x00038434)
typedef int (*SetShowFn)(void* elem, int show);
#define SET_SHOW  ((SetShowFn)0x00058860)

#define MSG_DETACHED  0x18000007

/* The mod the next detail navigation shows. A feed id is ascii; a ModScan id is
   wide, narrowed here. Single active detail, so a global is sufficient. */
static char g_detail_target_id[REPO_ID_LEN];
/* Which list opened the detail. Browse (catalog) prefers the feed's description;
   Manage (installed) always shows the mod's own manifest description. Set explicitly
   by each entry point, not inferred from the id type. */
static int g_detail_from_browse = 0;

extern "C" void GemModDetailSetTargetA(const char* id, int from_browse) {
    int i = 0;
    g_detail_from_browse = from_browse;
    if (!id) { g_detail_target_id[0] = 0; return; }
    for (; id[i] && i < REPO_ID_LEN - 1; i++) g_detail_target_id[i] = id[i];
    g_detail_target_id[i] = 0;
}

extern "C" void GemModDetailSetTargetW(const wchar_t* id, int from_browse) {
    int i = 0;
    g_detail_from_browse = from_browse;
    if (!id) { g_detail_target_id[0] = 0; return; }
    for (; id[i] && i < REPO_ID_LEN - 1; i++) g_detail_target_id[i] = (char)(unsigned char)id[i];
    g_detail_target_id[i] = 0;
}

struct GemModDetailInstance {
    DWORD vtable;
    DWORD scene_handle;
    DWORD breadcrumb_elem;       /* +0x08 */
    DWORD reserved_0c[7];        /* +0x0c..+0x24 */
    DWORD title_elem;            /* +0x28 */
    DWORD status_elem;           /* +0x2c */
    DWORD version_elem;          /* +0x30 */
    DWORD author_elem;           /* +0x34 */
    DWORD description_elem;      /* +0x38 */
    DWORD enable_button_elem;    /* +0x3c */
    DWORD disable_button_elem;   /* +0x40 */
    DWORD delete_button_elem;    /* +0x44 */
    DWORD install_button_elem;   /* +0x48 */
    DWORD update_button_elem;    /* +0x4c */
    int   local_idx;             /* +0x50 - ModScan index or -1 */
    DWORD info_elem;             /* +0x54 - experimental marker value */
    DWORD info_title_elem;       /* +0x58 - experimental marker "info:" caption */
    DWORD changelog_elem;        /* +0x5c - "what's new" block, shown on update */
    DWORD uninstall_button_elem; /* +0x60 - platform row only, "remove Lyra" */
};

static GemModDetailInstance* g_active_detail = NULL;

static void render_label(void* elem, const wchar_t* text) {
    if (!elem || !text) return;
    __try { SET_LABEL_TEXT(elem, text); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void show_elem(DWORD e, int on) {
    if (e) { __try { SET_SHOW((void*)e, on ? 1 : 0); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
}

/* wide == ascii equality (ids are ASCII). */
static int id_weq_a(const wchar_t* w, const char* a) {
    int i = 0;
    for (; a[i]; i++) if (w[i] != (wchar_t)(unsigned char)a[i]) return 0;
    return w[i] == 0;
}

/* ModScan index of the mod dir with this id, or -1. */
static int find_local(const char* id) {
    const ModRowSet* rs = ModScanGet();
    int i;
    if (!rs || !id[0]) return -1;
    for (i = 0; i < rs->count; i++)
        if (rs->rows[i].id && id_weq_a(rs->rows[i].id, id)) return i;
    return -1;
}

/* Feed row with this id, or NULL. */
static const RepoRow* find_feed(const char* id) {
    RepoBlock* repo = RepoClientBlock();
    int i;
    if (!repo || !id[0]) return NULL;
    for (i = 0; i < repo->count; i++)
        if (strcmp(repo->rows[i].id, id) == 0) return &repo->rows[i];
    return NULL;
}

static const wchar_t* install_status_text(long st) {
    switch (st) {
        case REPO_INSTALL_FETCHING:  return L"downloading";
        case REPO_INSTALL_VERIFYING: return L"verifying";
        case REPO_INSTALL_UNPACKING: return L"unpacking";
        case REPO_INSTALL_ENABLING:  return L"enabling";
        case REPO_INSTALL_DONE:      return L"installed. restart to apply.";
        case REPO_INSTALL_ERROR:     return L"install failed";
        default:                     return L"";
    }
}

static void render_all(GemModDetailInstance* self) {
    const ModRowSet* rs = ModScanGet();
    int li = find_local(g_detail_target_id);
    const RepoRow* feed = find_feed(g_detail_target_id);
    const ModRow* local = (rs && li >= 0 && li < rs->count) ? &rs->rows[li] : NULL;
    /* ModScan (disk + enabled.json) is the single source of truth for installed and
       enabled; the feed is consulted only for catalog membership and the available
       version. reposd's per-row installed flag is deliberately not read here - it is a
       second, feed-fetch-stale copy of the same fact and mixing the two is what showed
       contradictory buttons. */
    int present = (local != NULL);          /* installed on disk (enabled or archived) */
    int enabled = present && local->enabled;
    /* An update needs a newer feed version than the installed manifest version. Use
       the local (on-disk) version so a disabled-but-present mod is still covered. */
    int update_avail = 0;
    if (present && feed && local->version) {
        char iv[REPO_VERSION_LEN]; int k = 0;
        for (; local->version[k] && k < REPO_VERSION_LEN - 1; k++) iv[k] = (char)(unsigned char)local->version[k];
        iv[k] = 0;
        update_avail = version_cmp(feed->version, iv) > 0;
    }

    self->local_idx = li;

    /* Metadata: an installed mod is self-describing, so prefer its local (on-disk)
       copy; the feed's catalog copy is only the preview for a not-yet-installed mod. */
    const wchar_t* title = local ? local->name : (feed ? feed->name : L"unknown mod");
    const wchar_t* author = local ? local->author : (feed ? feed->author : L"-");
    /* Description: Browse shows the feed's (catalog) copy; Manage always shows the
       installed manifest's own copy. */
    const wchar_t* descr = g_detail_from_browse
        ? (feed ? feed->description : (local ? local->description : L""))
        : (local ? local->description : (feed ? feed->description : L""));
    render_label((void*)self->title_elem, title ? title : L"unknown mod");
    render_label((void*)self->author_elem, author ? author : L"-");
    render_label((void*)self->description_elem, descr ? descr : L"");

    /* Version: installed manifest version if present, else the feed's. */
    {
        wchar_t vbuf[REPO_VERSION_LEN + 8];
        const wchar_t* vw = NULL;
        wchar_t widened[REPO_VERSION_LEN];
        if (present && local->version) {
            vw = local->version;
        } else if (feed) {
            int i = 0;
            for (; feed->version[i] && i < REPO_VERSION_LEN - 1; i++)
                widened[i] = (wchar_t)(unsigned char)feed->version[i];
            widened[i] = 0;
            vw = widened;
        }
        if (vw) { _snwprintf(vbuf, sizeof(vbuf) / sizeof(vbuf[0]) - 1, L"v%s", vw); render_label((void*)self->version_elem, vbuf); }
        else render_label((void*)self->version_elem, L"-");
    }

    /* Info row: shown only when experimental. Prefer the installed manifest's flag
       (self-describing), else the feed's, so installed and catalog mods agree. */
    {
        int exp = local ? local->experimental : (feed ? feed->experimental : 0);
        if (exp) render_label((void*)self->info_elem, L"experimental");
        show_elem(self->info_elem, exp);
        show_elem(self->info_title_elem, exp);
    }

    /* The one platform (Lyra) is update-only: always active, never enabled/disabled or
       removed here (its own remove-to-stock is a separate action, wired later). */
    int is_plat = present && local->is_platform;

    /* Status. An available update names the version it offers, e.g.
       "update available (v1.0.8)". update_avail implies present + feed. */
    {
        const wchar_t* s;
        wchar_t ubuf[64];
        if (update_avail && feed) {
            wchar_t vw[REPO_VERSION_LEN]; int i = 0;
            for (; feed->version[i] && i < REPO_VERSION_LEN - 1; i++)
                vw[i] = (wchar_t)(unsigned char)feed->version[i];
            vw[i] = 0;
            _snwprintf(ubuf, sizeof(ubuf) / sizeof(ubuf[0]) - 1, L"update available (v%s)", vw);
            ubuf[sizeof(ubuf) / sizeof(ubuf[0]) - 1] = 0;
            s = ubuf;
        }
        else if (is_plat) s = L"installed";
        else if (present) s = enabled ? L"enabled" : L"disabled";
        else if (feed)    s = L"not installed";
        else              s = L"unknown mod";
        render_label((void*)self->status_elem, s);
    }

    /* What's new: the available version's release notes, shown only when an update is
       available (the update is defined by the newer feed version, so read the feed). */
    if (update_avail && feed && feed->changelog[0]) {
        wchar_t buf[REPO_CHANGELOG_LEN + 16];
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]) - 1, L"What's new\n%s", feed->changelog);
        buf[sizeof(buf) / sizeof(buf[0]) - 1] = 0;
        render_label((void*)self->changelog_elem, buf);
        show_elem(self->changelog_elem, 1);
    } else {
        show_elem(self->changelog_elem, 0);
    }

    /* Actions the state allows. Install shows only when the mod is not on disk; a
       present-but-disabled mod offers enable, not install. The platform shows update
       only. */
    show_elem(self->install_button_elem, (!is_plat && feed && !present) ? 1 : 0);
    show_elem(self->update_button_elem,  update_avail ? 1 : 0);
    show_elem(self->enable_button_elem,  (!is_plat && present && !enabled) ? 1 : 0);
    show_elem(self->disable_button_elem, (!is_plat && present && enabled) ? 1 : 0);
    show_elem(self->delete_button_elem,  (!is_plat && present) ? 1 : 0);
    /* The platform is removed via its own uninstall (wipe + reboot), never the
       per-mod delete. Offer it only on the platform row. */
    show_elem(self->uninstall_button_elem, is_plat ? 1 : 0);
}

extern "C" __declspec(dllexport)
HRESULT GemModDetail_OnInit(GemModDetailInstance* self) {
    void* p;
    if (!self) return -1;
    self->breadcrumb_elem = 0;
    { int i; for (i = 0; i < 7; i++) self->reserved_0c[i] = 0; }
    self->title_elem = 0; self->status_elem = 0; self->version_elem = 0;
    self->author_elem = 0; self->description_elem = 0;
    self->enable_button_elem = 0; self->disable_button_elem = 0; self->delete_button_elem = 0;
    self->install_button_elem = 0; self->update_button_elem = 0;
    self->info_elem = 0; self->info_title_elem = 0; self->changelog_elem = 0;
    self->uninstall_button_elem = 0;
    self->local_idx = -1;

    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb",    &p, 0); self->breadcrumb_elem     = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"title",         &p, 0); self->title_elem          = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"status",        &p, 0); self->status_elem         = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"version",       &p, 0); self->version_elem        = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"author",        &p, 0); self->author_elem         = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"description",   &p, 0); self->description_elem    = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"enableButton",  &p, 0); self->enable_button_elem  = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"disableButton", &p, 0); self->disable_button_elem = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"deleteButton",  &p, 0); self->delete_button_elem  = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"installButton", &p, 0); self->install_button_elem = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"updateButton",  &p, 0); self->update_button_elem  = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"info",          &p, 0); self->info_elem           = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"infoTitle",     &p, 0); self->info_title_elem     = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"changelog",     &p, 0); self->changelog_elem      = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"uninstallButton", &p, 0); self->uninstall_button_elem = (DWORD)p;

    g_active_detail = self;
    return 0;
}

/* ── Confirm-then-reboot dialog (mods-tab "remove" + apply-after-change) ──────────
   The native shell's interactive "Are you sure" is a HUD-hosted dialog raised by gemstone
   0x72db8 (ZHD0 method 0xe); the context-menu Delete flow at 0x31d34 is the reference. Show
   it with the delete-confirm's button config (8,1), storing the dialog handle; the tapped
   result returns as scene message 0x8000007 (payload[0]=handle, payload[1]=result, 3=OK).
   The ZDKSystem message box does not receive taps from a gemstone scene, so it is not used.

   One dialog, two outcomes, routed by the pending action so the confirm result decides on
   structure rather than surrounding state: CONFIRM_UNINSTALL takes the device back to stock
   (flip the installer tile, arm the boot wipe) then reboots; CONFIRM_RESTART just reboots to
   apply an enable/disable/install/update the caller has already committed to disk. */
typedef int (*HudConfirmShowFn)(void* host, const wchar_t* message, int cfg, int flag,
                                int reserved, DWORD* out_handle);
#define HUD_CONFIRM_SHOW  ((HudConfirmShowFn)0x00072db8)

#define MSG_HUD_RESULT     0x8000007
#define HUD_RESULT_CONFIRM 3
#define HUD_CONFIRM_CFG    8   /* button config, verbatim from the native delete-confirm */
#define HUD_CONFIRM_FLAG   1

enum ConfirmAction { CONFIRM_NONE = 0, CONFIRM_RESTART, CONFIRM_UNINSTALL };

static volatile LONG g_confirm_action = CONFIRM_NONE;
static DWORD         g_confirm_handle = 0;

/* Off the UI thread: RebootDevice does not return, and the uninstall branch's SetTitleName
   scans the content store. Uninstall mirrors the XNA path (main.cpp uninstall_work). */
static DWORD WINAPI confirm_exec(LPVOID param) {
    if ((int)(INT_PTR)param == CONFIRM_UNINSTALL) {
        SetTitleName(L"Install Project Lyra");
        ModScanUninstallArm();
    }
    RebootDevice();   /* does not return */
    InterlockedExchange(&g_confirm_action, CONFIRM_NONE);
    return 0;
}

static void show_confirm(void* host, const wchar_t* message, int action) {
    int hr = -1;
    if (InterlockedCompareExchange(&g_confirm_action, action, CONFIRM_NONE) != CONFIRM_NONE) return;
    g_confirm_handle = 0;
    __try {
        hr = HUD_CONFIRM_SHOW(host, message, HUD_CONFIRM_CFG, HUD_CONFIRM_FLAG, 0, &g_confirm_handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
    if (hr < 0) InterlockedExchange(&g_confirm_action, CONFIRM_NONE);
}

/* HUD confirm result (scene msg 0x8000007): act only when the handle matches ours. */
static void on_hud_confirm_result(DWORD* payload) {
    DWORD h = 0, result = 0;
    int action;
    HANDLE t;
    __try { if (payload) { h = payload[0]; result = payload[1]; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!g_confirm_handle || h != g_confirm_handle) return;
    g_confirm_handle = 0;
    action = g_confirm_action;
    if (result != HUD_RESULT_CONFIRM) { InterlockedExchange(&g_confirm_action, CONFIRM_NONE); return; }
    t = CreateThread(NULL, 0, confirm_exec, (LPVOID)(INT_PTR)action, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_confirm_action, CONFIRM_NONE);
}

extern "C" __declspec(dllexport)
HRESULT GemModDetail_OnMessage(GemModDetailInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    if (msg_id == MSG_INIT_BIND) {
        render_all(self);
        __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    if (msg_id == MSG_DETACHED) {
        DWORD ph = 0;
        __try { DWORD* p = (DWORD*)m[4]; if (p) ph = p[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ph == 1 && g_active_detail == self) {
            g_active_detail = NULL;
            g_confirm_handle = 0;
            InterlockedExchange(&g_confirm_action, CONFIRM_NONE);
        }
    }

    if (msg_id == MSG_HUD_RESULT) {
        DWORD* payload = NULL;
        __try { payload = (DWORD*)m[4]; } __except (EXCEPTION_EXECUTE_HANDLER) { payload = NULL; }
        on_hud_confirm_result(payload);
        /* fall through to base: 0x8000007 is a general HUD notify. */
    }

    if (msg_id == MSG_DATA_SOURCE) {
        DataSourceSubStruct* sub = NULL;
        DWORD sub_code = 0, target = 0;
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (sub_code == SUB_DS_SET_SEL && target) {
            if ((target == self->install_button_elem || target == self->update_button_elem)
                && g_detail_target_id[0]) {
                RepoClientRequestInstall(g_detail_target_id);
                render_label((void*)self->status_elem, L"downloading");
                show_elem(self->install_button_elem, 0);
                show_elem(self->update_button_elem, 0);
                __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                return 0;
            }
            if (target == self->enable_button_elem || target == self->disable_button_elem) {
                int toggled = 0;
                if (self->local_idx >= 0) { __try { toggled = ModScanToggleEnabled(self->local_idx); } __except (EXCEPTION_EXECUTE_HANDLER) { toggled = 0; } }
                render_all(self);
                if (toggled) show_confirm((void*)self->scene_handle, L"Restart now to apply?", CONFIRM_RESTART);
                __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                return 0;
            }
            if (target == self->delete_button_elem) {
                int result = 0;
                if (self->local_idx >= 0) { __try { result = ModScanDelete(self->local_idx); } __except (EXCEPTION_EXECUTE_HANDLER) { result = -1; } }
                if (result == 1) { self->local_idx = -1; g_detail_target_id[0] = 0; }
                render_all(self);
                __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                return 0;
            }
            if (target == self->uninstall_button_elem) {
                show_confirm((void*)self->scene_handle,
                             L"Remove Project Lyra and restart the device?", CONFIRM_UNINSTALL);
                __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                return 0;
            }
        }
    }

    {
        HRESULT hr = 0;
        __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
        return hr;
    }
}

/* reposd DONE: reflect install/uninstall progress on the active detail's status +
   buttons. The browse module refreshes the browse list; the Manage list module
   refreshes its updates tab. */
extern "C" void GemModBrowseHandleRepoDone(void);
extern "C" void GemModsListHandleRepoDone(void);

extern "C" void GemModDetailOnRepoDone(void) {
    RepoBlock* repo = RepoClientBlock();
    if (repo) {
        long req = repo->request;
        if (req == REPO_REQ_INSTALL || req == REPO_REQ_UNINSTALL) {
            long st = repo->install_status;
            GemModDetailInstance* d = g_active_detail;
            if (d && d->status_elem) {
                const wchar_t* t = (req == REPO_REQ_UNINSTALL && st == REPO_INSTALL_DONE)
                                 ? L"removed" : install_status_text(st);
                render_label((void*)d->status_elem, t);
            }
            if (st == REPO_INSTALL_DONE) {
                /* The mod dir + enabled.json now reflect the install, but ModScan is only
                   re-projected on Manage entry; re-project here so the detail shows
                   enable/disable/delete without waiting for the reboot. */
                ModScanRebuild();
                if (d) {
                    render_all(d);
                    if (req == REPO_REQ_INSTALL) {
                        render_label((void*)d->status_elem, L"installed. restart to apply.");
                        show_confirm((void*)d->scene_handle, L"Restart now to apply?", CONFIRM_RESTART);
                    }
                }
            } else if (st == REPO_INSTALL_ERROR && d) {
                render_all(d);
            }
        }
    }
    GemModBrowseHandleRepoDone();
    GemModsListHandleRepoDone();
}
