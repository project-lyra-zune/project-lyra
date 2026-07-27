#ifndef MOD_STATE_ABI_H
#define MOD_STATE_ABI_H

#include <windows.h>

/* The cross-process shared-memory layouts: the mod state block, the notify
   consumer registry, and the picker list channel. Declared once and compiled
   only by platform binaries; a mod reaches these through exported calls, never
   by mapping them itself.

   The section names carry the layout version, and a CE6 named kernel object can
   outlive a soft reboot, so a stale section must never be mapped at a new
   stride. Bump the names with MOD_STATE_VERSION for any change to a stride, a
   slot count, or a block's size. Prefer a change that keeps every offset intact:
   growing a block also breaks mapping against a surviving smaller one. */

#define MOD_STATE_SECTION_NAME   L"zune-mod-state-v3"
#define MOD_STATE_LOCK_NAME      L"zune-mod-state-lock-v3"
#define MOD_STATE_VERSION        3u
#define MOD_STATE_MAX_SLOTS      32
#define MOD_STATE_ID_LEN         48     /* role-namespaced key, NUL-padded; not NUL-terminated at full length */

/* Slot flags. Only `setting/` slots carry them. */
#define MOD_SLOT_FLAG_QUICK      0x01u  /* the setting appears in the HUD quick menu */

typedef struct {
    char  key[MOD_STATE_ID_LEN];   /* "setting/<mod>/<id>" | "status/<mod>/<id>" */
    BYTE  state;                   /* current state 0..N-1 (bool: 0/1) */
    BYTE  flags;                   /* MOD_SLOT_FLAG_* */
    BYTE  _pad[2];
    DWORD owner_pid;               /* 0 = control/subsystem-owned; else daemon pid (status reaping) */
} ModFeatureSlot;                  /* 56 bytes */

typedef struct {
    DWORD          version;
    DWORD          count;          /* high-water count of assigned slots */
    ModFeatureSlot slots[MOD_STATE_MAX_SLOTS];
} ModStateBlock;

/* Notify registry: every consumer process registers its wake endpoint here, and
   a producer fans a change out to all of them. */
#define MOD_NOTIFY_SECTION_NAME  L"zune-mod-notify-v1"
#define MOD_NOTIFY_LOCK_NAME     L"zune-mod-notify-lock-v1"
#define MOD_NOTIFY_VERSION       1u
#define MOD_NOTIFY_MAX           16
#define MOD_NOTIFY_NAME_LEN      48

enum {
    MOD_NOTIFY_FREE         = 0,
    MOD_NOTIFY_UI_QUEUE     = 1,   /* CreateMsgQueue + WriteMsgQueue ping */
    MOD_NOTIFY_DAEMON_EVENT = 2    /* CreateEventW + SetEvent, one per daemon */
};

typedef struct {
    DWORD   kind;                       /* MOD_NOTIFY_* (FREE = empty slot) */
    DWORD   owner_pid;                  /* registering process (diagnostic) */
    wchar_t name[MOD_NOTIFY_NAME_LEN];  /* queue / event name, NUL-terminated */
} NotifyConsumer;

typedef struct {
    DWORD          version;
    NotifyConsumer c[MOD_NOTIFY_MAX];
} NotifyBlock;

/* Picker list channel: one per setting that declares a context picker. The mod's
   daemon publishes the option rows; the HUD picker writes back the selection. */
#define MODLISTCH_MAX_ROWS       8
#define MODLISTCH_NAME_LEN       48    /* wchar: row primary + sub label */
#define MODLISTCH_VAL_LEN        40    /* char: opaque selection token, e.g. "ip:port" */
#define MODLISTCH_SUBLABEL_LEN   64    /* wchar: daemon-composed quick-toggle row sub-label */

typedef struct {
    wchar_t name[MODLISTCH_NAME_LEN];   /* primary label (row main) */
    wchar_t sub[MODLISTCH_NAME_LEN];    /* optional sub-label ("" = none) */
    char    value[MODLISTCH_VAL_LEN];   /* selection token the daemon understands */
} ModListChannelRow;

typedef struct {
    DWORD             version;                       /* 1 once initialised */
    DWORD             list_seq;                      /* daemon bumps on each publish */
    DWORD             count;                         /* # valid rows */
    ModListChannelRow row[MODLISTCH_MAX_ROWS];
    DWORD             sel_seq;                       /* HUD bumps on each selection */
    char              sel_value[MODLISTCH_VAL_LEN];  /* chosen token (HUD writes, daemon reads) */
    /* Overrides the setting's status state label when non-empty. A trailing
       ellipsis renders as the native list's animated loading indicator. */
    wchar_t           sublabel[MODLISTCH_SUBLABEL_LEN];
} ModListChannelBlock;

#endif /* MOD_STATE_ABI_H */
