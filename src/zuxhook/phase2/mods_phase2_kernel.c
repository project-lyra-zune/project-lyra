/* mods_phase2_kernel.c - kernel-state capabilities (patch_bytes / kcall /
   require_kernel_value / read_kernel_va / require_back_ref_* / load_module /
   install_function_hook) + the ks_* hex/kernel helpers. Split from
   mods_phase2.c; dispatched via its table. */

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

static void ModStateReaperStart(void);   /* defined below; used in the servicesd Phase 2 init */
#include "mods_scene_suppress.h" /* ModSceneSuppressAdd (suppress_scene cap) */
#include "mods_phase2_internal.h"
#include "mod_scanner.h"      /* ModScanBuild - build the mods-tab row set on this
                                 boot thread (reusing our resolved ModSet) so the
                                 UI-thread data-source pull is a resident memory
                                 read, never a scan. */

#include <windows.h>

/* ── kernel-state caps (Phase 2, kerncore is ready by gemstone-side) ──
   These live in Phase 2 because kerncore is bootstrapped by
   nativeapp.exe's hax() + plant_helpers() at boot. By the time
   gemstone's ModsApplyPhase2 fires, kerncore is fully active and
   kreadu32 / kcall / kerncore_patch_code etc. work correctly. The
   same caps in Phase 1 (compositor boot) would see all-zero reads
   because the gadget isn't open yet. */

/* Pavo v4.5 NK proc-struct VA. Boot-stable.
   Used as `target_proc` for kerncore_patch_code() when patching kernel
   driver code (libnvusbf @ 0xC0880000, MtpClientDrvUsb @ 0xC0C50000, …). */
#define ZUXHOOK_NK_PROC  0x80BEE328u

/* Kernel-code range. Reads/writes via plain kerncore_kwriteb fault in
   PL1 because CE6 marks DLL code pages PL1-RO. Anything in this range
   takes the PT-flip + helper_v7 + ICIMVAU route (kerncore_patch_code).
   Outside this range (kernel heap 0xD0000000+, NK static data
   0x8000xxxx, kernel scratch 0x8001xxxx) plain kwrite works. */
#define ZUXHOOK_KCODE_LO 0xC0000000u
#define ZUXHOOK_KCODE_HI 0xD0000000u

/* Decode a hex-string token contents (s,n) into a fresh arena buffer.
   Returns 0 on success and sets *out_buf/*out_len. */
static int ks_decode_hex(ModsArena* arena, const char* s, int n,
                          unsigned char** out_buf, int* out_len) {
    int i, byte_len;
    unsigned char* b;
    if (n < 0 || (n & 1)) return -1;
    byte_len = n / 2;
    if (byte_len == 0) {
        *out_buf = NULL; *out_len = 0; return 0;
    }
    b = (unsigned char*)ModsArenaAlloc(arena, byte_len);
    if (!b) return -1;
    for (i = 0; i < byte_len; i++) {
        int hi, lo;
        char c1 = s[i * 2], c2 = s[i * 2 + 1];
        hi = (c1 >= '0' && c1 <= '9') ? c1 - '0'
           : (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10
           : (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10
           : -1;
        lo = (c2 >= '0' && c2 <= '9') ? c2 - '0'
           : (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10
           : (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10
           : -1;
        if (hi < 0 || lo < 0) return -1;
        b[i] = (unsigned char)((hi << 4) | lo);
    }
    *out_buf = b;
    *out_len = byte_len;
    return 0;
}

/* Read an arg's hex-string contents into a fresh arena buffer. Returns:
     0  = key present, bytes decoded into *out_buf/*out_len
     -2 = key absent (caller decides whether that's OK)
     -1 = present but bad / not a hex string */
static int ks_action_get_hex(const ModAction* a, ModsArena* arena,
                              const char* key,
                              unsigned char** out_buf, int* out_len) {
    int vi;
    const ModsJsonTok* t;
    const char* s;
    int n;
    *out_buf = NULL; *out_len = 0;
    vi = ModsJsonObjectFind(&a->mod->json, a->action_tok, key);
    if (vi < 0) return -2;
    t = &a->mod->json.toks[vi];
    if (t->type != MODS_JSON_STRING) return -1;
    s = a->mod->json.src + t->start;
    n = t->end - t->start;
    return ks_decode_hex(arena, s, n, out_buf, out_len);
}

/* Read current bytes at a kernel VA into a caller-supplied buffer. */
static void ks_kread_buf(DWORD va, unsigned char* dst, int n) {
    int i;
    for (i = 0; i < n; i++) dst[i] = kerncore_kreadb(va + (DWORD)i);
}

/* Write `n` bytes at `va`, auto-routing by VA range:
     0xC0000000..0xD0000000 → PT-flip + helper_v7 (kerncore_patch_code),
     chunked ≤64 B per call.
     Else → plain kerncore_kmemcpy (kernel heap, NK scratch, etc).
   Returns 0 on success, -1 on failure. */
static int ks_write_kernel(DWORD va, const unsigned char* bytes, int n) {
    if (va >= ZUXHOOK_KCODE_LO && va < ZUXHOOK_KCODE_HI) {
        int off = 0;
        while (off < n) {
            int chunk = n - off;
            if (chunk > 64) chunk = 64;
            if (kerncore_patch_code(ZUXHOOK_NK_PROC, va + (DWORD)off,
                                     bytes + off, chunk) != 0) {
                ModsLogf(L"    write_kernel(code) failed at 0x%08lx+0x%x",
                         (unsigned long)va, off);
                return -1;
            }
            off += chunk;
        }
        return 0;
    }
    kerncore_kmemcpy(va, bytes, (size_t)n);
    return 0;
}

/* Compare two byte buffers. Returns 1 if equal, 0 otherwise. */
static int ks_bytes_equal(const unsigned char* a, const unsigned char* b, int n) {
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Format short hex for logs (caller-supplied buf, max 9*2+1 chars). */
static void ks_fmt_hex(wchar_t* dst, int cap, const unsigned char* b, int n) {
    int i, p = 0;
    const wchar_t* hexc = L"0123456789abcdef";
    for (i = 0; i < n && p + 2 < cap; i++) {
        dst[p++] = hexc[b[i] >> 4];
        dst[p++] = hexc[b[i] & 0xF];
    }
    if (p < cap) dst[p] = 0;
}

/* ── cap: patch_bytes (kernel target) ───────────────────────────────── */
int apply_patch_bytes(ModAction* a, ModsArena* arena) {
    const char* target = NULL;
    DWORD va = 0;
    unsigned char *new_buf = NULL, *expected_buf = NULL, *already_buf = NULL;
    int new_len = 0, expected_len = 0, already_len = 0;
    int rh, vi;
    unsigned char* current;

    if (ModActionGetString(a, "target", arena, &target, NULL, 0) != 0 || !target) {
        ModsLogf(L"    patch_bytes: missing 'target'");
        return -1;
    }
    if (strcmp(target, "gemstone") == 0) {
        ModsLogf(L"    patch_bytes target=gemstone not yet supported in Phase 2");
        return -1;
    }
    if (strcmp(target, "kernel")   != 0) {
        ModsLogf(L"    patch_bytes: target must be 'kernel' or 'gemstone'");
        return -1;
    }
    if (ModActionGetU32Required(a, "va", &va) < 0) {
        ModsLogf(L"    patch_bytes: missing/bad 'va'");
        return -1;
    }
    rh = ks_action_get_hex(a, arena, "new", &new_buf, &new_len);
    if (rh < 0 || new_len <= 0) {
        ModsLogf(L"    patch_bytes: missing/bad 'new'");
        return -1;
    }
    (void)ks_action_get_hex(a, arena, "expected", &expected_buf, &expected_len);
    (void)ks_action_get_hex(a, arena, "if_already", &already_buf, &already_len);
    if (expected_len && expected_len != new_len) {
        ModsLogf(L"    patch_bytes: expected len %d != new len %d",
                 expected_len, new_len);
        return -1;
    }
    if (already_len && already_len != new_len) {
        ModsLogf(L"    patch_bytes: if_already len %d != new len %d",
                 already_len, new_len);
        return -1;
    }

    /* Apply relocations to `new` BEFORE comparison. Lets if_already
       reflect the post-relocation image (caller's expected/if_already
       can include placeholders that relocations overlay). */
    vi = ModsJsonObjectFind(&a->mod->json, a->action_tok, "relocations");
    if (vi >= 0) {
        const ModsJsonTok* rt = &a->mod->json.toks[vi];
        if (rt->type != MODS_JSON_ARRAY) {
            ModsLogf(L"    patch_bytes: relocations is not an array");
            return -1;
        }
        {
            int i;
            int n_relocs = rt->size;
            for (i = 0; i < n_relocs; i++) {
                int et = ModsJsonArrayAt(&a->mod->json, vi, i);
                int off_tok, val_tok;
                int off;
                DWORD val;
                if (et < 0 || a->mod->json.toks[et].type != MODS_JSON_OBJECT) {
                    ModsLogf(L"    patch_bytes: relocations[%d] not object", i);
                    return -1;
                }
                off_tok = ModsJsonObjectFind(&a->mod->json, et, "offset");
                val_tok = ModsJsonObjectFind(&a->mod->json, et, "value");
                if (off_tok < 0 || val_tok < 0) {
                    ModsLogf(L"    patch_bytes: relocations[%d] missing offset/value", i);
                    return -1;
                }
                if (a->mod->json.toks[off_tok].type != MODS_JSON_PRIMITIVE) {
                    ModsLogf(L"    patch_bytes: relocations[%d].offset not int", i);
                    return -1;
                }
                if (ModsJsonInt(&a->mod->json, off_tok, &off) < 0 || off < 0 ||
                    off + 4 > new_len) {
                    ModsLogf(L"    patch_bytes: relocations[%d].offset out of range", i);
                    return -1;
                }
                if (ModResolveTokU32(a->mod, val_tok, &val) < 0) {
                    ModsLogf(L"    patch_bytes: relocations[%d].value unresolved", i);
                    return -1;
                }
                new_buf[off + 0] = (unsigned char)(val & 0xFFu);
                new_buf[off + 1] = (unsigned char)((val >> 8) & 0xFFu);
                new_buf[off + 2] = (unsigned char)((val >> 16) & 0xFFu);
                new_buf[off + 3] = (unsigned char)((val >> 24) & 0xFFu);
            }
        }
    }

    current = (unsigned char*)ModsArenaAlloc(arena, new_len);
    if (!current) return -1;
    ks_kread_buf(va, current, new_len);

    if (already_len && ks_bytes_equal(current, already_buf, new_len)) {
        ModsLogf(L"    patch_bytes 0x%08lx: already %d B (no-op)",
                 (unsigned long)va, new_len);
        return 0;
    }
    if (expected_len && !ks_bytes_equal(current, expected_buf, new_len)) {
        wchar_t want[64], have[64];
        ks_fmt_hex(want, 64, expected_buf, new_len > 16 ? 16 : new_len);
        ks_fmt_hex(have, 64, current,      new_len > 16 ? 16 : new_len);
        ModsLogf(L"    patch_bytes 0x%08lx: expected %s found %s",
                 (unsigned long)va, want, have);
        return -1;
    }

    if (ks_write_kernel(va, new_buf, new_len) < 0) return -1;
    ModsLogf(L"    patch_bytes kernel 0x%08lx <- %d B",
             (unsigned long)va, new_len);
    return 0;
}

/* ── cap: kcall ──────────────────────────────────────────────────────── */
int apply_kcall(ModAction* a, ModsArena* arena) {
    DWORD fn_va = 0;
    DWORD args[6] = {0, 0, 0, 0, 0, 0};
    DWORD result;
    int vi, i, n_args;
    if (ModActionGetU32Required(a, "fn_va", &fn_va) < 0) {
        ModsLogf(L"    kcall: missing/bad 'fn_va'");
        return -1;
    }
    vi = ModsJsonObjectFind(&a->mod->json, a->action_tok, "args");
    if (vi >= 0) {
        const ModsJsonTok* at = &a->mod->json.toks[vi];
        if (at->type != MODS_JSON_ARRAY) {
            ModsLogf(L"    kcall: args not an array");
            return -1;
        }
        n_args = at->size;
        if (n_args > 6) {
            ModsLogf(L"    kcall: max 6 args (got %d)", n_args);
            return -1;
        }
        for (i = 0; i < n_args; i++) {
            int et = ModsJsonArrayAt(&a->mod->json, vi, i);
            if (et < 0) { ModsLogf(L"    kcall: args[%d] missing", i); return -1; }
            if (ModResolveTokU32(a->mod, et, &args[i]) < 0) {
                ModsLogf(L"    kcall: args[%d] unresolved", i);
                return -1;
            }
        }
    }
    result = kerncore_kcall(fn_va, args[0], args[1], args[2],
                             args[3], args[4], args[5]);
    ModsLogf(L"    kcall 0x%08lx -> 0x%08lx",
             (unsigned long)fn_va, (unsigned long)result);
    if (a->back_ref) {
        if (ModScopeSet(a->mod, arena, a->back_ref, result) < 0) return -1;
        ModsLogf(L"      back-ref $%S = 0x%08lx",
                 a->back_ref, (unsigned long)result);
    }
    return 0;
}

/* ── cap: require_kernel_value ─────────────────────────────────────── */
int apply_require_kernel_value(ModAction* a, ModsArena* arena) {
    DWORD va = 0;
    unsigned char* expected_buf = NULL;
    int expected_len = 0;
    unsigned char* current;
    if (ModActionGetU32Required(a, "va", &va) < 0) {
        ModsLogf(L"    require_kernel_value: missing/bad 'va'");
        return -1;
    }
    if (ks_action_get_hex(a, arena, "expected", &expected_buf, &expected_len) < 0
        || expected_len <= 0) {
        ModsLogf(L"    require_kernel_value: missing/bad 'expected'");
        return -1;
    }
    current = (unsigned char*)ModsArenaAlloc(arena, expected_len);
    if (!current) return -1;
    ks_kread_buf(va, current, expected_len);
    if (!ks_bytes_equal(current, expected_buf, expected_len)) {
        wchar_t want[64], have[64];
        ks_fmt_hex(want, 64, expected_buf, expected_len > 16 ? 16 : expected_len);
        ks_fmt_hex(have, 64, current,      expected_len > 16 ? 16 : expected_len);
        ModsLogf(L"    require_kernel_value 0x%08lx: expected %s found %s",
                 (unsigned long)va, want, have);
        return -1;
    }
    ModsLogf(L"    require_kernel_value 0x%08lx == %d B OK",
             (unsigned long)va, expected_len);
    return 0;
}

/* ── cap: read_kernel_va ───────────────────────────────────────────── */
int apply_read_kernel_va(ModAction* a, ModsArena* arena) {
    DWORD va = 0, value;
    if (ModActionGetU32Required(a, "va", &va) < 0) {
        ModsLogf(L"    read_kernel_va: missing/bad 'va'");
        return -1;
    }
    value = kerncore_kreadu32(va);
    ModsLogf(L"    read_kernel_va 0x%08lx -> 0x%08lx",
             (unsigned long)va, (unsigned long)value);
    if (a->back_ref) {
        if (ModScopeSet(a->mod, arena, a->back_ref, value) < 0) return -1;
        ModsLogf(L"      back-ref $%S = 0x%08lx",
                 a->back_ref, (unsigned long)value);
    }
    return 0;
}

/* ── cap: require_back_ref_range ───────────────────────────────────── */
int apply_require_back_ref_range(ModAction* a, ModsArena* arena) {
    DWORD v = 0, lo = 0, hi = 0;
    (void)arena;
    if (ModActionGetU32Required(a, "value", &v) < 0 ||
        ModActionGetU32Required(a, "min",   &lo) < 0 ||
        ModActionGetU32Required(a, "max",   &hi) < 0) {
        ModsLogf(L"    require_back_ref_range: missing/bad value/min/max");
        return -1;
    }
    if (!(v >= lo && v < hi)) {
        ModsLogf(L"    require_back_ref_range: 0x%08lx not in [0x%08lx, 0x%08lx)",
                 (unsigned long)v, (unsigned long)lo, (unsigned long)hi);
        return -1;
    }
    ModsLogf(L"    require_back_ref_range: 0x%08lx in [0x%08lx, 0x%08lx) OK",
             (unsigned long)v, (unsigned long)lo, (unsigned long)hi);
    return 0;
}

/* ── cap: require_back_ref_equal ───────────────────────────────────── */
int apply_require_back_ref_equal(ModAction* a, ModsArena* arena) {
    DWORD x = 0, y = 0;
    (void)arena;
    if (ModActionGetU32Required(a, "actual",   &x) < 0 ||
        ModActionGetU32Required(a, "expected", &y) < 0) {
        ModsLogf(L"    require_back_ref_equal: missing/bad actual/expected");
        return -1;
    }
    if (x != y) {
        ModsLogf(L"    require_back_ref_equal: 0x%08lx != 0x%08lx",
                 (unsigned long)x, (unsigned long)y);
        return -1;
    }
    ModsLogf(L"    require_back_ref_equal: 0x%08lx == 0x%08lx OK",
             (unsigned long)x, (unsigned long)y);
    return 0;
}

/* ── cap: load_module ─────────────────────────────────────────────────
   In-process twin of spawn_daemon. LoadLibrary a mod-provided DLL into the
   current host process and call a named `int init(void)` export. A mod that
   ships native code which must run inside a specific host's address space
   (a COM control, IAT hooks) declares this with target_proc set to that
   host; the consumption ABI (what init installs) is the mod's to define.
   CE GetProcAddress takes a wide symbol name. */
int apply_load_module(ModAction* a, ModsArena* arena) {
    wchar_t mod_path[MODS_MAX_PATH];
    const char* init_name = NULL;
    wchar_t init_w[128];
    HMODULE h;
    FARPROC fn;
    int k, rc;
    mod_path[0] = 0;
    if (ModActionGetString(a, "module_ref", arena, NULL, mod_path, MODS_MAX_PATH) != 1
        || mod_path[0] == 0) {
        ModsLogf(L"    load_module: missing or non-blob 'module_ref'");
        return -1;
    }
    if (ModActionGetString(a, "init", arena, &init_name, NULL, 0) != 0 || init_name == NULL) {
        ModsLogf(L"    load_module: missing 'init' export name");
        return -1;
    }
    h = LoadLibraryW(mod_path);
    if (h == NULL) {
        ModsLogf(L"    load_module: LoadLibrary failed err=0x%lx path=%s",
                 GetLastError(), mod_path);
        return -1;
    }
    for (k = 0; init_name[k] && k < 127; k++) init_w[k] = (wchar_t)(unsigned char)init_name[k];
    init_w[k] = 0;
    fn = GetProcAddress(h, init_w);
    if (fn == NULL) {
        ModsLogf(L"    load_module: export %S not found in %s", init_name, mod_path);
        FreeLibrary(h);
        return -1;
    }
    rc = ((int (*)(void))fn)();
    ModsLogf(L"    load_module: %s %S() rc=%d", mod_path, init_name, rc);
    if (rc != 0) {
        FreeLibrary(h);   /* init failed: the module did nothing useful, so unload it */
        return -1;
    }
    return 0;
}

/* ── cap: install_function_hook ───────────────────────────────────────
   Plant the canonical 8-byte ldr-pc inline hook at a kernel code site:
     site_va + 0   E5 1F F0 04   ldr pc, [pc, #-4]
     site_va + 4   <trampoline_va as u32 LE>
   Idempotent: if current 8 bytes already encode the same jump to the
   same trampoline_va, no-op success. Otherwise asserts the first
   `len(expected_prologue)` bytes at site match the caller's expected
   prologue, then writes via kerncore_patch_code (PT-flip). */
#define HOOK_LDR_PC_OPCODE  0xE51FF004u
int apply_install_function_hook(ModAction* a, ModsArena* arena) {
    DWORD site_va = 0, tramp_va = 0;
    unsigned char* prologue_buf = NULL;
    int prologue_len = 0;
    unsigned char current8[8];
    DWORD current_opc, current_dst;
    unsigned char jump_image[8];
    unsigned char* head;
    if (ModActionGetU32Required(a, "site_va",       &site_va)  < 0 ||
        ModActionGetU32Required(a, "trampoline_va", &tramp_va) < 0) {
        ModsLogf(L"    install_function_hook: missing site_va/trampoline_va");
        return -1;
    }
    if (ks_action_get_hex(a, arena, "expected_prologue",
                          &prologue_buf, &prologue_len) < 0
        || prologue_len < 4 || prologue_len > 64) {
        ModsLogf(L"    install_function_hook: bad expected_prologue (4..64 B)");
        return -1;
    }
    /* Idempotency: already a jump to the same tramp? */
    ks_kread_buf(site_va, current8, 8);
    current_opc = (DWORD)current8[0] | ((DWORD)current8[1] << 8)
                | ((DWORD)current8[2] << 16) | ((DWORD)current8[3] << 24);
    current_dst = (DWORD)current8[4] | ((DWORD)current8[5] << 8)
                | ((DWORD)current8[6] << 16) | ((DWORD)current8[7] << 24);
    if (current_opc == HOOK_LDR_PC_OPCODE && current_dst == tramp_va) {
        ModsLogf(L"    install_function_hook 0x%08lx -> 0x%08lx (already)",
                 (unsigned long)site_va, (unsigned long)tramp_va);
        return 0;
    }
    /* Safety: current first N bytes must match expected_prologue. */
    head = (unsigned char*)ModsArenaAlloc(arena, prologue_len);
    if (!head) return -1;
    ks_kread_buf(site_va, head, prologue_len);
    if (!ks_bytes_equal(head, prologue_buf, prologue_len)) {
        wchar_t want[64], have[64];
        ks_fmt_hex(want, 64, prologue_buf,
                   prologue_len > 16 ? 16 : prologue_len);
        ks_fmt_hex(have, 64, head, prologue_len > 16 ? 16 : prologue_len);
        ModsLogf(L"    install_function_hook 0x%08lx: prologue %s != %s",
                 (unsigned long)site_va, want, have);
        return -1;
    }
    /* Write 8-byte jump via PT-flip (site is in kernel code). */
    jump_image[0] = (unsigned char)(HOOK_LDR_PC_OPCODE & 0xFFu);
    jump_image[1] = (unsigned char)((HOOK_LDR_PC_OPCODE >> 8) & 0xFFu);
    jump_image[2] = (unsigned char)((HOOK_LDR_PC_OPCODE >> 16) & 0xFFu);
    jump_image[3] = (unsigned char)((HOOK_LDR_PC_OPCODE >> 24) & 0xFFu);
    jump_image[4] = (unsigned char)(tramp_va & 0xFFu);
    jump_image[5] = (unsigned char)((tramp_va >> 8) & 0xFFu);
    jump_image[6] = (unsigned char)((tramp_va >> 16) & 0xFFu);
    jump_image[7] = (unsigned char)((tramp_va >> 24) & 0xFFu);
    if (ks_write_kernel(site_va, jump_image, 8) < 0) {
        ModsLogf(L"    install_function_hook 0x%08lx: write failed",
                 (unsigned long)site_va);
        return -1;
    }
    ModsLogf(L"    install_function_hook 0x%08lx -> 0x%08lx (ldr pc)",
             (unsigned long)site_va, (unsigned long)tramp_va);
    return 0;
}
