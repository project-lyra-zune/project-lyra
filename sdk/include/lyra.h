#ifndef LYRA_H
#define LYRA_H

#include <windows.h>

/* The Lyra mod runtime. Link `lyra_client.c`, and declare in your manifest:
 *
 *     "requires": ["lyra.mod_runtime"]
 *
 * Without that line the install gate cannot stop a platform too old to serve
 * these calls, and they fail quietly at runtime instead. Every call is safe
 * before the platform is up: it returns a failure value rather than faulting.
 * See README.md for what the platform owns and what stays yours. */

#ifdef __cplusplus
extern "C" {
#endif

/* ── state ───────────────────────────────────────────────────────────────────
 * Slot keys are role-namespaced. "setting/<mod>/<id>" is INTENT, written by a
 * control surface and read by you. "status/<mod>/<id>" is EFFECT, written by
 * you and read by the HUD. You may only write your own status. */

/* Current state of a slot, 0..N-1. -1 if the slot does not exist yet. */
int    lyra_state_get(const char* key);

/* Publish your own status and wake the UI. The platform stamps your pid, so a
 * status resets to 0 if your process dies. Ignored for a non-"status/" key. */
void   lyra_state_set_status(const char* key, int state);

/* Write your own setting (intent), so a mod that cannot do what its toggle
 * promises can switch itself off rather than leave the control lying. Prefer
 * leaving intent to the user. Ignored for a non-"setting/" key. */
void   lyra_state_set_setting(const char* key, int state);

/* Wake every consumer without touching a slot, e.g. after publishing rows. */
void   lyra_state_notify(void);

/* Your wake event, created and registered on first call. Wait on it rather than
 * polling: a toggle is a discrete act. The name must be unique to your daemon,
 * e.g. L"zune-mod-state-evt-castd" - one event per mod, or an auto-reset wake
 * will reach the wrong waiter. NULL if it cannot be created. */
HANDLE lyra_state_change_event(const wchar_t* daemon_event_name);

/* ── picker channel ──────────────────────────────────────────────────────────
 * For a setting declaring `"context": {"kind": "select"}`. You publish the
 * options; the user's choice comes back as the opaque token you supplied. */

/* Buffer sizes the channel accepts, NUL included. Sizing your locals from these
 * keeps you within what the platform stores; longer strings are truncated. */
#define LYRA_CHANNEL_ROWS_MAX   8
#define LYRA_CHANNEL_NAME_MAX   48   /* wchar_t, for a row's name and sub-label */
#define LYRA_CHANNEL_VALUE_MAX  40   /* char, for your opaque selection token */
#define LYRA_CHANNEL_SUBLABEL_MAX 64 /* wchar_t, for the quick-toggle sub-label */

/* Every channel verb takes your setting's key. Nothing is bound per process, so
 * two mods sharing a host (a load_module DLL in gemstone) cannot collide. */

/* The event the picker signals when it opens, so you can rescan on demand. */
HANDLE lyra_channel_scan_event(const char* toggle_key);

/* Stage one row. `value` is yours: the picker hands it back verbatim on select.
 * Staging is private until commit, so a partly-built list is never displayed.
 * Returns 0 if idx is out of range. */
int    lyra_channel_stage_row(const char* toggle_key, int idx, const wchar_t* name,
                              const wchar_t* sub, const char* value);

/* Publish the first `count` staged rows as the option list, and wake the UI. */
void   lyra_channel_commit(const char* toggle_key, int count);

/* Read back the published list, e.g. to merge a lossy rescan into it. */
int    lyra_channel_row_count(const char* toggle_key);
int    lyra_channel_get_row(const char* toggle_key, int idx,
                            wchar_t* name_out, int name_cap,
                            wchar_t* sub_out, int sub_cap,
                            char* value_out, int value_cap);

/* The user's current choice. Returns 1 if one is set, 0 if none. */
int    lyra_channel_get_selection(const char* toggle_key, char* out, int out_cap);

/* Set the choice yourself, e.g. after auto-picking a single device. */
void   lyra_channel_set_selection(const char* toggle_key, const char* value);

/* Override the quick-toggle row's sub-label with live text. A trailing ellipsis
 * renders as the animated loading indicator. "" restores the state label. */
void   lyra_channel_set_sublabel(const char* toggle_key, const wchar_t* text);

/* ── kernel tools ────────────────────────────────────────────────────────────
 * The addresses are yours; the mechanism behind these is Lyra's. That is the
 * whole reason they are calls: relocate a helper or change how kernel access
 * works and your mod keeps running, because it never compiled either in. */

/* 1 once kernel access is available. Everything below returns a failure value
 * until it is, so poll this at startup rather than assuming. */
int    lyra_kernel_ready(void);

/* Validate and, if needed, replant the kernel helpers. Cheap when they are
 * intact. Call before a sequence of kernel work. 1 if usable. */
int    lyra_kernel_ensure_helpers(void);

DWORD  lyra_kreadu32(DWORD va);
void   lyra_kread(DWORD va, void* buf, DWORD len);
void   lyra_kmemcpy(DWORD va, const void* buf, DWORD len);

/* Call a kernel function with up to six arguments; returns its r0. */
DWORD  lyra_kcall(DWORD fn, DWORD a0, DWORD a1, DWORD a2,
                  DWORD a3, DWORD a4, DWORD a5);

/* Kernel proc-struct VA for a pid, for cross-process work. 0 if not found. */
DWORD  lyra_find_proc_struct(DWORD pid);

/* A kernel scratch word for functions that write an out-parameter: pass it as
 * the pointer, then lyra_kreadu32 it back. Not reentrant; do not hold it. */
DWORD  lyra_kscratch(void);

/* Run the code at `code_va` in another process's context. `target_proc` comes
 * from lyra_find_proc_struct. The address space switch is the platform's. */
DWORD  lyra_kexec_in_proc(DWORD target_proc, DWORD code_va);

/* Patch read-only code in another process. `target_proc` comes from
 * lyra_find_proc_struct; len is 1..64. 0 on success. */
int    lyra_patch_code(DWORD target_proc, DWORD target_va,
                       const void* bytes, int len);

/* 1 if the runtime resolved. Call once at startup to log a clear reason. */
int    lyra_runtime_available(void);

#ifdef __cplusplus
}
#endif

#endif /* LYRA_H */
