/* mods_phase2_settings_inject.c - inject a row into a native settings
   sub-list (GemSettingsListScene). Sibling of mods_phase2_menu.c: that one
   adds a Start-menu tile via GemStartListScene::populate; this one adds a row
   to a settings section's list and routes its tap to a mod scene by name.

   A settings section is a static array of 16-byte records in gemstone .rdata:
   record = {label:u32, type:u32, arg:u32, code:u32}; record[0].label doubles
   as the section header; the array ends at the first record whose type is
   0xFFFFFFFF. GemSettingsListScene's section mapper (0x5c394) returns the
   array base for a section id; populate walks it into the render vector, whose
   entries point at each record's `type` field. get-item (0x5c6f4) and row-tap
   (0x5ced4) then read the record by type.

   To add a row we redirect the section mapper to a mod-owned copy of the
   section array with our record(s) appended, and give our records a sentinel
   type (MOD_ROW_TYPE) that the native switch never matches; two wrap detours
   handle that type: get-item renders our label, row-tap navigates to our scene
   by name (XuiSceneCreate + scene_navigate_wrapper, the same race-free path the
   menu tiles use, never the scene-id table). All addresses are gemstone v4.5 at
   its preferred base 0x00010000 (static VA == live VA). */

#include "mods_phase2_internal.h"
#include "mods_log.h"
#include "kerncore.h"
#include <windows.h>
#include <string.h>

/* ── gemstone v4.5 fixed addresses (base 0x10000) ───────────────────────── */
#define SECTION_MAPPER        0x0005c394u   /* (section_id, &out_base) -> HRESULT */
#define SECTION_MAPPER_ORIG0  0xe2402001u   /* sub r2, r0, #1     */
#define SECTION_MAPPER_ORIG1  0xe352000bu   /* cmp r2, #0xb       */
#define GET_ITEM              0x0005c6f4u   /* (this, elem, sub, out_flag) */
#define GET_ITEM_ORIG0        0xe92d40f0u   /* push {r4,r5,r6,r7,lr} */
#define GET_ITEM_ORIG1        0xe24dd00cu   /* sub sp, sp, #0xc   */
#define ROW_TAP               0x0005ced4u   /* (this, elem, ...) */
#define ROW_TAP_ORIG0         0xe92d4030u   /* push {r4,r5,lr}   */
#define ROW_TAP_ORIG1         0xe24dd014u   /* sub sp, sp, #0x14 */

#define LIST_GET_SEL_IDX      0x0003195cu   /* (list, NULL) -> selected row idx */
#define BIND_ROW_LABEL_SHIM   0x0001c3f4u   /* (string_id, out_8, out_c) -> HRESULT */
#define XUI_SCENE_CREATE_VA   0x418358d0u   /* xuidll, fixed image base */
#define SCENE_NAV_WRAPPER     0x0001e5d8u   /* (ctx, hScene) */
#define CURRENT_CTX_GLOBAL    0x00097300u

/* instance offsets (GemSettingsListScene, live-confirmed) */
#define INST_LIST_ELEM        0x48u         /* bound XuiList element */
#define INST_VEC_BEGIN        0x3cu         /* render vector begin ptr */
#define INST_VEC_END          0x40u         /* render vector end ptr   */

/* Sentinel type for a mod-injected settings row. Outside every native type
   (0x50..0x5d, 0x65, 0x68..0x6b): the native get-item / row-tap switches never
   match it, so our wraps own it end to end. */
#define MOD_ROW_TYPE          0x00000090u
#define SECTION_SENTINEL      0xffffffffu

typedef HRESULT (*SectMapFn)(DWORD section_id, DWORD* out_base);
typedef HRESULT (*GetItemFn)(DWORD r0, DWORD r1, DWORD r2, DWORD r3);
typedef HRESULT (*RowTapFn)(DWORD r0, DWORD r1, DWORD r2, DWORD r3);
typedef int     (*ListGetSelFn)(DWORD list, DWORD* out);
typedef HRESULT (*BindRowLabelFn)(DWORD string_id, DWORD out_8, DWORD out_c);
typedef HRESULT (*XuiSceneCreateFn)(const wchar_t* base, const wchar_t* name,
                                    void* init, void** out);
typedef HRESULT (*SceneNavFn)(void* ctx, void* hScene);

/* ── pending rows (recorded by apply, drained by flush) ─────────────────── */
typedef struct {
    int   section_id;
    int   label_id;
    DWORD scene_name_va;   /* scratch VA of L"<name>.xur" */
} PendingSettingsRow;

#define MAX_PENDING_ROWS 16
static PendingSettingsRow g_pending[MAX_PENDING_ROWS];
static int                g_pending_count = 0;

/* per-section mod-owned extended arrays (section id -> array VA) */
typedef struct { int section_id; DWORD ext_va; } SectionExt;
#define MAX_EXT_SECTIONS 8
static SectionExt g_ext[MAX_EXT_SECTIONS];
static int        g_ext_count = 0;

/* call-through trampolines to the originals (this process) */
static SectMapFn g_orig_sectmap = 0;
static GetItemFn g_orig_getitem = 0;
static RowTapFn  g_orig_rowtap  = 0;
static int       g_patched = 0;

/* ── shared low-level helpers (mirrors mods_ui_tint.c) ──────────────────── */
static DWORD build_tramp(DWORD target, DWORD orig0, DWORD orig1) {
    DWORD* t = (DWORD*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    t[0] = orig0;
    t[1] = orig1;
    t[2] = 0xe51ff004u;      /* ldr pc, [pc, #-4] */
    t[3] = target + 8;
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    return (DWORD)t;
}

static int patch_entry(DWORD target, void* wrapper) {
    DWORD proc, entry[2], cur = 0;
    __try { cur = *(volatile DWORD*)target; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (cur == 0xe51ff004u) return 0;                 /* already redirected */
    if (!kerncore_is_ready() || !kerncore_ensure_helpers()) return -1;
    proc = kerncore_find_proc_struct(GetCurrentProcessId());
    if (proc == 0) return -1;
    entry[0] = 0xe51ff004u;
    entry[1] = (DWORD)wrapper;
    if (kerncore_patch_code(proc, target, entry, 8) != 0) return -1;
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 8);
    return 0;
}

static DWORD ext_for_section(int section_id) {
    int i;
    for (i = 0; i < g_ext_count; i++)
        if (g_ext[i].section_id == section_id) return g_ext[i].ext_va;
    return 0;
}

/* ── the three wrap detours ─────────────────────────────────────────────── */

/* Section mapper: hand out our extended array for a targeted section. */
static HRESULT sectmap_wrap(DWORD section_id, DWORD out_base, DWORD r2, DWORD r3) {
    DWORD ext;
    (void)r2; (void)r3;
    if (!g_orig_sectmap)
        g_orig_sectmap = (SectMapFn)build_tramp(SECTION_MAPPER,
                                    SECTION_MAPPER_ORIG0, SECTION_MAPPER_ORIG1);
    if (!g_orig_sectmap) return (HRESULT)0x8000ffffu;
    ext = ext_for_section((int)section_id);
    if (ext && out_base) {
        __try { *(DWORD*)out_base = ext; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return (HRESULT)0x8000ffffu; }
        return 0;
    }
    return g_orig_sectmap(section_id, (DWORD*)out_base);
}

/* Resolve the row record a data-source query targets. Returns the record VA
   (points at record.type, matching the render vector) or 0 if it isn't our
   list / a valid in-range row. */
static DWORD resolve_row(DWORD this_, DWORD target_elem, int idx) {
    DWORD list = 0, begin = 0, end = 0, row = 0;
    int count;
    __try {
        list = *(volatile DWORD*)(this_ + INST_LIST_ELEM);
        if (target_elem != list) return 0;
        if (idx < 0) return 0;
        begin = *(volatile DWORD*)(this_ + INST_VEC_BEGIN);
        end   = *(volatile DWORD*)(this_ + INST_VEC_END);
        count = (int)((end - begin) >> 2);
        if (idx >= count) return 0;
        row = ((volatile DWORD*)begin)[idx];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return row;
}

/* get-item: render our label for a MOD_ROW_TYPE record; else the original.
   sub = {idx, gate, out_8, out_c}; render only when gate == 0. */
static HRESULT getitem_wrap(DWORD this_, DWORD elem, DWORD sub, DWORD out_flag) {
    DWORD row, type, label, out_8, out_c, gate;
    int idx;
    HRESULT hr;
    if (!g_orig_getitem)
        g_orig_getitem = (GetItemFn)build_tramp(GET_ITEM,
                                        GET_ITEM_ORIG0, GET_ITEM_ORIG1);
    if (!g_orig_getitem) return (HRESULT)0x8000ffffu;

    __try {
        idx   = (int)((volatile DWORD*)sub)[0];
        gate  = ((volatile DWORD*)sub)[1];
        out_8 = ((volatile DWORD*)sub)[2];
        out_c = ((volatile DWORD*)sub)[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return g_orig_getitem(this_, elem, sub, out_flag);
    }
    if (gate != 0) return g_orig_getitem(this_, elem, sub, out_flag);

    row = resolve_row(this_, elem, idx);
    if (!row) return g_orig_getitem(this_, elem, sub, out_flag);
    __try { type = *(volatile DWORD*)row; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return g_orig_getitem(this_, elem, sub, out_flag); }
    if (type != MOD_ROW_TYPE) return g_orig_getitem(this_, elem, sub, out_flag);

    __try { label = *(volatile DWORD*)(row - 4); }   /* record.label precedes .type */
    __except (EXCEPTION_EXECUTE_HANDLER) { return (HRESULT)0x8000ffffu; }

    hr = ((BindRowLabelFn)BIND_ROW_LABEL_SHIM)(label, out_8, out_c);
    if ((int)hr < 0) return hr;
    __try { *(DWORD*)out_flag = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return (HRESULT)0x8000ffffu; }
    return hr;
}

/* row-tap: navigate a MOD_ROW_TYPE record to its scene by name; else original.
   The scene name wstring VA is stored in the record's `code` field (+8). */
static HRESULT rowtap_wrap(DWORD this_, DWORD elem, DWORD r2, DWORD r3) {
    DWORD list = 0, row, type, scene_va = 0;
    int idx;
    void* hScene = NULL;
    void* ctx;
    if (!g_orig_rowtap)
        g_orig_rowtap = (RowTapFn)build_tramp(ROW_TAP, ROW_TAP_ORIG0, ROW_TAP_ORIG1);
    if (!g_orig_rowtap) return (HRESULT)0x8000ffffu;

    __try { list = *(volatile DWORD*)(this_ + INST_LIST_ELEM); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return g_orig_rowtap(this_, elem, r2, r3); }
    if (elem != list) return g_orig_rowtap(this_, elem, r2, r3);

    idx = ((ListGetSelFn)LIST_GET_SEL_IDX)(list, 0);
    row = resolve_row(this_, elem, idx);
    if (!row) return g_orig_rowtap(this_, elem, r2, r3);
    __try {
        type = *(volatile DWORD*)row;
        scene_va = *(volatile DWORD*)(row + 8);   /* record.code holds the name VA */
    } __except (EXCEPTION_EXECUTE_HANDLER) { return g_orig_rowtap(this_, elem, r2, r3); }
    if (type != MOD_ROW_TYPE || !scene_va)
        return g_orig_rowtap(this_, elem, r2, r3);

    ctx = *(void**)CURRENT_CTX_GLOBAL;
    if (((XuiSceneCreateFn)XUI_SCENE_CREATE_VA)(L"gem://",
            (const wchar_t*)scene_va, NULL, &hScene) == 0 && hScene)
        ((SceneNavFn)SCENE_NAV_WRAPPER)(ctx, hScene);
    return 0;
}

/* ── apply: record a pending row ────────────────────────────────────────── */
int apply_inject_settings_row(ModAction* a, ModsArena* arena) {
    int label_id, section_id, name_chars, i;
    const char* scene_name = NULL;
    wchar_t name_w[64];
    DWORD name_va;

    if (g_pending_count >= MAX_PENDING_ROWS) {
        ModsLogf(L"    inject_settings_row: pending list full (%d)", MAX_PENDING_ROWS);
        return -1;
    }
    if (ModActionGetInt(a, "section", -1, &section_id) < 0 || section_id < 0) {
        ModsLogf(L"    inject_settings_row: missing/invalid section");
        return -1;
    }
    if (ModActionGetInt(a, "label_id_or_ref", -1, &label_id) < 0 || label_id < 0) {
        if (ModActionGetInt(a, "label_id", -1, &label_id) < 0 || label_id < 0) {
            ModsLogf(L"    inject_settings_row: missing label_id");
            return -1;
        }
    }
    if (ModActionGetString(a, "scene", arena, &scene_name, NULL, 0) != 0
        || scene_name == NULL) {
        ModsLogf(L"    inject_settings_row: missing scene");
        return -1;
    }

    /* Plant L"<scene>.xur" in scratch (manifest gives the bare name). */
    name_chars = (int)strlen(scene_name);
    if (name_chars + 4 + 1 > (int)(sizeof(name_w) / sizeof(name_w[0]))) {
        ModsLogf(L"    inject_settings_row: scene %S too long", scene_name);
        return -1;
    }
    for (i = 0; i < name_chars; i++)
        name_w[i] = (wchar_t)(unsigned char)scene_name[i];
    name_w[name_chars + 0] = L'.';
    name_w[name_chars + 1] = L'x';
    name_w[name_chars + 2] = L'u';
    name_w[name_chars + 3] = L'r';
    name_w[name_chars + 4] = 0;
    name_va = scratch_alloc((name_chars + 4 + 1) * 2);
    if (!name_va || scratch_write(name_va, name_w, (name_chars + 4 + 1) * 2) < 0) {
        ModsLogf(L"    inject_settings_row: scene name plant failed");
        return -1;
    }

    g_pending[g_pending_count].section_id    = section_id;
    g_pending[g_pending_count].label_id      = label_id;
    g_pending[g_pending_count].scene_name_va = name_va;
    g_pending_count++;
    ModsLogf(L"    inject_settings_row: section=%d label=%d scene=%S.xur (name_va=0x%08x)",
             section_id, label_id, scene_name, name_va);
    return 0;
}

/* Build one extended array for `section_id`: copy the native records up to the
   sentinel, append every pending row for this section as a MOD_ROW_TYPE record
   {label, MOD_ROW_TYPE, 0, scene_name_va}, then the sentinel. Returns 0 on ok. */
static int build_section_ext(int section_id) {
    DWORD base = 0, ext_va, rec[4];
    DWORD* src;
    int nrec = 0, i, mine = 0, out_off;
    SectMapFn orig = (SectMapFn)SECTION_MAPPER;   /* not yet patched at flush time */

    if (g_ext_count >= MAX_EXT_SECTIONS) return -1;
    if (ext_for_section(section_id)) return 0;    /* already built */

    if (orig(section_id, &base) != 0 || base == 0) {
        ModsLogf(L"    settings ext: section %d has no native array", section_id);
        return -1;
    }
    src = (DWORD*)base;
    __try {
        while (src[nrec * 4 + 1] != SECTION_SENTINEL) {
            nrec++;
            if (nrec > 64) return -1;             /* runaway guard */
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }

    for (i = 0; i < g_pending_count; i++)
        if (g_pending[i].section_id == section_id) mine++;
    if (mine == 0) return 0;

    ext_va = scratch_alloc((nrec + mine + 1) * 16);
    if (!ext_va) { ModsLogf(L"    settings ext: scratch exhausted"); return -1; }

    /* native records, verbatim */
    if (scratch_write(ext_va, src, nrec * 16) < 0) return -1;
    out_off = nrec * 16;

    /* injected records */
    for (i = 0; i < g_pending_count; i++) {
        if (g_pending[i].section_id != section_id) continue;
        rec[0] = (DWORD)g_pending[i].label_id;
        rec[1] = MOD_ROW_TYPE;
        rec[2] = 0;
        rec[3] = g_pending[i].scene_name_va;
        if (scratch_write(ext_va + out_off, rec, 16) < 0) return -1;
        out_off += 16;
    }

    /* sentinel record */
    rec[0] = 0; rec[1] = SECTION_SENTINEL; rec[2] = 0; rec[3] = 0;
    if (scratch_write(ext_va + out_off, rec, 16) < 0) return -1;

    g_ext[g_ext_count].section_id = section_id;
    g_ext[g_ext_count].ext_va     = ext_va;
    g_ext_count++;
    ModsLogf(L"    settings ext: section %d -> 0x%08x (%d native + %d injected rows)",
             section_id, ext_va, nrec, mine);
    return 0;
}

/* ── flush: build extended arrays + install the three detours ───────────── */
int flush_settings_rows(void) {
    int i;
    if (g_pending_count == 0) return 0;

    for (i = 0; i < g_pending_count; i++)
        (void)build_section_ext(g_pending[i].section_id);
    if (g_ext_count == 0) {
        ModsLogf(L"  flush_settings_rows: no sections built");
        return -1;
    }

    /* Build call-throughs before redirecting the entries. */
    if (!g_orig_sectmap)
        g_orig_sectmap = (SectMapFn)build_tramp(SECTION_MAPPER,
                                    SECTION_MAPPER_ORIG0, SECTION_MAPPER_ORIG1);
    if (!g_orig_getitem)
        g_orig_getitem = (GetItemFn)build_tramp(GET_ITEM,
                                        GET_ITEM_ORIG0, GET_ITEM_ORIG1);
    if (!g_orig_rowtap)
        g_orig_rowtap = (RowTapFn)build_tramp(ROW_TAP, ROW_TAP_ORIG0, ROW_TAP_ORIG1);
    if (!g_orig_sectmap || !g_orig_getitem || !g_orig_rowtap) {
        ModsLogf(L"  flush_settings_rows: trampoline alloc failed");
        return -1;
    }

    if (g_patched) return 0;
    if (patch_entry(SECTION_MAPPER, (void*)&sectmap_wrap) != 0 ||
        patch_entry(GET_ITEM,       (void*)&getitem_wrap) != 0 ||
        patch_entry(ROW_TAP,        (void*)&rowtap_wrap)  != 0) {
        ModsLogf(L"  flush_settings_rows: detour install failed (kerncore not ready?)");
        return -1;
    }
    g_patched = 1;
    ModsLogf(L"  flush_settings_rows: COMPLETE (%d sections, %d rows)",
             g_ext_count, g_pending_count);
    return 0;
}
