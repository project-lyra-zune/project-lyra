#ifndef MODS_TOGGLES_H
#define MODS_TOGGLES_H

#include <windows.h>

/* Registry of declared settings, fed by the register_setting manifest action
   and consumed by the HUD quick-toggle menu (servicesd). Process-local. The
   setting VALUE lives cross-process in ModStateBlock; this registry holds the
   declaration metadata (key + label + kind + quick-menu eligibility).
   Registration order = enabled-mod order x action order, which fixes each menu
   row's index (the menu's result_value). */

typedef enum {
    MOD_TOGGLE_BINARY = 0,   /* bool: tap flips ModStateBlock.active */
    MOD_TOGGLE_MULTISTATE,   /* reserved (enum, Phase 2): cycle visual_variant through N states */
    MOD_TOGGLE_SUBMENU       /* reserved: tap opens a nested menu from a provider */
} ModToggleKind;

/* HUD quick-menu eligibility - register_setting's quick_toggle field. */
typedef enum {
    MOD_QT_NONE = 0,    /* not a quick toggle (settings-page only) */
    MOD_QT_ELIGIBLE,    /* eligible, but hidden until the user promotes it */
    MOD_QT_DEFAULT      /* eligible + seeded into the curated set on first boot */
} ModQuickToggle;

#define MOD_TOGGLE_MAX        16
#define MOD_TOGGLE_KEY_LEN    48   /* holds a role-namespaced key - matches MOD_STATE_ID_LEN */
#define MOD_TOGGLE_LABEL_LEN  40
#define MOD_TOGGLE_ICON_LEN   160   /* file:// path for the quick-settings row icon */
#define MOD_TOGGLE_STATE_MAX  8    /* max state sub-labels (multistate cap) */
#define MOD_TOGGLE_STATE_LEN  31   /* per-state sub-label length */

#ifdef __cplusplus
extern "C" {
#endif

/* Register one declared setting. key is the ModStateBlock owner/id key (ASCII);
   label_utf8 is the base row label (widened internally); qt is its quick-menu
   eligibility. persist=0 marks the value transient (not restored from / written
   to mod-settings.json, so it resets to its declared default every boot; e.g. a
   "casting" toggle that must default off). Dedup by key; a re-register updates
   kind/label/qt/persist in place. */
void ModTogglesRegister(ModToggleKind kind, const char* key,
                        const char* label_utf8, ModQuickToggle qt, int persist);

int            ModTogglesCount(void);
ModToggleKind  ModToggleGetKind(int i);
const char*    ModToggleGetKey(int i);          /* NUL-terminated ASCII, NULL if OOR */
const wchar_t* ModToggleGetLabel(int i);        /* base label, NULL if OOR */
ModQuickToggle ModToggleGetQuickToggle(int i);  /* MOD_QT_NONE if OOR */
int            ModToggleGetPersist(int i);      /* 1 = value persists across boots (default); 0 = transient */
int            ModToggleIndexForKey(const char* key);  /* registry index, -1 if absent */
const wchar_t* ModToggleGetQuickIcon(int i);     /* file:// path/ref for row art; NULL if none */

/* Per-state sub-label text. BINARY settings default to 0="Disabled" /
   1="Enabled" at register; a mod overrides either via ModToggleSetStateLabel.
   MULTISTATE settings (future) declare each state's label this way. state_idx
   in [0, MOD_TOGGLE_STATE_MAX); label_utf8 is widened internally and extends
   the toggle's state count. */
void           ModToggleSetStateLabel(const char* key, int state_idx, const char* label_utf8);
void           ModToggleSetQuickIcon(const char* key, const wchar_t* icon_path);
int            ModToggleGetStateCount(int i);                    /* 2 for binary; N for multistate */
const wchar_t* ModToggleGetStateLabel(int i, int state_idx);    /* NULL if out of range */

/* Optional status link: a setting whose row sub-label should reflect a status
   slot's state (the effect) rather than the setting's own value (the intent).
   E.g. the "Cast" row showing Connecting/Casting/Error from status/.../casting.
   When set, the menu indexes the toggle's state labels by ModStateGetState of
   this key. ModToggleGetStatusKey returns NULL when no link. */
void           ModToggleSetStatusKey(const char* key, const char* status_key);
const char*    ModToggleGetStatusKey(int i);

#ifdef __cplusplus
}
#endif

#endif /* MODS_TOGGLES_H */
