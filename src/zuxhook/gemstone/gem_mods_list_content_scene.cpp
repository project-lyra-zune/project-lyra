/* gem_mods_list_content_scene.cpp - canonical sibling leaf list-content
   scene class. Same shape as GemLibraryListContentScene,
   GemMarketplaceGamesListContentScene, GemInboxListContentScene, etc.
   parent_name = "GemBaseScene". Backs the ManageModsContent.xur scene.

   Loaded INTO GemModManager's frame via LOAD_CONTENT_SCENE (0x1c250)
   when the outer's tab commit (or initial visibility) triggers the
   self-dispatched 0x1800001c chain.

   Instance layout - mirrors GemModHub/GemModManager's shape so XuiScene
   base's writes to +0x0c..+0x18 don't trample our state:

     +0x00   vtable_ptr
     +0x04   scene_handle
     +0x08   breadcrumb_elem        (no breadcrumb in this XUI, left 0;
                                     XuiScene base compares target==0
                                     which never matches a real tap)
     +0x0c   reserved (base writes here)
     +0x10   reserved
     +0x14   reserved
     +0x18   reserved
     +0x1c   reserved
     +0x20   reserved
     +0x24   reserved
     +0x28   list_element           (bound by OnInit)
     +0x2c   noItems_element        (bound by OnInit; optional)
     +0x30   view_subtype           (0=installed, 1=updates, 2=archived; msg=0x13)
     +0x34   filtered_count
     +0x38   filter_indices_ptr     (heap array mapping filtered idx →
                                     ModScanGet's unfiltered idx)
     +0x3c   detail_scene_id        (ctor extra_init - id_nav_stub target)
     +0x40   nav_args_mod_row_idx   (instance storage for id_nav_stub args)
     +0x44..+0x4c  reserved

   Total instance size: 0x50 (80 bytes). */

extern "C" {
#include "mod_scanner.h"
}

#include <windows.h>
#include <string.h>
#include "gem_scene_common.h"
#include "gem_mod_detail.h"
#include "repo_client.h"
#include "repo_version.h"

/* Twist tab -> view_subtype, delivered by the manager via msg=0x13 args.
   Tab order (and thus the index): installed, updates, archived. */
#define VIEW_INSTALLED 0
#define VIEW_UPDATES   1
#define VIEW_ARCHIVED  2

/* Case-insensitive wide compare for A-Z row sorting. NULL sorts first. */
static int wci_cmp(const wchar_t* a, const wchar_t* b) {
    if (!a) a = L"";
    if (!b) b = L"";
    for (;; a++, b++) {
        wchar_t ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca += 32;
        if (cb >= L'A' && cb <= L'Z') cb += 32;
        if (ca != cb) return (ca < cb) ? -1 : 1;
        if (!ca) return 0;
    }
}

/* True if the reposd feed advertises a newer version of this installed mod than the
   on-disk one. Opportunistic: shows an update hint if the feed is already mapped (a
   prior Browse fetch); Manage does not fetch on its own yet. */
static int row_has_update(const wchar_t* id_w, const wchar_t* ver_w) {
    RepoBlock* repo = RepoClientBlock();
    char id_a[REPO_ID_LEN], ver_a[REPO_VERSION_LEN];
    int k, i;
    if (!repo || !id_w || !ver_w) return 0;
    for (k = 0; id_w[k] && k < REPO_ID_LEN - 1; k++) id_a[k] = (char)(unsigned char)id_w[k];
    id_a[k] = 0;
    for (k = 0; ver_w[k] && k < REPO_VERSION_LEN - 1; k++) ver_a[k] = (char)(unsigned char)ver_w[k];
    ver_a[k] = 0;
    for (i = 0; i < repo->count; i++)
        if (strcmp(repo->rows[i].id, id_a) == 0)
            return version_cmp(repo->rows[i].version, ver_a) > 0;
    return 0;
}

struct GemModsListContentSceneInstance {
    DWORD vtable;
    DWORD scene_handle;
    DWORD breadcrumb_elem;       /* +0x08 - base reads, never used here */
    DWORD reserved_0c[7];        /* +0x0c..+0x24 */
    DWORD list_element;          /* +0x28 */
    DWORD noItems_element;       /* +0x2c */
    DWORD view_subtype;          /* +0x30 */
    int   filtered_count;        /* +0x34 */
    DWORD filter_indices_ptr;    /* +0x38 */
    DWORD reserved_3c;
    DWORD nav_args_mod_row_idx;  /* +0x40 - args storage for scene init */
    DWORD reserved_tail[3];      /* +0x44..+0x4c - pad to 0x50 */
};

/* The list instance currently loaded in the Manage frame. Only the updates tab
   needs it (to re-filter when the async feed answer lands); tracked for every tab
   and gated on view_subtype at DONE time. Set on OnInit, cleared on OnDestroy. */
static GemModsListContentSceneInstance* g_active_list = NULL;

/* ── Engine primitives ─────────────────────────────────────────────────── */

typedef HRESULT (*RawRowLabelFn)(DWORD out_8, DWORD out_c, const wchar_t* text);
#define RAW_ROW_LABEL  ((RawRowLabelFn)0x00083914)

typedef int (*ListGetSelectedIdxFn)(void* list_element, int* out_secondary);
#define GET_SELECTED_IDX  ((ListGetSelectedIdxFn)0x0003195c)

/* Name-based scene create + navigate. The init_data arg is delivered
   to the new scene via msg=0x13 args (same mechanism the id-based
   path used). */
typedef HRESULT (*XuiSceneCreateFn)(const wchar_t* base, const wchar_t* path,
                                      void* init_data, void** out_handle);
#define XUI_SCENE_CREATE  ((XuiSceneCreateFn)0x418358d0)

typedef int (*SceneNavigateFn)(void* current_ctx, void* scene_handle);
#define SCENE_NAVIGATE  ((SceneNavigateFn)0x0001e5d8)

#define CURRENT_CTX_GLOBAL  ((void**)0x00097300)

static int nav_to_scene_by_name_with_args(const wchar_t* name, void* args) {
    void* h = NULL;
    HRESULT hr = XUI_SCENE_CREATE(L"gem://", name, args, &h);
    if (FAILED(hr) || h == NULL) return -1;
    return SCENE_NAVIGATE(*CURRENT_CTX_GLOBAL, h);
}

/* The canonical list/noItems flip, observed verbatim in gemstone
   0x317ac (GemLibraryListContentScene). Sequence is fixed:
     ListInvalidate(list, 0, 1) → ListGetRowCount(list) →
     SetShow(list, count>0) → SetShow(noItems, count==0).
   Each primitive wraps an xuidll!XuiSendMessage to the element with
   a specific msg id. */
typedef int (*ListInvalidateFn)(void* list_element, int arg2, int arg3);
#define LIST_INVALIDATE  ((ListInvalidateFn)0x00058890)

typedef int (*ListGetRowCountFn)(void* list_element);
#define LIST_GET_ROW_COUNT  ((ListGetRowCountFn)0x0004b058)

typedef int (*SetShowFn)(void* elem, int show);
#define SET_SHOW  ((SetShowFn)0x00058860)


/* ── Canonical visibility flip ─────────────────────────────────────────

   Mirror of GemLibraryListContentScene's private helper at 0x317ac.
   Every sibling leaf list-content class in gemstone has this same shape;
   the canonical class invokes it from 4 lifecycle points (activation
   plus three row-state-change notifications). For us, the analogous
   trigger is post-rebuild_filter; call this after every filter rebuild
   so list+noItems track the count. */

static void flip_visibility(GemModsListContentSceneInstance* self) {
    if (!self->list_element) return;
    __try { LIST_INVALIDATE((void*)self->list_element, 0, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    int count = 0;
    __try { count = LIST_GET_ROW_COUNT((void*)self->list_element); }
    __except (EXCEPTION_EXECUTE_HANDLER) { count = 0; }
    __try { SET_SHOW((void*)self->list_element, count > 0 ? 1 : 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (self->noItems_element) {
        __try { SET_SHOW((void*)self->noItems_element, count <= 0 ? 1 : 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

/* ── Filter rebuild ───────────────────────────────────────────────────── */

static void rebuild_filter(GemModsListContentSceneInstance* self) {
    if (self->filter_indices_ptr) {
        LocalFree((HLOCAL)self->filter_indices_ptr);
        self->filter_indices_ptr = 0;
    }
    self->filtered_count = 0;

    const ModRowSet* rs = ModScanGet();
    if (!rs || rs->count <= 0) return;

    int* indices = (int*)LocalAlloc(LPTR, sizeof(int) * rs->count);
    if (!indices) return;

    /* Tab predicate. installed/archived split the disk rows by the enabled flag;
       updates cross-references the reposd feed and keeps any installed row (either
       state) the feed advertises a newer version for. The feed is fetched on Manage
       entry and its DONE re-runs this filter, so an initially-empty updates tab fills
       in once the answer lands. */
    int n = 0;
    for (int i = 0; i < rs->count; i++) {
        int include;
        if (self->view_subtype == VIEW_UPDATES)
            include = row_has_update(rs->rows[i].id, rs->rows[i].version);
        else
            include = (rs->rows[i].enabled == (self->view_subtype == VIEW_INSTALLED ? 1 : 0));
        if (include) indices[n++] = i;
    }

    /* Present the list A-Z by name (insertion sort; the list is small). */
    for (int a = 1; a < n; a++) {
        int key = indices[a];
        const wchar_t* kn = rs->rows[key].name;
        int b = a - 1;
        while (b >= 0 && wci_cmp(rs->rows[indices[b]].name, kn) > 0) {
            indices[b + 1] = indices[b];
            b--;
        }
        indices[b + 1] = key;
    }

    self->filter_indices_ptr = (DWORD)indices;
    self->filtered_count     = n;
}

/* ── Class entry points ────────────────────────────────────────────────── */

extern "C" __declspec(dllexport)
HRESULT GemModsListContentScene_OnInit(GemModsListContentSceneInstance* self) {
    if (!self) return -1;
    self->breadcrumb_elem        = 0;
    for (int i = 0; i < 7; i++) self->reserved_0c[i] = 0;
    self->list_element           = 0;
    self->noItems_element        = 0;
    self->view_subtype           = 0;
    self->filtered_count         = 0;
    self->filter_indices_ptr     = 0;
    self->nav_args_mod_row_idx   = 0;
    self->reserved_3c            = 0;
    self->reserved_tail[0] = self->reserved_tail[1] = self->reserved_tail[2] = 0;

    /* Bind breadcrumb if present (harmless if absent). */
    void* breadcrumb = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"breadcrumb", &breadcrumb, 0);
    self->breadcrumb_elem = (DWORD)breadcrumb;

    void* list = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle, L"list", &list, 0);
    self->list_element = (DWORD)list;

    void* noItems = NULL;
    XUI_GET_DESC_BY_ID((void*)self->scene_handle,
                        L"noItems", &noItems, 0);
    self->noItems_element = (DWORD)noItems;

    g_active_list = self;
    return 0;
}


extern "C" __declspec(dllexport)
HRESULT GemModsListContentScene_OnMessage(GemModsListContentSceneInstance* self, void* msg) {
    DWORD* m = (DWORD*)msg;
    DWORD msg_id = 0;
    __try { msg_id = m[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    DWORD sub_code = 0, target = 0;
    DataSourceSubStruct* sub = NULL;
    if (msg_id == MSG_DATA_SOURCE) {
        __try {
            sub = (DataSourceSubStruct*)m[4];
            if (sub) { sub_code = sub->sub_code; target = sub->target_elem; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    /* msg=0x13 - args from parent via 0x1c250. CRITICAL: msg->[0x10] is
       a POINTER-TO-POINTER: the outer holds the address of the args
       pointer storage (*outer = &sub_idx), whose target holds the tab
       index. Both derefs are required; a single deref yields a stack
       pointer, not the index. */
    if (msg_id == MSG_INIT_BIND) {
        DWORD parent_arg = 0;
        __try {
            DWORD** outer = (DWORD**)m[4];          /* &(args_ptr_storage) */
            if (outer) {
                DWORD* args_ptr = *outer;           /* args_ptr = &sub_idx */
                if (args_ptr) parent_arg = *args_ptr;  /* sub_idx value */
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        self->view_subtype = parent_arg;
        rebuild_filter(self);
        flip_visibility(self);
        __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    /* List data source: row count + row text. */
    if (msg_id == MSG_DATA_SOURCE && sub && target == self->list_element) {
        if (sub_code == SUB_DS_COUNT) {
            __try {
                *((DWORD*)(sub->output_area + 4)) = (DWORD)self->filtered_count;
                m[2] = 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
        if (sub_code == SUB_DS_GET_ITEM) {
            DWORD* output = NULL;
            int idx = -1, col = 0;
            DWORD out_8 = 0, out_c = 0;
            __try {
                output = (DWORD*)sub->output_area;
                if (output) {
                    idx   = (int)output[0];
                    col   = (int)output[1];
                    out_8 = output[2];
                    out_c = output[3];
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (!output || idx < 0 || idx >= self->filtered_count) return 0;

            int* indices = (int*)self->filter_indices_ptr;
            const ModRowSet* rs = ModScanGet();
            if (!indices || !rs) return 0;
            int real_idx = indices[idx];
            if (real_idx < 0 || real_idx >= rs->count) return 0;

            /* DiscographyAlbumList renders three visible text lines, bound to data-source
               columns 0, 1, and 5 (device-observed); the other queried columns are not
               rendered. Line map: 0=title, 1=author-or-update, 5=description. An enabled
               mod the resolver can't satisfy renders its held-back label on the title. */
            const ModRow* row = &rs->rows[real_idx];
            const wchar_t* text;
            if (col == 0) {
                text = (row->held_back && row->name_held_back) ? row->name_held_back : row->name;
            } else if (col == 1) {
                text = row_has_update(row->id, row->version) ? L"update available"
                                                             : (row->author ? row->author : L"");
            } else if (col == 5) {
                text = row->description ? row->description : L"";
            } else {
                text = L"";
            }
            if (!text) text = L"";
            HRESULT hr = -1;
            __try { hr = RAW_ROW_LABEL(out_8, out_c, text); }
            __except (EXCEPTION_EXECUTE_HANDLER) { hr = -1; }
            if (hr >= 0) {
                __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            return 0;
        }
        if (sub_code == SUB_DS_SET_SEL) {
            int tapped = -1;
            __try {
                tapped = GET_SELECTED_IDX((void*)self->list_element, NULL);
            } __except (EXCEPTION_EXECUTE_HANDLER) { tapped = -1; }
            if (tapped >= 0 && tapped < self->filtered_count) {
                int* indices = (int*)self->filter_indices_ptr;
                const ModRowSet* rs = ModScanGet();
                if (indices && rs) {
                    int real_idx = indices[tapped];
                    if (real_idx >= 0 && real_idx < rs->count && rs->rows[real_idx].id) {
                        /* The detail is keyed by mod_id: set the target, then navigate. */
                        GemModDetailSetTargetW(rs->rows[real_idx].id, 0);   /* from Manage: manifest description */
                        __try {
                            nav_to_scene_by_name_with_args(L"ManageModDetail.xur", NULL);
                        } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }
            }
            __try { m[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return 0;
        }
    }

    HRESULT hr = 0;
    __try { hr = XUISCENE_ON_MESSAGE(self, msg); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}

extern "C" __declspec(dllexport)
HRESULT GemModsListContentScene_OnDestroy(GemModsListContentSceneInstance* self) {
    if (self && self->filter_indices_ptr) {
        LocalFree((HLOCAL)self->filter_indices_ptr);
        self->filter_indices_ptr = 0;
    }
    if (g_active_list == self) g_active_list = NULL;
    return 0;
}

/* reposd feed DONE: the updates tab's row set derives from the feed, which Manage
   fetches asynchronously on entry. Re-filter + re-flip the active list when the
   answer lands. No-op for the installed/archived tabs (their rows are pure disk
   state). Called from the single registered DONE dispatcher (GemModDetailOnRepoDone). */
extern "C" void GemModsListHandleRepoDone(void) {
    GemModsListContentSceneInstance* self = g_active_list;
    if (!self || self->view_subtype != VIEW_UPDATES) return;
    rebuild_filter(self);
    flip_visibility(self);
}
