/* Resolve-once client: every verb is bound by name on first use, so a mod ships
 * export names and never a memory layout. Binding is late and failure is soft,
 * because a daemon may start before the platform is up and must not fail to
 * load over a missing symbol. */

#include "lyra.h"

#define LYRA_RUNTIME_PATH  L"\\flash2\\automation\\zuxhook.dll"

typedef int    (*fn_state_get)(const char*);
typedef void   (*fn_state_set_status)(const char*, int);
typedef void   (*fn_void)(void);
typedef HANDLE (*fn_change_event)(const wchar_t*);
typedef HANDLE (*fn_handle_key)(const char*);
typedef int    (*fn_int)(void);
typedef int    (*fn_int_key)(const char*);
typedef int    (*fn_stage_row)(const char*, int, const wchar_t*, const wchar_t*, const char*);
typedef void   (*fn_commit)(const char*, int);
typedef int    (*fn_get_row)(const char*, int, wchar_t*, int, wchar_t*, int, char*, int);
typedef int    (*fn_get_sel)(const char*, char*, int);
typedef void   (*fn_set_sel)(const char*, const char*);
typedef void   (*fn_set_sub)(const char*, const wchar_t*);
typedef DWORD  (*fn_kreadu32)(DWORD);
typedef void   (*fn_kread)(DWORD, void*, DWORD);
typedef void   (*fn_kmemcpy)(DWORD, const void*, DWORD);
typedef DWORD  (*fn_kcall)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef DWORD  (*fn_dword_arg)(DWORD);
typedef int    (*fn_patch)(DWORD, DWORD, const void*, int);
typedef DWORD  (*fn_dword_void)(void);
typedef DWORD  (*fn_kexec)(DWORD, DWORD);

static struct {
    int                 tried;
    HMODULE             rt;
    fn_state_get        state_get;
    fn_state_set_status state_set_status;
    fn_state_set_status state_set_setting;
    fn_void             state_notify;
    fn_change_event     change_event;
    fn_handle_key       channel_scan_event;
    fn_stage_row        channel_stage_row;
    fn_commit           channel_commit;
    fn_int_key          channel_row_count;
    fn_get_row          channel_get_row;
    fn_get_sel          channel_get_selection;
    fn_set_sel          channel_set_selection;
    fn_set_sub          channel_set_sublabel;
    fn_int              kernel_ready;
    fn_int              kernel_ensure_helpers;
    fn_kreadu32         kreadu32;
    fn_kread            kread;
    fn_kmemcpy          kmemcpy_;
    fn_kcall            kcall;
    fn_dword_arg        find_proc_struct;
    fn_patch            patch_code;
    fn_dword_void       kscratch;
    fn_kexec            kexec_in_proc;
} R;

static void* bind_verb(const char* name) {
    wchar_t w[64];
    int i;
    for (i = 0; i < 63 && name[i]; i++) w[i] = (wchar_t)(unsigned char)name[i];
    w[i] = 0;
    return (void*)GetProcAddress(R.rt, w);
}

/* One attempt per process. A daemon that starts before the platform stays
   unbound for its lifetime, which is correct: the platform spawns its daemons,
   so if it is absent when we start, it is absent. */
static int resolve(void) {
    if (R.tried) return R.rt != NULL;
    R.tried = 1;
    R.rt = LoadLibraryW(LYRA_RUNTIME_PATH);
    if (!R.rt) return 0;

    R.state_get             = (fn_state_get)       bind_verb("lyra_state_get");
    R.state_set_status      = (fn_state_set_status)bind_verb("lyra_state_set_status");
    R.state_set_setting     = (fn_state_set_status)bind_verb("lyra_state_set_setting");
    R.state_notify          = (fn_void)            bind_verb("lyra_state_notify");
    R.change_event          = (fn_change_event)    bind_verb("lyra_state_change_event");
    R.channel_scan_event    = (fn_handle_key)      bind_verb("lyra_channel_scan_event");
    R.channel_stage_row     = (fn_stage_row)       bind_verb("lyra_channel_stage_row");
    R.channel_commit        = (fn_commit)          bind_verb("lyra_channel_commit");
    R.channel_row_count     = (fn_int_key)         bind_verb("lyra_channel_row_count");
    R.channel_get_row       = (fn_get_row)         bind_verb("lyra_channel_get_row");
    R.channel_get_selection = (fn_get_sel)         bind_verb("lyra_channel_get_selection");
    R.channel_set_selection = (fn_set_sel)         bind_verb("lyra_channel_set_selection");
    R.channel_set_sublabel  = (fn_set_sub)         bind_verb("lyra_channel_set_sublabel");
    R.kernel_ready          = (fn_int)             bind_verb("lyra_kernel_ready");
    R.kernel_ensure_helpers = (fn_int)             bind_verb("lyra_kernel_ensure_helpers");
    R.kreadu32              = (fn_kreadu32)        bind_verb("lyra_kreadu32");
    R.kread                 = (fn_kread)           bind_verb("lyra_kread");
    R.kmemcpy_              = (fn_kmemcpy)         bind_verb("lyra_kmemcpy");
    R.kcall                 = (fn_kcall)           bind_verb("lyra_kcall");
    R.find_proc_struct      = (fn_dword_arg)       bind_verb("lyra_find_proc_struct");
    R.patch_code            = (fn_patch)           bind_verb("lyra_patch_code");
    R.kscratch              = (fn_dword_void)      bind_verb("lyra_kscratch");
    R.kexec_in_proc         = (fn_kexec)           bind_verb("lyra_kexec_in_proc");
    return 1;
}

int lyra_runtime_available(void) { return resolve(); }

int lyra_state_get(const char* key) {
    if (!resolve() || !R.state_get) return -1;
    return R.state_get(key);
}

void lyra_state_set_status(const char* key, int state) {
    if (resolve() && R.state_set_status) R.state_set_status(key, state);
}

void lyra_state_set_setting(const char* key, int state) {
    if (resolve() && R.state_set_setting) R.state_set_setting(key, state);
}

void lyra_state_notify(void) {
    if (resolve() && R.state_notify) R.state_notify();
}

HANDLE lyra_state_change_event(const wchar_t* daemon_event_name) {
    if (!resolve() || !R.change_event) return NULL;
    return R.change_event(daemon_event_name);
}

HANDLE lyra_channel_scan_event(const char* toggle_key) {
    if (!resolve() || !R.channel_scan_event) return NULL;
    return R.channel_scan_event(toggle_key);
}

int lyra_channel_stage_row(const char* toggle_key, int idx, const wchar_t* name,
                           const wchar_t* sub, const char* value) {
    if (!resolve() || !R.channel_stage_row) return 0;
    return R.channel_stage_row(toggle_key, idx, name, sub, value);
}

void lyra_channel_commit(const char* toggle_key, int count) {
    if (resolve() && R.channel_commit) R.channel_commit(toggle_key, count);
}

int lyra_channel_row_count(const char* toggle_key) {
    if (!resolve() || !R.channel_row_count) return 0;
    return R.channel_row_count(toggle_key);
}

int lyra_channel_get_row(const char* toggle_key, int idx,
                         wchar_t* name_out, int name_cap,
                         wchar_t* sub_out, int sub_cap,
                         char* value_out, int value_cap) {
    if (!resolve() || !R.channel_get_row) return 0;
    return R.channel_get_row(toggle_key, idx, name_out, name_cap, sub_out, sub_cap,
                             value_out, value_cap);
}

int lyra_channel_get_selection(const char* toggle_key, char* out, int out_cap) {
    if (!resolve() || !R.channel_get_selection) return 0;
    return R.channel_get_selection(toggle_key, out, out_cap);
}

void lyra_channel_set_selection(const char* toggle_key, const char* value) {
    if (resolve() && R.channel_set_selection) R.channel_set_selection(toggle_key, value);
}

void lyra_channel_set_sublabel(const char* toggle_key, const wchar_t* text) {
    if (resolve() && R.channel_set_sublabel) R.channel_set_sublabel(toggle_key, text);
}

int lyra_kernel_ready(void) {
    if (!resolve() || !R.kernel_ready) return 0;
    return R.kernel_ready();
}

int lyra_kernel_ensure_helpers(void) {
    if (!resolve() || !R.kernel_ensure_helpers) return 0;
    return R.kernel_ensure_helpers();
}

DWORD lyra_kreadu32(DWORD va) {
    if (!resolve() || !R.kreadu32) return 0;
    return R.kreadu32(va);
}

void lyra_kread(DWORD va, void* buf, DWORD len) {
    if (resolve() && R.kread) R.kread(va, buf, len);
}

void lyra_kmemcpy(DWORD va, const void* buf, DWORD len) {
    if (resolve() && R.kmemcpy_) R.kmemcpy_(va, buf, len);
}

DWORD lyra_kcall(DWORD fn, DWORD a0, DWORD a1, DWORD a2,
                 DWORD a3, DWORD a4, DWORD a5) {
    if (!resolve() || !R.kcall) return 0;
    return R.kcall(fn, a0, a1, a2, a3, a4, a5);
}

DWORD lyra_find_proc_struct(DWORD pid) {
    if (!resolve() || !R.find_proc_struct) return 0;
    return R.find_proc_struct(pid);
}

int lyra_patch_code(DWORD target_proc, DWORD target_va,
                    const void* bytes, int len) {
    if (!resolve() || !R.patch_code) return -1;
    return R.patch_code(target_proc, target_va, bytes, len);
}

DWORD lyra_kscratch(void) {
    if (!resolve() || !R.kscratch) return 0;
    return R.kscratch();
}

DWORD lyra_kexec_in_proc(DWORD target_proc, DWORD code_va) {
    if (!resolve() || !R.kexec_in_proc) return 0;
    return R.kexec_in_proc(target_proc, code_va);
}
