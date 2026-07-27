# Lyra mod SDK

`lyra.h` declares 24 calls into the Lyra platform. `lyra_client.c` binds them by
name at first use. Requires Lyra 1.3.0 or newer.

    include/lyra.h      the API
    src/lyra_client.c   binds it at runtime
    optional/ce_log.*   a flash logger, not part of the API

## BUILD

Add both files to the mod's build. In the `.mak`:

    !IFNDEF LYRA_SDK
    LYRA_SDK = ..\..\..\sdk
    !ENDIF

    INCS = /I"$(LYRA_SDK)\include" ...

    $(OBJ_DIR)\lyra_client.obj: $(LYRA_SDK)\src\lyra_client.c
        $(CC) $(C_CFLAGS) /Fo"$(OBJ_DIR)\lyra_client.obj" /c $(LYRA_SDK)\src\lyra_client.c

The default suits a mod living in the platform repo's `mods/`. Override
`LYRA_SDK` for an SDK unpacked elsewhere.

## MANIFEST

    "requires": ["lyra.mod_runtime"],

    "settings": [{
      "id": "screencast",
      "type": "bool",
      "label": "Screen share",
      "default": false,
      "persist": false,
      "quick_toggle": true,
      "status": "sharing",
      "context": { "kind": "select" }
    }],

    "status": [{
      "id": "sharing",
      "states": ["Off", "Ready", "Live"]
    }]

Without the `requires` line the install gate cannot hold the mod back on a
platform too old to serve these calls, and every call fails quietly at runtime
instead of the install saying so.

`"context": {"kind": "select"}` is what puts a picker behind a long press on the
quick toggle. A setting without it has no channel.

## KEYS

    setting/<mod_id>/<settings[].id>    intent, written by a control surface
    status/<mod_id>/<status[].id>       effect, written by the owning mod

Both ids come from `manifest.json`, so the keys for the declaration above are
`setting/screencast/screencast` and `status/screencast/sharing`.

A slot holds an index, 0..N-1, into the declaration that created it: a bool
setting is 0 or 1, a status is a position in its `states` array. A mod may write
only its own status. A slot exists once the platform has applied the mod; until
then every read is -1.

## STATE

    int    lyra_state_get(const char* key)
           Current value. -1 if the slot does not exist.

    void   lyra_state_set_status(const char* key, int state)
           Publish and wake the UI. The platform stamps the calling pid, so the
           status resets to 0 if the process dies. Ignored for a non-status key.

    void   lyra_state_set_setting(const char* key, int state)
           Write own intent, for a mod that cannot do what its toggle promises
           and should switch itself off rather than leave the control lying.
           Ignored for a non-setting key.

    void   lyra_state_notify(void)
           Wake every consumer without touching a slot, e.g. after publishing
           channel rows.

    HANDLE lyra_state_change_event(const wchar_t* daemon_event_name)
           The daemon's wake event, created and registered on first call. NULL
           if it cannot be created.

A publish reaches every consumer, so publish only on a real change. Re-asserting
the same value in a control loop wakes both UI hosts at loop rate and pins their
message pumps.

Wait on the change event rather than polling: a toggle is a discrete act. The
name must be unique to the daemon, for example `L"zune-mod-state-evt-castd"`.
One event per mod, since an auto-reset wake reaches a single waiter.

## CHANNEL

The option list behind a `"kind": "select"` setting. The mod publishes rows; the
user's choice comes back as the opaque token the mod supplied. Every verb takes
the setting's key, so mods sharing a host cannot collide.

    LYRA_CHANNEL_ROWS_MAX       8
    LYRA_CHANNEL_NAME_MAX      48   wchar_t, a row's name and sub-label
    LYRA_CHANNEL_VALUE_MAX     40   char, the selection token
    LYRA_CHANNEL_SUBLABEL_MAX  64   wchar_t, the quick-toggle sub-label

Sizes include the NUL. Longer strings are truncated.

    HANDLE lyra_channel_scan_event(const char* toggle_key)
           Signalled when the picker opens, for rescanning on demand.

    int    lyra_channel_stage_row(const char* toggle_key, int idx,
                                  const wchar_t* name, const wchar_t* sub,
                                  const char* value)
           Stage one row. Staging is private until commit, so a partly built
           list is never displayed. 0 if idx is out of range.

    void   lyra_channel_commit(const char* toggle_key, int count)
           Publish the first count staged rows and wake the UI.

    int    lyra_channel_row_count(const char* toggle_key)
    int    lyra_channel_get_row(const char* toggle_key, int idx,
                                wchar_t* name_out, int name_cap,
                                wchar_t* sub_out, int sub_cap,
                                char* value_out, int value_cap)
           Read the published list back, e.g. to merge a lossy rescan into it.

    int    lyra_channel_get_selection(const char* toggle_key,
                                      char* out, int out_cap)
           The current choice. 1 if one is set, 0 if none.

    void   lyra_channel_set_selection(const char* toggle_key, const char* value)
           Set the choice, e.g. after auto-picking a single device.

    void   lyra_channel_set_sublabel(const char* toggle_key,
                                     const wchar_t* text)
           Live text on the quick-toggle row. A trailing ellipsis renders as the
           animated loading indicator; "" restores the state label.

## KERNEL

The addresses belong to the mod. The mechanism behind these calls belongs to the
platform, which is why they are calls: a helper can move, or kernel access can
change shape, and a mod that compiled neither one keeps running.

    int    lyra_kernel_ready(void)
           1 once kernel access is available. Everything below returns a failure
           value until then.

    int    lyra_kernel_ensure_helpers(void)
           Validate and, if needed, replant the kernel helpers. Cheap when they
           are intact. 1 if usable.

    DWORD  lyra_kreadu32(DWORD va)
    void   lyra_kread(DWORD va, void* buf, DWORD len)
    void   lyra_kmemcpy(DWORD va, const void* buf, DWORD len)

    DWORD  lyra_kcall(DWORD fn, DWORD a0, DWORD a1, DWORD a2,
                      DWORD a3, DWORD a4, DWORD a5)
           Call a kernel function with up to six arguments. Returns its r0.

    DWORD  lyra_find_proc_struct(DWORD pid)
           Kernel proc-struct VA for a pid, for cross-process work. 0 if not
           found.

    DWORD  lyra_kscratch(void)
           A kernel scratch word for functions that write an out-parameter: pass
           it as the pointer, then read it back with lyra_kreadu32. Not
           reentrant; do not hold it.

    DWORD  lyra_kexec_in_proc(DWORD target_proc, DWORD code_va)
           Run the code at code_va in another process's context. target_proc
           comes from lyra_find_proc_struct.

    int    lyra_patch_code(DWORD target_proc, DWORD target_va,
                           const void* bytes, int len)
           Patch read-only code in another process. len is 1..64. 0 on success.

Kernel access is not up the instant a daemon starts. Poll `lyra_kernel_ready()`
before the first kernel call, and call `lyra_kernel_ensure_helpers()` before a
run of them.

## NOTES

    int    lyra_runtime_available(void)
           1 if the runtime resolved.

Binding is late and failure is soft, because a daemon can start before the
platform is up and must not fail to load over a missing symbol. Every call is
safe to make before then: it returns a failure value rather than faulting.
`lyra_runtime_available()` answers once, so a daemon can log a single clear
reason instead of a string of failures.

Nothing in the runtime is bound per process. Channel verbs take the setting key
and wake events are created per name, so a mod loaded into gemstone alongside
others cannot take over their channel or wait on their event by accident.

`optional/ce_log` is a flash logger for daemons that want one. It is not part of
the API: nothing in `lyra.h` depends on it, its changes fall outside
`lyra.mod_runtime`'s compatibility window, and a mod may drop it for its own.

Any logger on this device has to cap itself. Nothing prunes a log file, so an
uncapped one fills flash. At its ceiling `ce_log` keeps the most recent three
quarters of the file and drops the rest, cut at a line boundary.

## GAPS

A platform internal that no verb reaches is a missing verb. Report it at
https://github.com/project-lyra-zune/project-lyra
