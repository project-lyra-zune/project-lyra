/* gem_mod_browse.cpp - the mod-repository Browse UI, hosted in zuxhook.
 *
 * Two scene classes: GemModBrowse (host: a category twist) and GemModBrowseList
 * (a category's mods). Absorbed from the former browse.dll so one binary owns all
 * mod-management UI. Reads the reposd feed through the shared RepoClient; a row tap
 * sets the detail target id and navigates to the unified detail (ManageModDetail.xur),
 * so Browse, the installed list, and the Updates list all reach the same detail. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "gem_scene_common.h"
#include "gem_mod_detail.h"
#include "repo_client.h"
#include "repo_version.h"
extern "C" {
#include "mod_scanner.h"
}

/* Engine primitives beyond the shared gem_scene_common set. */
typedef HRESULT (*XuiSceneCreateFn)(const wchar_t*, const wchar_t*, void*, void**);
#define XUI_SCENE_CREATE  ((XuiSceneCreateFn)0x418358d0)
typedef int (*SceneNavigateFn)(void*, void*);
#define SCENE_NAVIGATE  ((SceneNavigateFn)0x0001e5d8)
#define CURRENT_CTX_GLOBAL  ((void**)0x00097300)
typedef HRESULT (*RawRowLabelFn)(DWORD, DWORD, const wchar_t*);
#define RAW_ROW_LABEL  ((RawRowLabelFn)0x00083914)
typedef int (*ListGetSelectedIdxFn)(void*, int*);
#define GET_SELECTED_IDX  ((ListGetSelectedIdxFn)0x0003195c)
typedef int (*ListInvalidateFn)(void*, int, int);
#define LIST_INVALIDATE  ((ListInvalidateFn)0x00058890)
typedef int (*ListGetRowCountFn)(void*);
#define LIST_GET_ROW_COUNT  ((ListGetRowCountFn)0x0004b058)
typedef int (*SetShowFn)(void*, int);
#define SET_SHOW  ((SetShowFn)0x00058860)

#define MSG_NAV_SOURCE_QUERY 0x18000022
#define MSG_CONTENT_LOAD     0x1800001c
#define MSG_DETACHED         0x18000007

/* Fixed category tabs render the twist at bind time; the async feed refreshes only
   the list. Tab 0 ("all") = every mod; the rest filter by feed category name. The
   non-"all" tabs mirror modkit's BROWSE_CATEGORIES (manifest.py), alphabetical; feed
   --all rejects a feature mod whose category isn't one of them, so every feature mod
   maps to a tab. Case-insensitive match (cat_matches). */
static const wchar_t* const CATS[] = { L"all", L"appearance", L"media", L"network", L"utilities" };
#define NCATS ((int)(sizeof(CATS) / sizeof(CATS[0])))

static volatile LONG g_feed_fetched = 0;
static long g_last_done_seq = -1;

static void show_elem(DWORD e, int on) {
    if (e) { __try { SET_SHOW((void*)e, on ? 1 : 0); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
}

static int nav_to_scene_by_name(const wchar_t* name) {
    void* h = NULL;
    HRESULT hr = XUI_SCENE_CREATE(L"gem://", name, NULL, &h);
    if (FAILED(hr) || h == NULL) return -1;
    return SCENE_NAVIGATE(*CURRENT_CTX_GLOBAL, h);
}

static const wchar_t* category_of(const RepoRow* r) {
    return r->category[0] ? r->category : L"Other";
}
static int ci_weq(const wchar_t* a, const wchar_t* b) {
    for (;; a++, b++) {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca += 32;
        if (cb >= L'A' && cb <= L'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}
static int cat_matches(const RepoRow* r, int cat) {
    /* The platform (reserved id "lyra") is managed and updated, never browsed;
       exclude it from every Browse tab, including "all". */
    if (strcmp(r->id, "lyra") == 0) return 0;
    return cat == 0 ? 1 : ci_weq(category_of(r), CATS[cat]);
}
/* Installed-state for a catalog row is disk truth, matched by id in the scanner. */
static const ModRow* scan_find(const char* id) {
    const ModRowSet* rs = ModScanGet();
    int i, j;
    if (!rs || !id[0]) return NULL;
    for (i = 0; i < rs->count; i++) {
        const wchar_t* w = rs->rows[i].id;
        if (!w) continue;
        for (j = 0; id[j]; j++) if (w[j] != (wchar_t)(unsigned char)id[j]) break;
        if (!id[j] && w[j] == 0) return &rs->rows[i];
    }
    return NULL;
}

/* A newer feed version than the installed (scanner) version means an update. */
static int scan_update_available(const RepoRow* r, const ModRow* inst) {
    char iv[REPO_VERSION_LEN]; int k = 0;
    if (!inst || !inst->version) return 0;
    for (; inst->version[k] && k < REPO_VERSION_LEN - 1; k++)
        iv[k] = (char)(unsigned char)inst->version[k];
    iv[k] = 0;
    return version_cmp(r->version, iv) > 0;
}
static int count_in_category(int cat) {
    RepoBlock* repo = RepoClientBlock();
    int n = 0, i;
    if (cat < 0 || cat >= NCATS || !repo) return 0;
    for (i = 0; i < repo->count; i++) if (cat_matches(&repo->rows[i], cat)) n++;
    return n;
}
static int nth_in_category(int cat, int display_idx) {
    RepoBlock* repo = RepoClientBlock();
    int seen = 0, i;
    if (cat < 0 || cat >= NCATS || !repo) return -1;
    for (i = 0; i < repo->count; i++)
        if (cat_matches(&repo->rows[i], cat)) { if (seen == display_idx) return i; seen++; }
    return -1;
}

/* ── GemModBrowse (host): category twist ── */

struct GemBrowseHostInstance {
    DWORD vtable;
    DWORD scene_handle;
    DWORD breadcrumb_elem;   /* +0x08 */
    DWORD reserved_0c[4];    /* +0x0c..+0x18 */
    DWORD nav_source_elem;   /* +0x1c */
    DWORD reserved_20[2];    /* +0x20..+0x24 */
    DWORD twist_elem;        /* +0x28 */
    DWORD reserved_2c;       /* +0x2c */
    DWORD content_sub_idx;   /* +0x30 */
    DWORD reserved_34[7];    /* +0x34..+0x4c */
};

extern "C" __declspec(dllexport)
HRESULT GemModBrowse_OnInit(GemBrowseHostInstance* self) {
    void* h = NULL;
    if (!self) return -1;
    self->breadcrumb_elem = 0; self->nav_source_elem = 0;
    self->twist_elem = 0; self->content_sub_idx = 0;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &h, 0);
    self->breadcrumb_elem = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"twist", &h, 0);
    self->twist_elem = (DWORD)h;
    self->nav_source_elem = (DWORD)h;
    /* Fetch every time Browse opens (WiFi/DNS may be late at boot; refreshes installed
       state too). The fixed twist renders now; the feed refreshes the list on DONE. */
    g_feed_fetched = 0;
    RepoClientRequestFeed();
    return 0;
}

extern "C" __declspec(dllexport)
HRESULT GemModBrowse_OnMessage(GemBrowseHostInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    DWORD sub_code = 0, target = 0;
    DataSourceSubStruct* sub = NULL;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    if (msg_id == MSG_DATA_SOURCE) {
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (msg_id == MSG_DATA_SOURCE && sub && target == self->twist_elem) {
        if (sub_code == SUB_DS_COUNT) {
            __try { *((DWORD*)(sub->output_area + 4)) = (DWORD)NCATS; m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (sub_code == SUB_DS_GET_ITEM) {
            DWORD* output = NULL; int idx = -1; DWORD out_8 = 0, out_c = 0;
            HRESULT hr = -1;
            __try {
                output = (DWORD*)sub->output_area;
                if (output) { idx = (int)output[0]; out_8 = output[2]; out_c = output[3]; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (!output || idx < 0 || idx >= NCATS) return 0;
            __try { hr = RAW_ROW_LABEL(out_8, out_c, CATS[idx]); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
            if (hr >= 0) { __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            return 0;
        }
    }

    if (msg_id == MSG_CONTENT_LOAD) {
        DWORD* payload = NULL;
        __try { payload = (DWORD*)m[4]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (payload) {
            int idx = 0;
            void* hScene = NULL;
            __try { idx = (int)payload[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (idx < 0) idx = 0;
            if (idx >= NCATS) idx = NCATS - 1;
            self->content_sub_idx = (DWORD)idx;
            __try { XUI_SCENE_CREATE(L"gem://", L"BrowseModsContent.xur", &self->content_sub_idx, &hScene); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            __try { payload[1] = (DWORD)hScene; m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 0;
    }

    if (msg_id == MSG_NAV_SOURCE_QUERY) {
        __try {
            DWORD* payload = (DWORD*)m[4];
            if (payload) { GET_SELECTED_IDX((void*)self->twist_elem, (int*)payload); m[2] = 1; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    {
        HRESULT hr = 0;
        __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
        return hr;
    }
}

/* ── GemModBrowseList: a category's mods; tap opens the unified detail ── */

struct GemBrowseListInstance {
    DWORD vtable;
    DWORD scene_handle;
    DWORD breadcrumb_elem;   /* +0x08 */
    DWORD reserved_0c[3];    /* +0x0c..+0x14 */
    DWORD nav_source_elem;   /* +0x18 */
    DWORD reserved_1c[3];    /* +0x1c..+0x24 */
    DWORD list_element;      /* +0x28 */
    DWORD noItems_element;   /* +0x2c */
    DWORD loading_elem;      /* +0x30 */
    DWORD cat_idx;           /* +0x34 */
    DWORD reserved_38[6];    /* +0x38..+0x4c */
};

static GemBrowseListInstance* g_active_list = NULL;

static void list_show_state(GemBrowseListInstance* self) {
    int n, loading;
    if (!self) return;
    n = count_in_category((int)self->cat_idx);
    loading = (n == 0) && !g_feed_fetched;
    show_elem(self->list_element,    n > 0);
    show_elem(self->loading_elem,    loading);
    show_elem(self->noItems_element, (n == 0 && !loading));
}

static void list_refresh(GemBrowseListInstance* self) {
    if (!self) return;
    if (self->list_element && count_in_category((int)self->cat_idx) > 0) {
        __try { LIST_INVALIDATE((void*)self->list_element, 0, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { LIST_GET_ROW_COUNT((void*)self->list_element); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    list_show_state(self);
}

extern "C" __declspec(dllexport)
HRESULT GemModBrowseList_OnInit(GemBrowseListInstance* self) {
    void* h = NULL;
    if (!self) return -1;
    self->breadcrumb_elem = 0; self->nav_source_elem = 0;
    self->list_element = 0; self->noItems_element = 0; self->loading_elem = 0;
    self->cat_idx = 0;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &h, 0);
    self->breadcrumb_elem = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"list", &h, 0);
    self->list_element = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"noItems", &h, 0);
    self->noItems_element = (DWORD)h;
    h = NULL; XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"loading", &h, 0);
    self->loading_elem = (DWORD)h;
    g_active_list = self;
    return 0;
}

extern "C" __declspec(dllexport)
HRESULT GemModBrowseList_OnMessage(GemBrowseListInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    DWORD sub_code = 0, target = 0;
    DataSourceSubStruct* sub = NULL;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    if (msg_id == MSG_DATA_SOURCE) {
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (msg_id == MSG_INIT_BIND) {
        DWORD cat = 0;
        __try { DWORD** outer = (DWORD**)m[4]; if (outer && *outer) cat = **outer; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if ((int)cat >= NCATS) cat = 0;
        self->cat_idx = cat;
        list_refresh(self);
        __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }
    if (msg_id == MSG_DETACHED) {
        DWORD ph = 0;
        __try { DWORD* p = (DWORD*)m[4]; if (p) ph = p[0]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ph == 1 && g_active_list == self) g_active_list = NULL;
    }

    if (msg_id == MSG_DATA_SOURCE && sub && target == self->list_element) {
        int cat = (int)self->cat_idx;
        int displayed = count_in_category(cat);
        if (sub_code == SUB_DS_COUNT) {
            __try { *((DWORD*)(sub->output_area + 4)) = (DWORD)displayed; m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (sub_code == SUB_DS_GET_ITEM) {
            DWORD* output = NULL; int idx = -1, col = 0; DWORD out_8 = 0, out_c = 0;
            RepoBlock* repo = RepoClientBlock();
            int ri;
            const RepoRow* row;
            const wchar_t* field;
            HRESULT hr = -1;
            __try {
                output = (DWORD*)sub->output_area;
                if (output) { idx = (int)output[0]; col = (int)output[1]; out_8 = output[2]; out_c = output[3]; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            ri = nth_in_category(cat, idx);
            if (!output || ri < 0 || !repo) return 0;
            row = &repo->rows[ri];
            const ModRow* inst = scan_find(row->id);
            /* DiscographyAlbumList renders three visible text lines, bound to data-source
               columns 0, 1, and 5 (device-observed); the other queried columns are not
               rendered. Title (col 0) is the name alone - appending a tag overflows long
               names and the tag clips. The installed/update indicator goes in the subtitle
               (col 1), replacing the author for an installed mod. Description is col 5. */
            if (col == 0) {
                field = row->name;
            } else if (col == 1) {
                field = inst ? (scan_update_available(row, inst) ? L"update available" : L"installed")
                             : row->author;
            } else if (col == 5) {
                field = row->description;
            } else {
                field = L"";
            }
            __try { hr = RAW_ROW_LABEL(out_8, out_c, field); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
            if (hr >= 0) { __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            return 0;
        }
        if (sub_code == SUB_DS_SET_SEL) {
            int tapped = -1, ri;
            RepoBlock* repo = RepoClientBlock();
            __try { tapped = GET_SELECTED_IDX((void*)self->list_element, NULL); } __except (EXCEPTION_EXECUTE_HANDLER) { tapped = -1; }
            ri = nth_in_category(cat, tapped);
            if (ri >= 0 && repo) {
                self->nav_source_elem = self->list_element;
                GemModDetailSetTargetA(repo->rows[ri].id, 1);   /* from Browse: feed description */
                nav_to_scene_by_name(L"ManageModDetail.xur");
            }
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
    }

    if (msg_id == MSG_NAV_SOURCE_QUERY) {
        __try { DWORD* p = (DWORD*)m[4]; if (p) p[0] = self->nav_source_elem; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    {
        HRESULT hr = 0;
        __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
        return hr;
    }
}

/* reposd DONE for the feed: on a new answer, mark fetched and refresh the active
   browse list. Called from GemModDetailOnRepoDone (the single registered handler). */
extern "C" void GemModBrowseHandleRepoDone(void) {
    RepoBlock* repo = RepoClientBlock();
    if (!repo) return;
    if (repo->request == REPO_REQ_INSTALL || repo->request == REPO_REQ_UNINSTALL) {
        if (g_active_list) list_refresh(g_active_list);
        return;
    }
    if (repo->done_seq == g_last_done_seq) return;
    g_last_done_seq = repo->done_seq;
    g_feed_fetched = 1;
    if (g_active_list) list_refresh(g_active_list);
}
