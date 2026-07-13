/* mods_phase2_xui.c - Layer-5 XUI class-blob loader: mod-shipped factory+vtable
   planting, class-reloc fixups, visual (.xur) registration, and XuiRegisterClass
   (register_visuals / register_xui_class). Split from mods_phase2.c. */

#include "mods_phase2.h"
#include "mods.h"
#include "mods_log.h"
#include "mods_arena.h"
#include "mods_manifest.h"
#include "mods_json.h"
#include "kerncore.h"
#include "mods_xui_registry.h"
#include "mods_icon_host.h"   /* ModsHudMenuButtonTap (HUD menu-button trampoline target) */
#include "mods_wifi_awake.h"  /* WifiAwake_EnsureActive (subsystem) + WifiAwake_Notify */
#include "mods_volume_state.h" /* VolumeStateInstall (subsystem activator) */
#include "mods_settings.h"    /* ModSettingsLoad (restore persisted toggle state) */
#include "mods_toggles.h"     /* register_setting declared-settings registry */
#include "mods_icons.h"       /* add_status_icon registry */
#include "mods_state_block.h" /* ModStateSeed (boot default), ModStateReapDeadOwners */
#include "mods_state_event.h" /* ModStateEventPublish (status reaper wakes the UI) */
#include "mods_scene_suppress.h" /* ModSceneSuppressAdd (suppress_scene cap) */
#include "mods_phase2_internal.h"
#include <windows.h>
#include <string.h>

/* gemstone v4.5 - class-registration shared constants. Every built-in
   GemXxxScene's 44-byte descriptor uses these slots. */
#define GEMBASESCENE_PTR             0x00011be0u  /* L"GemBaseScene" */
#define GEMLIBRARYBASESCENE_PTR      0x00011b6cu  /* L"GemLibraryBaseScene" */
#define GEMNOWPLAYINGSCENE_PTR       0x00011bb8u  /* L"GemNowPlayingScene" */
#define GEMLIBRARYLISTSCENE_PTR      0x00011b44u  /* L"GemLibraryListScene" */
#define XUICONTROL_PTR               0x000142d4u  /* L"XuiControl" - parent for element-controller classes (icons) */
#define DEFAULT_DESC_FIELD_14        0x0002acf4u  /* shared descriptor destructor */
#define DEFAULT_DESC_FIELD_1C        0x0002a7acu  /* shared descriptor finalizer */

/* servicesd (zhud_serv.dll) equivalents for an XuiControl-parented element
   controller; descriptor helpers are generic vtable-invoke trampolines, the
   parent name is zhud's own L"XuiControl" wstring. */
#define ZHUD_XUICONTROL_PTR          0x419b1fb0u  /* L"XuiControl" in zhud_serv .rdata */
#define ZHUD_HUDACTIVEBASESCENE_PTR  0x419b1454u  /* L"HudActiveBaseScene" - the interactive HUD scene base (HudNetworkListScene's parent) */
#define ZHUD_DESC_FIELD_14           0x419b9e24u  /* zhud shared descriptor destructor */
#define ZHUD_DESC_FIELD_1C           0x419b9978u  /* zhud shared descriptor finalizer */

typedef HRESULT (*XuiRegisterClassFn)(void* arg0, void* desc);

/* xuidll!XuiLoadVisualFromBinary - confirmed signature from XEDK headers.
   Loads a .xur containing visual definitions; visuals are added to a
   per-set registry keyed by `szResourcePath`. The URI is resolved via
   the registered resource transport whose scheme matches the URI prefix
   (xuidll registers `file://` itself at init).

   IMPORTANT: szPrefix MUST NOT be NULL on Zune. When NULL, the function
   tail-calls XuiVisualSetBasePath(path, NULL), which makes our loaded
   set the engine's DEFAULT visual set, replacing skin.xur and breaking
   every existing visual reference. Pass L"" or a non-empty prefix to
   suppress that global write; the visual definitions still load into
   the per-set registry. (RE'd at xuidll 0x418358f8 cmp r4,#0; bne.) */
typedef HRESULT (*XuiLoadVisualFromBinaryFn)(LPCWSTR szResourcePath,
                                              LPCWSTR szPrefix);
typedef HRESULT (*XuiVisualFindFn)(LPCWSTR szVisualId, LPCWSTR szPrefix,
                                    void** ppObjData, void** ppObjOwner);

static XuiLoadVisualFromBinaryFn g_XuiLoadVisualFromBinary = NULL;
static XuiVisualFindFn           g_XuiVisualFind           = NULL;

/* ── class-blob loader (Layer 5 mod-shipped factory + vtable) ────────── */

typedef enum {
    CFX_IMM8 = 0,
    CFX_BL   = 1,
    CFX_WORD = 2
} ClassFixupKind;

typedef enum {
    CFX_ABS           = 0,    /* target is an absolute VA (host pre-resolved extern) */
    CFX_INTERN        = 1,    /* target is a byte offset within the planted blob */
    CFX_VALUE         = 2,    /* target is a literal value (imm8 only) */
    CFX_EXTERN_MODULE = 3     /* resolve via GetModuleHandleW + GetProcAddress */
} ClassFixupTargetKind;

typedef struct {
    int                    at;
    ClassFixupKind         kind;
    ClassFixupTargetKind   target_kind;
    DWORD                  target;       /* ABS / INTERN / VALUE */
    const wchar_t*         module_w;     /* EXTERN_MODULE only - arena-allocated */
    const wchar_t*         symbol_w;     /* EXTERN_MODULE only - arena-allocated */
} ClassFixup;

/* Read a file at `path` into the arena. Returns 0 on success. */
static int read_blob_file(const wchar_t* path, ModsArena* arena,
                            const unsigned char** out_bytes, DWORD* out_len) {
    HANDLE h;
    DWORD size, got = 0;
    unsigned char* buf;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ModsLogf(L"      blob read: cannot open %s (err=0x%lx)",
                 path, GetLastError());
        return -1;
    }
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) {
        CloseHandle(h);
        ModsLogf(L"      blob read: bad size %lu for %s", size, path);
        return -1;
    }
    buf = (unsigned char*)ModsArenaAlloc(arena, size);
    if (!buf) { CloseHandle(h); return -1; }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        CloseHandle(h);
        ModsLogf(L"      blob read: short %lu/%lu for %s", got, size, path);
        return -1;
    }
    CloseHandle(h);
    *out_bytes = buf;
    *out_len   = size;
    return 0;
}

/* Compare a JSON string token's decoded content against a literal. */
static int json_str_is(const ModsJson* j, int tok_idx, const char* s) {
    return ModsJsonStrEq(j, tok_idx, s);
}

/* ASCII → arena-allocated UTF-16LE NUL-terminated wstring. Returns NULL
   on OOM or if any byte is non-ASCII. Used for module + symbol names
   in extern_module fixups (both are ASCII identifiers / DLL names). */
static const wchar_t* arena_ascii_to_w(ModsArena* arena,
                                        const ModsJson* j, int tok_idx) {
    const char* utf8;
    int n, i;
    wchar_t* out;
    if (ModsJsonTypeOf(j, tok_idx) != MODS_JSON_STRING) return NULL;
    utf8 = ModsJsonStrdup(arena, j, tok_idx);
    if (!utf8) return NULL;
    n = (int)strlen(utf8);
    if (n > 127) return NULL;       /* sanity cap - DLL/symbol names */
    out = (wchar_t*)ModsArenaAlloc(arena, (n + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if (c >= 0x80) return NULL;   /* extern names must be ASCII */
        out[i] = (wchar_t)c;
    }
    out[n] = 0;
    return out;
}

/* Parse one fixup object. Returns 0 on success. Arena needed only when
   the fixup is `extern_module` (allocates wide-string copies of module
   + symbol names). */
static int parse_one_fixup(const ModsJson* j, int obj_idx,
                            ModsArena* arena, ClassFixup* out) {
    int at_idx, kind_idx, abs_idx, intern_idx, value_idx, em_idx;
    int v = 0;

    at_idx = ModsJsonObjectFind(j, obj_idx, "at");
    if (at_idx < 0 || ModsJsonInt(j, at_idx, &v) < 0) return -1;
    out->at = v;
    out->module_w = NULL;
    out->symbol_w = NULL;

    kind_idx = ModsJsonObjectFind(j, obj_idx, "kind");
    if (kind_idx < 0 || ModsJsonTypeOf(j, kind_idx) != MODS_JSON_STRING)
        return -1;
    if (json_str_is(j, kind_idx, "imm8"))      out->kind = CFX_IMM8;
    else if (json_str_is(j, kind_idx, "bl"))   out->kind = CFX_BL;
    else if (json_str_is(j, kind_idx, "word")) out->kind = CFX_WORD;
    else return -1;

    abs_idx    = ModsJsonObjectFind(j, obj_idx, "abs");
    intern_idx = ModsJsonObjectFind(j, obj_idx, "intern");
    value_idx  = ModsJsonObjectFind(j, obj_idx, "value");
    em_idx     = ModsJsonObjectFind(j, obj_idx, "extern_module");

    if (out->kind == CFX_IMM8) {
        if (value_idx < 0) return -1;
        if (ModsJsonInt(j, value_idx, &v) < 0) return -1;
        if (v < 0 || v > 0xff) return -1;
        out->target_kind = CFX_VALUE;
        out->target = (DWORD)v;
    } else if (em_idx >= 0) {
        int mod_tok, sym_tok;
        if (ModsJsonTypeOf(j, em_idx) != MODS_JSON_OBJECT) return -1;
        mod_tok = ModsJsonObjectFind(j, em_idx, "module");
        sym_tok = ModsJsonObjectFind(j, em_idx, "symbol");
        if (mod_tok < 0 || sym_tok < 0) return -1;
        out->module_w = arena_ascii_to_w(arena, j, mod_tok);
        out->symbol_w = arena_ascii_to_w(arena, j, sym_tok);
        if (!out->module_w || !out->symbol_w) return -1;
        out->target_kind = CFX_EXTERN_MODULE;
        out->target = 0;
    } else if (abs_idx >= 0) {
        if (ModsJsonInt(j, abs_idx, &v) < 0) return -1;
        out->target_kind = CFX_ABS;
        out->target = (DWORD)v;
    } else if (intern_idx >= 0) {
        if (ModsJsonInt(j, intern_idx, &v) < 0) return -1;
        out->target_kind = CFX_INTERN;
        out->target = (DWORD)v;
    } else {
        return -1;
    }
    return 0;
}

/* Parse class.reloc.json into a ClassFixup array. On success:
     *out_factory_offset = blob byte-offset of the factory entry
     *out_fixups, *out_nfixups = arena-allocated array */
static int parse_class_reloc(ModsArena* arena, const char* src, DWORD srclen,
                              int* out_factory_offset,
                              ClassFixup** out_fixups, int* out_nfixups) {
    ModsJson j;
    int root, fo_idx, fx_arr_idx, i, fv;
    int n;
    ClassFixup* fxs;

    if (ModsJsonParse(arena, src, srclen, &j) < 0) {
        ModsLogf(L"      reloc parse: JSON parse failed");
        return -1;
    }
    if (j.ntoks == 0 || j.toks[0].type != MODS_JSON_OBJECT) {
        ModsLogf(L"      reloc parse: root not object");
        return -1;
    }
    root = 0;

    fo_idx = ModsJsonObjectFind(&j, root, "factory_offset");
    if (fo_idx < 0 || ModsJsonInt(&j, fo_idx, &fv) < 0) {
        ModsLogf(L"      reloc parse: missing/bad factory_offset");
        return -1;
    }
    *out_factory_offset = fv;

    fx_arr_idx = ModsJsonObjectFind(&j, root, "fixups");
    if (fx_arr_idx < 0 || j.toks[fx_arr_idx].type != MODS_JSON_ARRAY) {
        ModsLogf(L"      reloc parse: missing/bad fixups array");
        return -1;
    }
    n = j.toks[fx_arr_idx].size;
    if (n < 0 || n > 4096) {
        ModsLogf(L"      reloc parse: unreasonable fixup count %d", n);
        return -1;
    }
    fxs = (ClassFixup*)ModsArenaAlloc(arena, (n > 0 ? n : 1) * sizeof(ClassFixup));
    if (!fxs) return -1;
    for (i = 0; i < n; i++) {
        int it = ModsJsonArrayAt(&j, fx_arr_idx, i);
        if (it < 0 || j.toks[it].type != MODS_JSON_OBJECT) {
            ModsLogf(L"      reloc parse: fixup[%d] not object", i);
            return -1;
        }
        if (parse_one_fixup(&j, it, arena, &fxs[i]) < 0) {
            ModsLogf(L"      reloc parse: fixup[%d] malformed", i);
            return -1;
        }
    }
    *out_fixups = fxs;
    *out_nfixups = n;
    return 0;
}

/* Apply one fixup against bytes already planted at plant_va. */
static int apply_one_fixup(DWORD plant_va, const ClassFixup* fx) {
    DWORD addr = plant_va + (DWORD)fx->at;
    DWORD target;
    DWORD rel, off24, instr;

    switch (fx->kind) {
    case CFX_IMM8:
        if (fx->target_kind != CFX_VALUE) return -1;
        *(volatile unsigned char*)addr = (unsigned char)(fx->target & 0xff);
        return 0;

    case CFX_BL:
    case CFX_WORD:
        if (fx->target_kind == CFX_ABS) {
            target = fx->target;
        } else if (fx->target_kind == CFX_INTERN) {
            target = plant_va + fx->target;
        } else if (fx->target_kind == CFX_EXTERN_MODULE) {
            HMODULE mod = GetModuleHandleW(fx->module_w);
            FARPROC proc;
            if (mod == NULL) {
                ModsLogf(L"      extern_module: GetModuleHandle(%s) → NULL",
                         fx->module_w);
                return -1;
            }
            proc = GetProcAddress(mod, fx->symbol_w);
            if (proc == NULL) {
                ModsLogf(L"      extern_module: GetProcAddress(%s, %s) → NULL",
                         fx->module_w, fx->symbol_w);
                return -1;
            }
            target = (DWORD)proc;
        } else {
            return -1;
        }

        if (fx->kind == CFX_WORD) {
            *(volatile DWORD*)addr = target;
            return 0;
        }
        /* bl encoding: rel must be 4-aligned, fits signed 26-bit range
           (i.e. low 24 bits after >>2). */
        rel = target - (addr + 8);
        if (rel & 3) return -1;
        off24 = (rel >> 2) & 0xffffffu;
        instr = *(volatile DWORD*)addr;
        instr = (instr & 0xff000000u) | off24;
        *(volatile DWORD*)addr = instr;
        return 0;
    }
    return -1;
}

/* SEH-wrap the whole fixup pass; a single bad fixup shouldn't crash
   gemstone. */
static int apply_class_fixups(DWORD plant_va,
                                const ClassFixup* fxs, int n) {
    int i, ok = 0;
    __try {
        for (i = 0; i < n; i++) {
            if (apply_one_fixup(plant_va, &fxs[i]) < 0) {
                ModsLogf(L"      fixup[%d] @0x%x kind=%d failed",
                         i, fxs[i].at, fxs[i].kind);
                ok = -1;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ModsLogf(L"      fixup pass: access violation");
        return -1;
    }
    return ok;
}

/* Load + plant a class blob from the action's class_ref/reloc_ref.
   On success *out_factory_va holds the planted factory entry's VA.
   Both class_ref and reloc_ref are required. register_xui_class is
   defined as "ship a real class blob"; there is no stub fallback. */
static int plant_class_blob(ModAction* a, ModsArena* arena,
                              DWORD* out_factory_va) {
    wchar_t class_path[MODS_MAX_PATH];
    wchar_t reloc_path[MODS_MAX_PATH];
    const unsigned char* class_bytes;
    const unsigned char* reloc_bytes;
    DWORD class_len = 0, reloc_len = 0;
    int factory_offset = 0;
    ClassFixup* fxs = NULL;
    int nfx = 0;
    DWORD plant_va;
    int copy_ok = -1;

    if (ModActionGetString(a, "class_ref", arena, NULL,
                            class_path, MODS_MAX_PATH) != 1) {
        ModsLogf(L"      class blob: class_ref required");
        return -1;
    }
    if (ModActionGetString(a, "reloc_ref", arena, NULL,
                            reloc_path, MODS_MAX_PATH) != 1) {
        ModsLogf(L"      class blob: reloc_ref required");
        return -1;
    }

    if (read_blob_file(class_path, arena, &class_bytes, &class_len) < 0)
        return -1;
    if (read_blob_file(reloc_path, arena, &reloc_bytes, &reloc_len) < 0)
        return -1;

    if (parse_class_reloc(arena, (const char*)reloc_bytes, reloc_len,
                           &factory_offset, &fxs, &nfx) < 0)
        return -1;

    if (factory_offset < 0 || (DWORD)factory_offset >= class_len) {
        ModsLogf(L"      class blob: factory_offset %d out of range (len=%lu)",
                 factory_offset, class_len);
        return -1;
    }

    plant_va = scratch_alloc((int)class_len);
    if (!plant_va) {
        ModsLogf(L"      class blob: scratch exhausted (%lu bytes requested)",
                 class_len);
        return -1;
    }
    __try {
        memcpy((void*)plant_va, class_bytes, class_len);
        copy_ok = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        copy_ok = -1;
    }
    if (copy_ok < 0) {
        ModsLogf(L"      class blob: memcpy to 0x%08x faulted", plant_va);
        return -1;
    }

    if (apply_class_fixups(plant_va, fxs, nfx) < 0) {
        ModsLogf(L"      class blob: fixup pass failed");
        return -1;
    }

    /* Force I-cache invalidate so the freshly-planted code is fetched
       fresh on first call into the factory. */
    FlushInstructionCache(GetCurrentProcess(), (void*)plant_va, class_len);

    *out_factory_va = plant_va + (DWORD)factory_offset;
    ModsLogf(L"      class blob: planted %lu bytes @0x%08x; "
             L"factory @0x%08x (%d fixups applied)",
             class_len, plant_va, *out_factory_va, nfx);
    return 0;
}

/* ── Layer 5: register_visuals ───────────────────────────────────────── */

static int resolve_xuidll_visual_imports(void) {
    HMODULE xuidll;
    if (g_XuiLoadVisualFromBinary != NULL && g_XuiVisualFind != NULL)
        return 0;
    xuidll = GetModuleHandleW(L"xuidll.dll");
    if (xuidll == NULL) {
        ModsLogf(L"    register_visuals: xuidll.dll not loaded");
        return -1;
    }
    g_XuiLoadVisualFromBinary = (XuiLoadVisualFromBinaryFn)GetProcAddress(
        xuidll, L"XuiLoadVisualFromBinary");
    g_XuiVisualFind = (XuiVisualFindFn)GetProcAddress(
        xuidll, L"XuiVisualFind");
    ModsLogf(L"    register_visuals: xuidll exports load=0x%08x find=0x%08x",
             (DWORD)g_XuiLoadVisualFromBinary,
             (DWORD)g_XuiVisualFind);
    if (g_XuiLoadVisualFromBinary == NULL) return -1;
    return 0;
}

int apply_register_visuals(ModAction* a, ModsArena* arena) {
    wchar_t blob_path[MODS_MAX_PATH];
    wchar_t uri[MODS_MAX_PATH + 16];
    const char* verify_id = NULL;
    HRESULT hr;

    if (resolve_xuidll_visual_imports() < 0) return -1;

    if (ModActionGetString(a, "content_blob_ref", arena, NULL,
                            blob_path, MODS_MAX_PATH) != 1) {
        ModsLogf(L"    register_visuals: content_blob_ref required");
        return -1;
    }

    /* URI format: `file://` + absolute device path. Matches gemstone's
       own internal templates (file://%s, file://\\Windows\\%s#%s). */
    _snwprintf(uri, sizeof(uri)/sizeof(uri[0]) - 1, L"file://%s", blob_path);
    uri[sizeof(uri)/sizeof(uri[0]) - 1] = 0;

    ModsLogf(L"    register_visuals: %s", uri);

    /* szPrefix MUST be non-NULL: see typedef comment. L"" is the minimal
       non-NULL value; visuals still register into the per-set registry. */
    hr = 0x80000000;
    __try {
        hr = g_XuiLoadVisualFromBinary(uri, L"");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ModsLogf(L"    register_visuals: XuiLoadVisualFromBinary call faulted");
        return -1;
    }

    if (FAILED(hr)) {
        ModsLogf(L"    register_visuals: HRESULT 0x%08x", hr);
        return -1;
    }
    ModsLogf(L"    register_visuals: loaded OK (hr=0x%08x)", hr);

    /* Optional registry-verify: if the manifest declared a visual id we
       expect to see, look it up to prove the registry actually got the
       new entry. NULL ppVisual is tolerated by some xuidll variants;
       pass a real out-param. */
    if (ModActionGetString(a, "verify_visual_id", arena, &verify_id,
                            NULL, 0) == 0 && verify_id != NULL
        && g_XuiVisualFind != NULL) {
        wchar_t verify_w[64];
        int n = (int)strlen(verify_id);
        int i;
        void* obj_data  = NULL;
        void* obj_owner = NULL;
        HRESULT fhr = 0x80000000;
        if (n + 1 > (int)(sizeof(verify_w)/sizeof(verify_w[0]))) {
            ModsLogf(L"      verify: id too long");
            return 0;
        }
        for (i = 0; i < n; i++) verify_w[i] = (wchar_t)(unsigned char)verify_id[i];
        verify_w[n] = 0;
        /* XuiVisualFind takes 4 args: (id, prefix, **objData, **objOwner).
           Pass our load prefix to namespace the lookup; if visual was
           loaded into the per-set registry, this should succeed. */
        __try {
            fhr = g_XuiVisualFind(verify_w, L"", &obj_data, &obj_owner);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ModsLogf(L"      verify: XuiVisualFind faulted");
            return 0;
        }
        ModsLogf(L"      verify: XuiVisualFind(%S) hr=0x%08x data=0x%08x owner=0x%08x",
                 verify_id, fhr, (DWORD)obj_data, (DWORD)obj_owner);
    }

    return 0;
}

/* ── Layer 5: register_xui_class ─────────────────────────────────────── */

int apply_register_xui_class(ModAction* a, ModsArena* arena) {
    const char* name = NULL;
    const char* parent = NULL;
    wchar_t name_w[64];
    int name_chars, i, is_sd = 0;
    DWORD name_va, desc_va;
    DWORD desc[11];
    XuiRegisterClassFn fn;
    HMODULE hxui;
    HRESULT hr;
    int already;
    DWORD factory_va = 0;
    DWORD parent_ptr = GEMBASESCENE_PTR;   /* resolved below */

    /* Host is runtime-owned: action_target_matches already routed this action to
       the right process via its target_proc; the apply derives its host from the
       actually-loaded module (servicesd hosts zhud_serv) so there's one source of
       truth and the class VAs below can't disagree with the routing. */
    is_sd = (GetModuleHandleW(L"zhud_serv.dll") != NULL);
    if (ModActionGetString(a, "name", arena, &name, NULL, 0) != 0) return -1;
    (void)ModActionGetString(a, "parent_name", arena, &parent, NULL, 0);
    /* Resolve parent_name to its built-in name-string VA in the host's
       .rdata. XuiRegisterClass looks up the parent class by name internally.
       servicesd (zhud_serv) hosts only the XuiControl base relevant to icons;
       the Gem* scene parents exist in gemstone only. */
    if (parent) {
        if (strcmp(parent, "XuiControl") == 0) {
            parent_ptr = is_sd ? ZHUD_XUICONTROL_PTR : XUICONTROL_PTR;
        } else if (is_sd && strcmp(parent, "HudActiveBaseScene") == 0) {
            parent_ptr = ZHUD_HUDACTIVEBASESCENE_PTR;   /* interactive HUD scene base (list scenes) */
        } else if (is_sd) {
            ModsLogf(L"    register_xui_class: servicesd supports parent "
                     L"XuiControl/HudActiveBaseScene only (got %S)", parent);
            return -1;
        }
        else if (strcmp(parent, "GemBaseScene")        == 0) parent_ptr = GEMBASESCENE_PTR;
        else if (strcmp(parent, "GemLibraryBaseScene") == 0) parent_ptr = GEMLIBRARYBASESCENE_PTR;
        else if (strcmp(parent, "GemNowPlayingScene")  == 0) parent_ptr = GEMNOWPLAYINGSCENE_PTR;
        else if (strcmp(parent, "GemLibraryListScene") == 0) parent_ptr = GEMLIBRARYLISTSCENE_PTR;
        else {
            ModsLogf(L"    register_xui_class: parent_name=%S not supported "
                     L"(GemBaseScene/GemLibraryBaseScene/GemNowPlayingScene/"
                     L"GemLibraryListScene/XuiControl)", parent);
            return -1;
        }
    }

    /* ASCII → UTF-16LE in name_w, including trailing NUL */
    name_chars = (int)strlen(name);
    if (name_chars + 1 > (int)(sizeof(name_w)/sizeof(name_w[0]))) return -1;
    for (i = 0; i < name_chars; i++) name_w[i] = (wchar_t)(unsigned char)name[i];
    name_w[name_chars] = 0;

    /* Idempotency: xuidll's class registry persists across gemstone
       restarts (xuidll lives in compositor). If our class is already
       there, calling XuiRegisterClass would return 0x80300005 - skip. */
    already = walk_registry(name_w, NULL);
    if (already == 1) {
        ModsLogf(L"    register_xui_class: %S already registered - skipping", name);
        return 0;
    }
    if (already < 0) {
        ModsLogf(L"    register_xui_class: registry walk faulted");
        return -1;
    }

    /* Class-blob loader: every register_xui_class action must ship a
       real factory + vtable. plant_class_blob plants the bytes into
       scratch and applies fixups; the planted factory VA fills the
       descriptor's +0x18 slot below. */
    if (plant_class_blob(a, arena, &factory_va) < 0) {
        ModsLogf(L"    register_xui_class: %S - class blob load failed", name);
        return -1;
    }
    ModsLogf(L"    register_xui_class: %S - factory @0x%08x",
             name, factory_va);

    /* Allocate scratch for the name wstring and the 44-byte descriptor. */
    name_va = scratch_alloc((name_chars + 1) * 2);
    desc_va = scratch_alloc(sizeof(desc));
    if (!name_va || !desc_va) {
        ModsLogf(L"    register_xui_class: scratch exhausted");
        return -1;
    }
    if (scratch_write(name_va, name_w, (name_chars + 1) * 2) < 0) {
        ModsLogf(L"    register_xui_class: write name @0x%08x faulted", name_va);
        return -1;
    }

    desc[0]  = 0;
    desc[1]  = name_va;
    desc[2]  = parent_ptr;
    desc[3]  = 0;
    desc[4]  = 0;
    desc[5]  = is_sd ? ZHUD_DESC_FIELD_14 : DEFAULT_DESC_FIELD_14;
    desc[6]  = factory_va;
    desc[7]  = is_sd ? ZHUD_DESC_FIELD_1C : DEFAULT_DESC_FIELD_1C;
    desc[8]  = 0;
    desc[9]  = 0;
    desc[10] = 0;
    if (scratch_write(desc_va, desc, sizeof(desc)) < 0) {
        ModsLogf(L"    register_xui_class: write descriptor @0x%08x faulted", desc_va);
        return -1;
    }

    /* Resolve XuiRegisterClass from xuidll directly (XIP-fixed export,
       identical in gemstone and servicesd) rather than a per-host thunk.
       Call shape: XuiRegisterClass(&desc[1], &desc[0]). */
    hxui = GetModuleHandleW(L"xuidll.dll");
    fn = hxui ? (XuiRegisterClassFn)GetProcAddress(hxui, L"XuiRegisterClass") : NULL;
    if (fn == NULL) {
        ModsLogf(L"    register_xui_class: XuiRegisterClass unresolved (xuidll=%p)", hxui);
        return -1;
    }
    hr = -1;
    __try {
        hr = fn((void*)(desc_va + 4), (void*)desc_va);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ModsLogf(L"    register_xui_class: XuiRegisterClass call faulted");
        return -1;
    }
    if (hr != 0) {
        ModsLogf(L"    register_xui_class: HRESULT 0x%08x", hr);
        return -1;
    }
    ModsLogf(L"    register_xui_class: %S registered (desc_va=0x%08x)", name, desc_va);
    return 0;
}
