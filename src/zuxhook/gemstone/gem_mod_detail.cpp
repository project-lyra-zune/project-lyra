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
 *   +0x5c changelog   +0x60 uninstall (platform only)   +0x64 requires   +0x68
 *   requiresTitle; class-blob allocates 0x6c. */

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
#include "mods_phase2.h"   /* ModsPlatformProvides (install-gate capability check) */
#include "mods_capability.h" /* ModsCapSatisfies (match a mod's requires vs feed provides) */

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
    DWORD requires_elem;         /* +0x64 - feature-mod requirement value */
    DWORD requires_title_elem;   /* +0x68 - "requires:" caption */
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

/* Installed platform version from the scanner's synthetic Lyra row ("" if none). */
static void installed_lyra_version(char* out, int cap) {
    const ModRowSet* rs = ModScanGet();
    int i, k;
    out[0] = 0;
    if (!rs) return;
    for (i = 0; i < rs->count; i++) {
        if (rs->rows[i].is_platform && rs->rows[i].version) {
            for (k = 0; rs->rows[i].version[k] && k < cap - 1; k++)
                out[k] = (char)(unsigned char)rs->rows[i].version[k];
            out[k] = 0;
            return;
        }
    }
}

/* Gate verdict for a feed row against this platform. */
enum GateVerdict { GATE_OK = 0, GATE_BLOCK_OFFER, GATE_BLOCK_OFFER_SET, GATE_BLOCK_NO_OFFER };

/* Is the pinned official channel's Lyra newer than the installed one? Only then can a
   platform update change the outcome. The version comes from the platform-authority field
   (the official channel), not a browsable row, so the remedy holds even when the rows are
   from a subscribed feed with no `lyra` entry. The installed version comes from the on-disk
   marker (the scanner row), which already reflects a staged-but-not-rebooted update, so a
   Lyra installed and pending reboot counts as up to date and is not offered again. */
static int lyra_update_available(void) {
    RepoBlock* rb = RepoClientBlock();
    char iv[REPO_VERSION_LEN];
    installed_lyra_version(iv, sizeof(iv));
    return rb && rb->plat_version[0] && iv[0] && version_cmp(rb->plat_version, iv) > 0;
}

/* Does the pinned official channel's Lyra advertise a capability satisfying `required`? The
   advertised set is the platform-authority field, so the gate can tell whether an available
   Lyra would serve a capability the running platform lacks. */
static int feed_lyra_provides(const char* required) {
    RepoBlock* rb = RepoClientBlock();
    int i;
    if (!rb) return 0;
    for (i = 0; i < rb->plat_provides_count && i < REPO_PLAT_PROV_MAX; i++)
        if (ModsCapSatisfies(rb->plat_provides[i], required)) return 1;
    return 0;
}

/* Feed row index whose provided ranges satisfy required point `need`, or -1. */
static int feed_provider_index(RepoBlock* rb, const char* need) {
    int i, j;
    for (i = 0; i < rb->count && i < REPO_MAX_ROWS; i++) {
        const RepoRow* p = &rb->rows[i];
        for (j = 0; j < p->provides_count && j < REPO_MAX_PROVIDES; j++)
            if (ModsCapSatisfies(p->provides_caps[j], need)) return i;
    }
    return -1;
}

/* The not-yet-installed mods feed row `target` depends on (its non-lyra.* uses, resolved to
   feed providers, transitively), in install order: a provider precedes any mod that uses it, so
   installing the list left to right satisfies each mod's dependencies before it. Fills `out`
   with feed-row indices (target excluded) and returns the count; -1 with the unmet capability
   name in `err` if a dependency has no provider anywhere. Cycle-safe: a back edge is skipped.
   Resolution is over the feed listing and the installed set only, never a feed's word that a
   dependency is satisfied. */
static int mod_dep_install_order(const RepoRow* target, int* out, int out_cap,
                                 char* err, int err_cap) {
    RepoBlock* rb = RepoClientBlock();
    signed char state[REPO_MAX_ROWS];              /* 0 unseen, 1 on stack, 2 done */
    int node[REPO_MAX_ROWS], kidx[REPO_MAX_ROWS];  /* post-order DFS stack + resume cursor */
    int sp = 0, out_n = 0, ti, i;
    if (!rb || !target) return 0;
    ti = (int)(target - rb->rows);
    if (ti < 0 || ti >= rb->count) return 0;
    for (i = 0; i < REPO_MAX_ROWS; i++) state[i] = 0;
    node[sp] = ti; kidx[sp] = 0; state[ti] = 1; sp++;
    while (sp > 0) {
        const RepoRow* m = &rb->rows[node[sp - 1]];
        int k = kidx[sp - 1], descended = 0;
        for (; k < m->uses_count && k < REPO_MAX_USES; k++) {
            const char* need = m->uses_caps[k];
            int pi;
            if (strncmp(need, "lyra.", 5) == 0) continue;   /* platform cap, not a mod dep */
            pi = feed_provider_index(rb, need);
            if (pi < 0) {
                for (i = 0; need[i] && i < err_cap - 1; i++) err[i] = need[i];
                err[i] = 0;
                return -1;
            }
            if (state[pi] != 0) continue;                   /* already resolved, or a back edge */
            if (find_local(rb->rows[pi].id) >= 0) { state[pi] = 2; continue; }  /* installed */
            kidx[sp - 1] = k + 1;                            /* resume past this child on return */
            if (sp < REPO_MAX_ROWS) { node[sp] = pi; kidx[sp] = 0; state[pi] = 1; sp++; }
            descended = 1;
            break;
        }
        if (descended) continue;
        state[node[sp - 1]] = 2;
        if (node[sp - 1] != ti && out_n < out_cap) out[out_n++] = node[sp - 1];
        sp--;
    }
    return out_n;
}

/* Display names of the mods an install of `r` would pull in (0 = none), or -1 with the unmet
   capability in `out` when a dependency has no provider. Formats the ordered closure above. */
static int mod_deps_needed(const RepoRow* r, wchar_t* out, int out_cap) {
    RepoBlock* rb = RepoClientBlock();
    int idx[REPO_MAX_INSTALL_SET];
    char err[REPO_CAP_LEN];
    int n, i, j, len = 0;
    out[0] = 0;
    if (!rb) return 0;
    n = mod_dep_install_order(r, idx, REPO_MAX_INSTALL_SET, err, sizeof(err));
    if (n < 0) {
        for (j = 0; err[j] && j < out_cap - 1; j++) out[j] = (wchar_t)(unsigned char)err[j];
        out[j] = 0;
        return -1;
    }
    for (i = 0; i < n; i++) {
        const wchar_t* nm = rb->rows[idx[i]].name;
        if (len > 0 && len < out_cap - 2) { out[len++] = L','; out[len++] = L' '; }
        for (j = 0; nm && nm[j] && len < out_cap - 1; j++) out[len++] = nm[j];
    }
    out[len] = 0;
    return n;
}

/* Classify feed row `r` against this platform on install. A used lyra.* capability is a point
   (name@r) against the platform window [min_compat, cur]: r < min_compat means the platform
   moved past the revision the mod used (the mod needs updating; no Lyra update helps), r > cur
   (or the platform lacks it) means the platform is too old, which a newer Lyra may fix. A
   non-lyra.* capability is a mod dependency, resolved against installed mods and the feed. */
static int mod_compatible(const RepoRow* r, wchar_t* reason, int reason_cap) {
    int k;
    if (!r) return GATE_OK;
    for (k = 0; k < r->uses_count && k < REPO_MAX_USES; k++) {
        const char* need = r->uses_caps[k];
        char name[MODS_CAP_NAME_MAX];
        int rr = 1, cur, min_compat;
        if (strncmp(need, "lyra.", 5) != 0) continue;   /* mod dependency; resolved below */
        ModsCapParse(need, name, sizeof(name), &rr);
        if (ModsPlatformCapabilityRange(name, &cur, &min_compat)) {
            if (ModsCapRangeSatisfies(min_compat, cur, rr)) continue;
            if (rr < min_compat) {
                _snwprintf(reason, reason_cap - 1,
                           L"This mod needs an update: it uses %S, which this Lyra no longer supports.",
                           need);
                reason[reason_cap - 1] = 0;
                return GATE_BLOCK_NO_OFFER;
            }
            /* rr > cur: the platform is too old for this capability. */
        }
        if (feed_lyra_provides(need)) {
            /* A newer Lyra provides it: offer the update, or ask for a restart when that
               Lyra is already installed and only pending reboot. */
            if (lyra_update_available()) {
                _snwprintf(reason, reason_cap - 1,
                           L"Requires a newer Lyra (needs %S). Update Lyra now?", need);
                reason[reason_cap - 1] = 0;
                return GATE_BLOCK_OFFER;
            }
            _snwprintf(reason, reason_cap - 1,
                       L"A Lyra update is installed but not applied yet. Restart to apply "
                       L"it, then install this mod.");
        } else {
            _snwprintf(reason, reason_cap - 1,
                       L"This mod needs %S, which no available Lyra provides.", need);
        }
        reason[reason_cap - 1] = 0;
        return GATE_BLOCK_NO_OFFER;
    }
    {
        wchar_t list[REPO_NAME_LEN * 4 + 16];
        int n = mod_deps_needed(r, list, (int)(sizeof(list) / sizeof(list[0])));
        if (n < 0) {
            _snwprintf(reason, reason_cap - 1,
                       L"Requires %s, which no available mod provides.", list);
            reason[reason_cap - 1] = 0;
            return GATE_BLOCK_NO_OFFER;
        }
        if (n > 0) {
            _snwprintf(reason, reason_cap - 1, L"%s also needs: %s. Install them together?",
                       r->name, list);
            reason[reason_cap - 1] = 0;
            return GATE_BLOCK_OFFER_SET;
        }
    }
    return GATE_OK;
}

/* Format the capabilities in a mod's footprint that this platform does not provide, the
   ones that would block an install here. Returns 1 if any are unmet. */
static int format_unmet(const RepoRow* r, wchar_t* out, int cap) {
    int len = 0, k;
    out[0] = 0;
    if (!r) return 0;
    for (k = 0; k < r->uses_count && k < REPO_MAX_USES; k++) {
        wchar_t part[REPO_CAP_LEN + 4];
        int j = 0;
        if (strncmp(r->uses_caps[k], "lyra.", 5) != 0) continue;   /* mod dep, not a platform cap */
        if (ModsPlatformProvides(r->uses_caps[k])) continue;
        _snwprintf(part, REPO_CAP_LEN + 3, L"%s%S", (len > 0) ? L", " : L"", r->uses_caps[k]);
        part[REPO_CAP_LEN + 3] = 0;
        while (part[j] && len < cap - 1) out[len++] = part[j++];
        out[len] = 0;
    }
    return out[0] != 0;
}

/* Extract the reason from a scanner name_held_back ("<name> (held back: <reason>)") into `out`
   without the trailing ')'. Falls back to the whole string if the marker is absent. */
static void held_back_reason(const wchar_t* nh, wchar_t* out, int cap) {
    static const wchar_t marker[] = L"(held back: ";
    const wchar_t* p;
    int i;
    out[0] = 0;
    if (!nh) return;
    for (p = nh; *p; p++) {
        const wchar_t* a = p;
        const wchar_t* b = marker;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) { p = a; break; }
    }
    if (!*p) p = nh;
    for (i = 0; p[i] && i < cap - 1; i++) out[i] = p[i];
    if (i > 0 && out[i - 1] == L')') i--;
    out[i] = 0;
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

    /* The one platform (Lyra) is update-only and version-matched, so it carries no
       requirements and never shows enable/disable/remove here. */
    int is_plat = present && local->is_platform;

    /* Requires row: for a not-yet-installed mod, the platform capabilities its footprint needs
       that this platform lacks; for an installed but held-back mod, the boot resolver's reason
       (an unmet dependency), so a silently-inert mod explains itself. Hidden otherwise. */
    {
        wchar_t rq[REPO_CAP_LEN * REPO_MAX_USES + 48];
        int has_req = 0;
        if (present && local->held_back && local->name_held_back) {
            held_back_reason(local->name_held_back, rq, (int)(sizeof(rq) / sizeof(rq[0])));
            has_req = rq[0] != 0;
        } else if (!is_plat && feed) {
            has_req = format_unmet(feed, rq, (int)(sizeof(rq) / sizeof(rq[0])));
        }
        if (has_req) render_label((void*)self->requires_elem, rq);
        show_elem(self->requires_elem, has_req);
        show_elem(self->requires_title_elem, has_req);
    }

    /* Info row: shown only when experimental. Prefer the installed manifest's flag
       (self-describing), else the feed's, so installed and catalog mods agree. */
    {
        int exp = local ? local->experimental : (feed ? feed->experimental : 0);
        if (exp) render_label((void*)self->info_elem, L"experimental");
        show_elem(self->info_elem, exp);
        show_elem(self->info_title_elem, exp);
    }

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
        else if (present && local->held_back) s = L"held back";
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
    self->requires_elem = 0; self->requires_title_elem = 0;
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
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"requires",      &p, 0); self->requires_elem       = (DWORD)p;
    p = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"requiresTitle", &p, 0); self->requires_title_elem = (DWORD)p;

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
#define HUD_CONFIRM_CFG    8   /* two-button confirm (doubleOk + cancel); native delete-confirm */
#define HUD_CONFIRM_CFG_OK 3   /* one-button notice (singleOkButton only); zhud WaitMessageBox switch */
#define HUD_CONFIRM_FLAG   1

enum ConfirmAction { CONFIRM_NONE = 0, CONFIRM_RESTART, CONFIRM_UNINSTALL, CONFIRM_UPDATE_LYRA, CONFIRM_DISMISS, CONFIRM_INSTALL_SET };

/* Build the ordered install set for `target_id` (its not-yet-installed dependency closure, then
   the target) and hand it to reposd. Recomputed here from live feed + installed state, not from
   the offer, so a change since the offer cannot install a stale set. */
static void start_install_set(const char* target_id) {
    RepoBlock* rb = RepoClientBlock();
    const RepoRow* r = find_feed(target_id);
    int idx[REPO_MAX_INSTALL_SET];
    char err[REPO_CAP_LEN];
    char ids[REPO_MAX_INSTALL_SET][REPO_ID_LEN];
    int n, i, j, m = 0;
    if (!rb || !r) return;
    n = mod_dep_install_order(r, idx, REPO_MAX_INSTALL_SET - 1, err, sizeof(err));
    if (n < 0) return;
    for (i = 0; i < n && m < REPO_MAX_INSTALL_SET - 1; i++, m++) {
        const char* s = rb->rows[idx[i]].id;
        for (j = 0; s[j] && j < REPO_ID_LEN - 1; j++) ids[m][j] = s[j];
        ids[m][j] = 0;
    }
    for (j = 0; target_id[j] && j < REPO_ID_LEN - 1; j++) ids[m][j] = target_id[j];
    ids[m][j] = 0; m++;
    RepoClientRequestInstallSet(ids, m);
}

static volatile LONG g_confirm_action = CONFIRM_NONE;
static DWORD         g_confirm_handle = 0;

/* Off the UI thread: RebootDevice does not return, and the uninstall branch's SetTitleName
   scans the content store. Uninstall mirrors the XNA path (main.cpp uninstall_work). */
static DWORD WINAPI confirm_exec(LPVOID param) {
    int action = (int)(INT_PTR)param;
    if (action == CONFIRM_DISMISS) {
        /* Informational prompt (e.g. a requirement no available update can meet):
           the dialog just wraps and shows the full message, tapping only dismisses. */
        InterlockedExchange(&g_confirm_action, CONFIRM_NONE);
        return 0;
    }
    if (action == CONFIRM_UPDATE_LYRA) {
        /* Install the platform (reserved id "lyra") from the same feed; the
           install's DONE handler raises the restart prompt. No reboot here. */
        RepoClientRequestInstall("lyra");
        InterlockedExchange(&g_confirm_action, CONFIRM_NONE);
        return 0;
    }
    if (action == CONFIRM_INSTALL_SET) {
        /* Install the viewed mod together with its dependency closure, in order; the last
           member's DONE raises the restart prompt like a single install. */
        start_install_set(g_detail_target_id);
        InterlockedExchange(&g_confirm_action, CONFIRM_NONE);
        return 0;
    }
    if (action == CONFIRM_UNINSTALL) {
        SetTitleName(L"Install Project Lyra");
        ModScanUninstallArm();
    }
    RebootDevice();   /* does not return */
    InterlockedExchange(&g_confirm_action, CONFIRM_NONE);
    return 0;
}

static void show_confirm(void* host, const wchar_t* message, int action) {
    int hr = -1;
    /* An informational prompt with nothing to confirm gets one OK button; the
       actionable confirms get the two-button layout. */
    int cfg = (action == CONFIRM_DISMISS) ? HUD_CONFIRM_CFG_OK : HUD_CONFIRM_CFG;
    if (InterlockedCompareExchange(&g_confirm_action, action, CONFIRM_NONE) != CONFIRM_NONE) return;
    g_confirm_handle = 0;
    __try {
        hr = HUD_CONFIRM_SHOW(host, message, cfg, HUD_CONFIRM_FLAG, 0, &g_confirm_handle);
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
    if (action == CONFIRM_INSTALL_SET && g_active_detail) {
        show_elem(g_active_detail->install_button_elem, 0);
        show_elem(g_active_detail->update_button_elem, 0);
        render_label((void*)g_active_detail->status_elem, L"downloading");
    }
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
                const RepoRow* feed = find_feed(g_detail_target_id);
                wchar_t reason[192];
                int verdict = feed ? mod_compatible(feed, reason,
                        (int)(sizeof(reason) / sizeof(reason[0]))) : GATE_OK;
                if (verdict == GATE_BLOCK_OFFER) {
                    show_confirm((void*)self->scene_handle, reason, CONFIRM_UPDATE_LYRA);
                    __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    return 0;
                }
                if (verdict == GATE_BLOCK_OFFER_SET) {
                    show_confirm((void*)self->scene_handle, reason, CONFIRM_INSTALL_SET);
                    __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    return 0;
                }
                if (verdict == GATE_BLOCK_NO_OFFER) {
                    show_confirm((void*)self->scene_handle, reason, CONFIRM_DISMISS);
                    __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    return 0;
                }
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
            /* The install-gate's "Update Lyra" offer installs the platform (id "lyra"),
               not the mod being viewed, so its progress renders on the mod's own detail
               page. Label it as a Lyra update so it is not mistaken for the mod
               installing. Updating Lyra from the Lyra row itself is the normal path and
               keeps the plain text. */
            int lyra_update = strcmp(repo->install_id, "lyra") == 0
                              && strcmp(g_detail_target_id, "lyra") != 0;
            int is_set = (req == REPO_REQ_INSTALL && repo->install_set_count > 1);
            if (d && d->status_elem) {
                const wchar_t* t;
                wchar_t setbuf[128];
                if (lyra_update)
                    t = (st == REPO_INSTALL_DONE) ? L"Lyra updated. restart to apply."
                                                  : L"updating Lyra...";
                else if (req == REPO_REQ_UNINSTALL && st == REPO_INSTALL_DONE)
                    t = L"removed";
                else if (is_set && st != REPO_INSTALL_DONE) {
                    /* A member in flight: name it and its position; DONE falls through to the
                       set-complete text below. */
                    const RepoRow* cur = find_feed(repo->install_id);
                    const wchar_t* nm = cur ? cur->name : L"a required mod";
                    if (st == REPO_INSTALL_ERROR)
                        _snwprintf(setbuf, 127, L"couldn't install %s", nm);
                    else
                        _snwprintf(setbuf, 127, L"%s (%d of %d): %s", nm,
                                   (int)repo->install_set_index + 1,
                                   (int)repo->install_set_count, install_status_text(st));
                    setbuf[127] = 0;
                    t = setbuf;
                }
                else
                    t = install_status_text(st);
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
                        wchar_t donebuf[64];
                        const wchar_t* dt = lyra_update ? L"Lyra updated. restart to apply."
                                                        : L"installed. restart to apply.";
                        if (is_set) {
                            _snwprintf(donebuf, 63, L"installed %d mods. restart to apply.",
                                       (int)repo->install_set_count);
                            donebuf[63] = 0; dt = donebuf;
                        }
                        render_label((void*)d->status_elem, dt);
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
