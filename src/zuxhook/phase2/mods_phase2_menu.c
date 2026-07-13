/* mods_phase2_menu.c - Layer-6 menu-entry injection (inject_menu_entry) +
   the pending-menu table it fills and flush_menu_entries() that the core
   worker drains at end of Phase 2. Split from mods_phase2.c. */

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

/* gemstone v4.5 - menu-entry chain trampoline + populate-insert site. */
#define TRAMP_BASE               0x000F4000u  /* runtime trampoline area */
#define POPULATE_INSERT          0x00061198u  /* `add r1, r6, #0x118` */
#define POPULATE_INSERT_ORIG     0xe2861f46u  /* the instruction it replaces */
#define POPULATE_RESUME          0x0006119Cu  /* `mov r0, r5` next instr */
#define ADD_ITEM_FN              0x00060F04u  /* AddItem (public wrapper) */

/* Name-based scene navigation. Mod scenes are reached by URI, not by
   scene-id table lookup; the original id-based path required a 3-patch
   XIP extension of gemstone's scene-id table (pool word + 2 cmp bounds
   at 0x2ac98 / 0x2abe4 / 0x2abf0). That 3-patch sequence had an
   inherent race that no ordering could resolve: empirically disabling
   it restored stock navigation while breaking mod navigation as
   expected, but no single-signal poll could close the
   race against gemstone's continuous early-init scene-id lookups.

   The simpler architecture: never touch the scene-id table. Mod menu
   handlers and inter-mod-scene navigation call XuiSceneCreate directly
   (xuidll, fixed VA 0x418358d0) + scene_navigate_wrapper (gemstone,
   0x1e5d8). The only XIP patch we still need is POPULATE_INSERT for
   the menu tile insertion. */
#define CURRENT_CTX_GLOBAL       0x00097300u
#define SCENE_NAVIGATE_WRAPPER   0x0001E5D8u
#define XUI_SCENE_CREATE_VA      0x418358D0u  /* xuidll at fixed image base */

typedef struct {
    int    label_id;
    DWORD  scene_name_va;   /* scratch VA of L"<name>.xur" wstring */
    int    section_id;
    int    icon_id;
    int    flags;
} PendingMenuEntry;

#define MAX_PENDING_MENU 16
static PendingMenuEntry g_pending_menu[MAX_PENDING_MENU];
static int              g_pending_menu_count = 0;

/* ── Layer 6: inject_menu_entry ──────────────────────────────────────── */

int apply_inject_menu_entry(ModAction* a, ModsArena* arena) {
    int label_id;
    int section_id, icon_id, flags;
    const char* scene_name = NULL;
    wchar_t name_w[64];
    DWORD name_va;
    int name_chars, i;

    if (g_pending_menu_count >= MAX_PENDING_MENU) {
        ModsLogf(L"    inject_menu_entry: pending list full (%d)",
                 MAX_PENDING_MENU);
        return -1;
    }

    if (ModActionGetInt(a, "label_id_or_ref", -1, &label_id) < 0 || label_id < 0) {
        if (ModActionGetInt(a, "label_id", -1, &label_id) < 0 || label_id < 0) {
            ModsLogf(L"    inject_menu_entry: missing label_id");
            return -1;
        }
    }
    if (ModActionGetString(a, "scene_name", arena, &scene_name, NULL, 0) != 0
        || scene_name == NULL) {
        ModsLogf(L"    inject_menu_entry: missing scene_name");
        return -1;
    }
    (void)ModActionGetInt(a, "section_id", -2, &section_id);
    (void)ModActionGetInt(a, "icon_id", 0, &icon_id);
    (void)ModActionGetInt(a, "flags", 1, &flags);

    /* Plant the wchar_t URI tail (e.g. L"Mods.xur") in scratch; the
       handler's literal pool will point at this. We append ".xur" so
       the manifest can just say "Mods" and stay short. */
    name_chars = (int)strlen(scene_name);
    if (name_chars + 4 + 1 > (int)(sizeof(name_w)/sizeof(name_w[0]))) {
        ModsLogf(L"    inject_menu_entry: scene_name %S too long", scene_name);
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
    if (!name_va) {
        ModsLogf(L"    inject_menu_entry: scratch exhausted for name");
        return -1;
    }
    if (scratch_write(name_va, name_w, (name_chars + 4 + 1) * 2) < 0) {
        ModsLogf(L"    inject_menu_entry: name write @0x%08x faulted", name_va);
        return -1;
    }

    g_pending_menu[g_pending_menu_count].label_id      = label_id;
    g_pending_menu[g_pending_menu_count].scene_name_va = name_va;
    g_pending_menu[g_pending_menu_count].section_id    = section_id;
    g_pending_menu[g_pending_menu_count].icon_id       = icon_id;
    g_pending_menu[g_pending_menu_count].flags         = flags;
    g_pending_menu_count++;

    ModsLogf(L"    inject_menu_entry: label=%d scene=%S.xur section=%d "
             L"(name_va=0x%08x)",
             label_id, scene_name, section_id, name_va);
    return 0;
}

/* ── Shared name-nav helper ──────────────────────────────────────────── */

/* Single helper planted once per Phase 2 run at runtime. Each menu-entry
   handler tail-calls into it with r0 = scene_name wstring ptr. The
   helper does:
     XuiSceneCreate(L"gem://", scene_name, NULL, &hScene)
     scene_navigate_wrapper(*current_ctx, hScene)
   Returns the navigate's HRESULT.

   Compiled inline as an ARM shellcode array (caller's scratch_alloc
   places it in gemstone's RW metadata region). */
static DWORD g_nav_helper_va = 0;
static DWORD g_base_uri_va = 0;

static int ensure_nav_helper(void) {
    DWORD code[32];
    DWORD helper_va;
    int k, pool_start, i;
    wchar_t base_uri[8] = { L'g', L'e', L'm', L':', L'/', L'/', 0, 0 };

    if (g_nav_helper_va != 0) return 0;

    /* Plant base URI L"gem://" (7 wchars incl. NUL, pad to 8 for align). */
    g_base_uri_va = scratch_alloc(8 * 2);
    if (!g_base_uri_va || scratch_write(g_base_uri_va, base_uri, 8 * 2) < 0) {
        ModsLogf(L"    nav_helper: base URI plant failed");
        return -1;
    }

    /* Shellcode:
       nav_helper(LPCWSTR scene_name):  ; r0 = scene_name
         push {r4, lr}
         sub sp, sp, #8
         mov r4, r0                    ; r4 = scene_name
         mov r0, #0
         str r0, [sp]                  ; local handle = NULL
         ldr r0, [pc, #BASE_URI]       ; r0 = L"gem://"
         mov r1, r4                    ; r1 = scene_name
         mov r2, #0                    ; r2 = NULL init data
         add r3, sp, #0                ; r3 = &local handle
         ldr r4, [pc, #XUI_SC]
         blx r4                        ; XuiSceneCreate(...)
         cmp r0, #0
         bne fail
         ldr r0, [pc, #CTX]
         ldr r0, [r0]                  ; r0 = *current_ctx
         ldr r1, [sp]                  ; r1 = hScene
         ldr r4, [pc, #NAV]
         blx r4                        ; scene_navigate_wrapper(ctx, hScene)
       done:
         add sp, sp, #8
         pop {r4, lr}
         bx lr
       fail:
         mov r0, #0x80000000
         b done
       pool:
         .word base_uri_va
         .word XUI_SCENE_CREATE_VA
         .word CURRENT_CTX_GLOBAL
         .word SCENE_NAVIGATE_WRAPPER
    */
    k = 0;
    code[k++] = 0xe92d4010u;  /*  0 push {r4, lr}        */
    code[k++] = 0xe24dd008u;  /*  1 sub sp, sp, #8       */
    code[k++] = 0xe1a04000u;  /*  2 mov r4, r0           */
    code[k++] = 0xe3a00000u;  /*  3 mov r0, #0           */
    code[k++] = 0xe58d0000u;  /*  4 str r0, [sp]         */
    code[k++] = 0xe59f0000u;  /*  5 ldr r0, [pc, BASE]   - fixed */
    code[k++] = 0xe1a01004u;  /*  6 mov r1, r4           */
    code[k++] = 0xe3a02000u;  /*  7 mov r2, #0           */
    code[k++] = 0xe28d3000u;  /*  8 add r3, sp, #0       */
    code[k++] = 0xe59f4000u;  /*  9 ldr r4, [pc, XUI_SC] - fixed */
    code[k++] = 0xe12fff34u;  /* 10 blx r4               */
    code[k++] = 0xe3500000u;  /* 11 cmp r0, #0           */
    {
        int bne_fail_idx = k;
        code[k++] = 0xffffffffu;  /* 12 BNE fail - fixed */
        code[k++] = 0xe59f0000u;  /* 13 ldr r0, [pc, CTX] - fixed */
        code[k++] = 0xe5900000u;  /* 14 ldr r0, [r0]      */
        code[k++] = 0xe59d1000u;  /* 15 ldr r1, [sp]      */
        code[k++] = 0xe59f4000u;  /* 16 ldr r4, [pc, NAV] - fixed */
        code[k++] = 0xe12fff34u;  /* 17 blx r4            */
        {
            int done_idx = k;
            code[k++] = 0xe28dd008u;  /* 18 done: add sp, sp, #8 */
            code[k++] = 0xe8bd4010u;  /* 19 pop {r4, lr}    */
            code[k++] = 0xe12fff1eu;  /* 20 bx lr           */
            {
                int fail_idx = k;
                code[k++] = 0xe3a00102u;  /* 21 fail: mov r0, #0x80000000 */
                {
                    int b_done_idx = k;
                    code[k++] = 0xffffffffu;  /* 22 B done */

                    pool_start = k;
                    code[k++] = g_base_uri_va;        /* pool[0] */
                    code[k++] = XUI_SCENE_CREATE_VA;  /* pool[1] */
                    code[k++] = CURRENT_CTX_GLOBAL;   /* pool[2] */
                    code[k++] = SCENE_NAVIGATE_WRAPPER; /* pool[3] */

                    /* Fix forward branches. */
                    {
                        int off;
                        off = fail_idx - (bne_fail_idx + 2);
                        code[bne_fail_idx] = 0x1a000000u |
                            ((DWORD)off & 0xFFFFFFu);
                        off = done_idx - (b_done_idx + 2);
                        code[b_done_idx] = 0xea000000u |
                            ((DWORD)off & 0xFFFFFFu);
                    }
                }
            }
        }
    }

    /* Fix up the ldr [pc, #N] offsets to point into the pool. */
    {
        struct { int code_idx; int pool_idx; } fixups[] = {
            {  5, pool_start + 0 },  /* base_uri */
            {  9, pool_start + 1 },  /* XuiSceneCreate */
            { 13, pool_start + 2 },  /* current_ctx */
            { 16, pool_start + 3 },  /* scene_navigate */
        };
        int nf = (int)(sizeof(fixups)/sizeof(fixups[0]));
        for (i = 0; i < nf; i++) {
            int pc = (fixups[i].code_idx + 2) * 4;
            int tgt = fixups[i].pool_idx * 4;
            int imm = tgt - pc;
            if (imm < 0 || imm > 0xFFF) {
                ModsLogf(L"    nav_helper: ldr offset out of range");
                return -1;
            }
            code[fixups[i].code_idx] |= (DWORD)imm;
        }
    }

    helper_va = scratch_alloc(k * 4);
    if (!helper_va) {
        ModsLogf(L"    nav_helper: scratch exhausted");
        return -1;
    }
    if (scratch_write(helper_va, code, k * 4) < 0) {
        ModsLogf(L"    nav_helper: write faulted @0x%08x", helper_va);
        return -1;
    }
    FlushInstructionCache(GetCurrentProcess(), (void*)helper_va, (DWORD)(k * 4));
    g_nav_helper_va = helper_va;
    ModsLogf(L"    nav_helper planted @0x%08x (%d words, base_uri @0x%08x)",
             helper_va, k, g_base_uri_va);
    return 0;
}

/* Build chain trampoline at 0xF4000 + patch populate-insert site.

   Trampoline layout:
     push {lr}
     for each entry i:
       mov r0, r5                               ; parent in r5
       ldr r1, [pc, #pool_off_desc_i]
       ldr r2, [pc, #pool_off_AddItem]
       blx r2
     pop {lr}
     add r1, r6, #0x118                         ; replicate patched-out
     b POPULATE_RESUME
     .word AddItem (pool[0])
     .word descriptor_va_0..n-1                 ; pool[1..n] */
int flush_menu_entries(void) {
    int n, i, k;
    DWORD descriptor_vas[MAX_PENDING_MENU];
    DWORD tramp[80];
    int total_words;
    int n_code_words;
    int code_start, b_idx, pool_start;
    DWORD b_va, b_pc;
    int b_off;
    DWORD branch_word;
    DWORD insert_pc;
    int insert_off;

    if (g_pending_menu_count == 0) return 0;
    n = g_pending_menu_count;
    ModsLogf(L"  flushing menu chain: %d entries → trampoline @0x%08x",
             n, TRAMP_BASE);

    /* Plant the shared name-nav helper once. Each per-entry handler
       just loads its scene_name into r0 and tail-calls into it. */
    if (ensure_nav_helper() < 0) return -1;

    /* Plant per-entry handler + descriptor in scratch metadata.
       Handler layout (3 words = 12 bytes):
         [+0] ldr r0, [pc, #0]   ; r0 = scene_name (literal at +8)
         [+4] b   nav_helper_va
         [+8] .word scene_name_va */
    for (i = 0; i < n; i++) {
        DWORD handler_va, desc_va;
        DWORD scene_name_va = g_pending_menu[i].scene_name_va;
        DWORD branch_pc, b_inst, imm24, handler_words[3], desc_words[7];
        int target_off;

        handler_va = scratch_alloc(12);
        if (!handler_va) {
            ModsLogf(L"    menu chain: scratch exhausted for handler %d", i);
            return -1;
        }
        /* ldr r0, [pc, #0]; b nav_helper; .word scene_name_va */
        branch_pc = handler_va + 4 + 8;        /* PC at the `b` */
        target_off = (int)(g_nav_helper_va - branch_pc);
        imm24 = ((DWORD)(target_off >> 2)) & 0xFFFFFFu;
        b_inst = 0xea000000u | imm24;
        handler_words[0] = 0xe59f0000u;            /* ldr r0, [pc, #0] */
        handler_words[1] = b_inst;                 /* b nav_helper     */
        handler_words[2] = scene_name_va;          /* literal          */
        if (scratch_write(handler_va, handler_words, 12) < 0) return -1;
        FlushInstructionCache(GetCurrentProcess(), (void*)handler_va, 12);

        desc_va = scratch_alloc(28);
        if (!desc_va) return -1;
        desc_words[0] = (DWORD)g_pending_menu[i].label_id;
        desc_words[1] = handler_va;
        desc_words[2] = (DWORD)g_pending_menu[i].section_id;
        desc_words[3] = 0;
        desc_words[4] = (DWORD)g_pending_menu[i].icon_id;
        desc_words[5] = (DWORD)g_pending_menu[i].flags;
        desc_words[6] = 0;
        if (scratch_write(desc_va, desc_words, 28) < 0) return -1;

        descriptor_vas[i] = desc_va;
        ModsLogf(L"    entry[%d]: handler=0x%08x desc=0x%08x label=%d "
                 L"scene_name_va=0x%08x",
                 i, handler_va, desc_va, g_pending_menu[i].label_id,
                 scene_name_va);
    }

    /* Build trampoline. */
    n_code_words = 1 + n * 4 + 1 + 1 + 1;  /* push, n*{mov,ldr,ldr,blx}, pop, add, b */
    total_words  = n_code_words + 1 + n;   /* + AddItem pool + n desc pool */
    if (total_words > (int)(sizeof(tramp)/sizeof(tramp[0]))) {
        ModsLogf(L"    menu chain: trampoline too large (%d words)", total_words);
        return -1;
    }

    k = 0;
    tramp[k++] = 0xe52de004u;                     /* push {lr} */
    code_start = k;
    for (i = 0; i < n; i++) {
        tramp[k++] = 0xe1a00005u;                 /* mov r0, r5 */
        tramp[k++] = 0;                           /* ldr r1 - fixed up below */
        tramp[k++] = 0;                           /* ldr r2 - fixed up below */
        tramp[k++] = 0xe12fff32u;                 /* blx r2 */
    }
    tramp[k++] = 0xe8bd4000u;                     /* pop {lr} */
    tramp[k++] = POPULATE_INSERT_ORIG;            /* add r1, r6, #0x118 */
    b_idx = k++;                                  /* b POPULATE_RESUME */

    pool_start = k;
    tramp[k++] = ADD_ITEM_FN;                     /* pool[0] = AddItem */
    for (i = 0; i < n; i++) tramp[k++] = descriptor_vas[i];

    /* Fix up the per-entry ldr instructions. ARM `ldr Rd, [pc, #imm]` reads
       from PC+8+imm where PC is the address of the ldr instruction. */
    for (i = 0; i < n; i++) {
        int entry_base    = code_start + i * 4;
        int ldr_r1_idx    = entry_base + 1;
        int ldr_r2_idx    = entry_base + 2;
        int desc_word_idx = pool_start + 1 + i;
        int addit_word_idx= pool_start;
        int pc_r1 = (ldr_r1_idx + 2) * 4;   /* word offset → byte offset for PC */
        int pc_r2 = (ldr_r2_idx + 2) * 4;
        int off_r1 = desc_word_idx * 4 - pc_r1;
        int off_r2 = addit_word_idx * 4 - pc_r2;
        if (off_r1 < 0 || off_r2 < 0 || off_r1 > 0xFFF || off_r2 > 0xFFF) {
            ModsLogf(L"    menu chain: ldr offset out of range (r1=%d r2=%d)",
                     off_r1, off_r2);
            return -1;
        }
        tramp[ldr_r1_idx] = 0xe59f1000u | (DWORD)off_r1;
        tramp[ldr_r2_idx] = 0xe59f2000u | (DWORD)off_r2;
    }

    /* Fix up the `b POPULATE_RESUME` */
    b_va = TRAMP_BASE + (DWORD)b_idx * 4;
    b_pc = b_va + 8;
    b_off = (int)(POPULATE_RESUME - b_pc);
    tramp[b_idx] = 0xea000000u | (((DWORD)(b_off >> 2)) & 0xFFFFFFu);

    /* Plant the trampoline. */
    if (scratch_write(TRAMP_BASE, tramp, total_words * 4) < 0) {
        ModsLogf(L"    menu chain: trampoline write FAULTED");
        return -1;
    }
    /* Force I-cache invalidate for the trampoline range so the CPU
       fetches our freshly-planted instructions. */
    FlushInstructionCache(GetCurrentProcess(), (void*)TRAMP_BASE,
                          (DWORD)(total_words * 4));
    ModsLogf(L"    trampoline planted (%d code words + %d pool words)",
             n_code_words, 1 + n);

    /* PT-flip patch the populate-insert site. */
    insert_pc = POPULATE_INSERT + 8;
    insert_off = (int)(TRAMP_BASE - insert_pc);
    branch_word = 0xea000000u | (((DWORD)(insert_off >> 2)) & 0xFFFFFFu);
    if (patch_kernel_dword(POPULATE_INSERT, branch_word,
                            L"populate-insert") < 0)
        return -1;

    ModsLogf(L"    flush_menu_entries: COMPLETE");
    return 0;
}
