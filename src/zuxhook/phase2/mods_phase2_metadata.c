/* mods_phase2_metadata.c - declarative metadata capabilities
   (register_setting / register_status / add_status_icon / tint_element /
   suppress_scene). Split from mods_phase2.c; dispatched via its table. */

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
#include "mods_list_channel_provider.h" /* ModListChannelProviderRegister (context picker) */
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

/* ── slot-key + asset-url helpers (metadata-local) ──────────────────── */
/* Build a role-namespaced slot key "role/owner/id" (role = "setting" | "status").
   The role prefix is the writer-role contract enforced by ModStateBlock. Fail
   fast (-1) if the composed key would not fit the slot field; a key that
   overflows is a declaration error, not something to silently truncate. */
static int form_key(char* out, int cap, const char* role,
                    const char* owner, const char* id) {
    int rlen = role  ? (int)strlen(role)  : 0;
    int olen = owner ? (int)strlen(owner) : 0;
    int ilen = id    ? (int)strlen(id)    : 0;
    int p = 0;
    if (rlen + 1 + olen + 1 + ilen >= cap) return -1;
    memcpy(out + p, role, rlen);  p += rlen;  out[p++] = '/';
    memcpy(out + p, owner, olen); p += olen;  out[p++] = '/';
    memcpy(out + p, id, ilen);    p += ilen;  out[p]   = 0;
    return 0;
}

/* Split an indicator `source` ("setting/<id>" or "status/<id>") into its role
   and bare id, then form the full slot key "role/<mod>/<id>". The indicator
   binds to a control slot (setting/) or an actor-written status slot (status/);
   the role is the source's own prefix, not a fixed per-action-type value. */
static int form_source_key(char* out, int cap, const char* mod_id,
                           const char* source) {
    const char* slash;
    char role[16];
    int  rlen;
    if (!source) return -1;
    slash = strchr(source, '/');
    if (!slash) return -1;
    rlen = (int)(slash - source);
    if (rlen <= 0 || rlen >= (int)sizeof(role)) return -1;
    memcpy(role, source, rlen);
    role[rlen] = 0;
    if (strcmp(role, "setting") != 0 && strcmp(role, "status") != 0) return -1;
    return form_key(out, cap, role, mod_id, slash + 1);
}

/* Resolve a manifest asset ref to an XUI resource URI. A fully-qualified engine
   transport URI (file://, gemskin://, gem://, ...) is passed through verbatim so a
   mod can name an on-device asset it does not ship, e.g.
   gemskin://images/thumb_wifistate_3.png, the stock skin wifi glyph the network
   list draws. @asset//@mod/ shorthands expand against the mod's own directory. In
   every case forward slashes are folded to backslash; the resource resolver
   validates the scheme. */
static int build_asset_url(wchar_t* out, int cap, const Mod* mod,
                           const char* ref_utf8) {
    int p = 0, i = 0, use_assets = 0;
    const char* rel = ref_utf8;
    const char* scheme = ref_utf8 ? strstr(ref_utf8, "://") : NULL;
    const wchar_t prefix[] = L"file://";
    if (!out || cap <= 0 || !mod || !ref_utf8) return -1;
    out[0] = 0;

    if (scheme) {
        int head = (int)(scheme - ref_utf8) + 3;   /* copy "<scheme>://" verbatim */
        for (i = 0; i < head && p < cap - 1; i++)
            out[p++] = (wchar_t)(unsigned char)ref_utf8[i];
        for (; ref_utf8[i] && p < cap - 1; i++)
            out[p++] = (ref_utf8[i] == '/') ? L'\\' : (wchar_t)(unsigned char)ref_utf8[i];
        out[p] = 0;
        return ref_utf8[i] ? -1 : 0;
    }
    if (strncmp(ref_utf8, "@asset/", 7) == 0) { use_assets = 1; rel = ref_utf8 + 7; }
    else if (strncmp(ref_utf8, "@mod/", 5) == 0) { rel = ref_utf8 + 5; }
    else return -1;

    for (i = 0; prefix[i] && p < cap - 1; i++) out[p++] = prefix[i];
    for (i = 0; mod->source_dir[i] && p < cap - 1; i++) out[p++] = mod->source_dir[i];
    if (use_assets) {
        const wchar_t assets[] = L"\\assets";
        for (i = 0; assets[i] && p < cap - 1; i++) out[p++] = assets[i];
    }
    if (p < cap - 1) out[p++] = L'\\';
    for (i = 0; rel[i] && p < cap - 1; i++)
        out[p++] = (rel[i] == '/') ? L'\\' : (wchar_t)(unsigned char)rel[i];
    out[p] = 0;
    return rel[i] ? -1 : 0;
}

/* ── register_setting ────────────────────────────────────────────────────
   Declares one setting. Pure in-process metadata (no kerncore): forms the
   namespaced key owner/id (owner = the declaring mod's id), registers
   {kind, key, label, quick_toggle eligibility} into the declared-settings
   registry, and seeds the slot's boot default via ModStateSeed (idempotent,
   keeps a since-boot value). value_type bool only (initial support); an unknown
   value_type or quick_toggle is a declaration error (fail fast, not silent).
   servicesd-targeted (the menu host owns the registry). */
int apply_register_setting(ModAction* a, ModsArena* arena) {
    const char*    value_type = NULL;
    const char*    id = NULL;
    const char*    label = NULL;
    const char*    qt_s = NULL;
    ModQuickToggle qt = MOD_QT_NONE;
    int            default_val = 0;
    int            persist = 1;       /* values persist across boots unless persist:false */
    char           key[MOD_STATE_ID_LEN + 1];

    if (ModActionGetString(a, "value_type", arena, &value_type, NULL, 0) != 0 || !value_type) {
        ModsLogf(L"    register_setting: value_type required");
        return -1;
    }
    if (strcmp(value_type, "bool") != 0) {
        ModsLogf(L"    register_setting: value_type %S unsupported (value_type bool only)", value_type);
        return -1;
    }
    if (ModActionGetString(a, "id", arena, &id, NULL, 0) != 0 || !id) {
        ModsLogf(L"    register_setting: id required");
        return -1;
    }
    if (ModActionGetString(a, "label", arena, &label, NULL, 0) != 0 || !label) {
        ModsLogf(L"    register_setting: label required");
        return -1;
    }
    if (ModActionGetString(a, "quick_toggle", arena, &qt_s, NULL, 0) == 0 && qt_s) {
        if (strcmp(qt_s, "default") == 0)       qt = MOD_QT_DEFAULT;
        else if (strcmp(qt_s, "eligible") == 0) qt = MOD_QT_ELIGIBLE;
        else {
            ModsLogf(L"    register_setting: unknown quick_toggle %S", qt_s);
            return -1;
        }
    }
    if (ModActionGetBool(a, "default", 0, &default_val) < 0) {
        ModsLogf(L"    register_setting: 'default' must be true/false");
        return -1;
    }
    /* persist=false -> transient: the value is never restored from or written to
       mod-settings.json, so it resets to `default` each boot (e.g. a casting
       toggle that must default off). */
    if (ModActionGetBool(a, "persist", 1, &persist) < 0) {
        ModsLogf(L"    register_setting: 'persist' must be true/false");
        return -1;
    }

    if (form_key(key, sizeof(key), "setting", a->mod->mod_id, id) != 0) {
        ModsLogf(L"    register_setting: key setting/%S/%S too long", a->mod->mod_id, id);
        return -1;
    }
    ModTogglesRegister(MOD_TOGGLE_BINARY, key, label, qt, persist);
    ModStateSeed(key, default_val, 0);

    /* Optional per-setting sub-label overrides. bool defaults to
       Disabled/Enabled (set in ModTogglesRegister); a mod may override either. */
    {
        const char* slbl = NULL;
        if (ModActionGetString(a, "disabled_label", arena, &slbl, NULL, 0) == 0 && slbl)
            ModToggleSetStateLabel(key, 0, slbl);
        slbl = NULL;
        if (ModActionGetString(a, "enabled_label", arena, &slbl, NULL, 0) == 0 && slbl)
            ModToggleSetStateLabel(key, 1, slbl);
    }
    {
        const char* icon_ref = NULL;
        wchar_t icon_url[MOD_TOGGLE_ICON_LEN + 1];
        if (ModActionGetString(a, "quick_icon", arena, &icon_ref, NULL, 0) == 0 && icon_ref) {
            if (build_asset_url(icon_url, (int)(sizeof(icon_url) / sizeof(icon_url[0])),
                                a->mod, icon_ref) != 0) {
                ModsLogf(L"    register_setting: bad quick_icon %S", icon_ref);
                return -1;
            }
            ModToggleSetQuickIcon(key, icon_url);
        }
    }
    /* holds: subsystems demanded while this setting is on. "wifi_awake" registers
       this setting's slot as a pull-based keepalive demand source (servicesd). */
    {
        const char* holds = NULL;
        if (ModActionGetString(a, "holds", arena, &holds, NULL, 0) == 0 && holds) {
            const char* s = holds;
            while (*s) {
                const char* e = s;
                while (*e && *e != ',') e++;
                if ((e - s) == 15 && strncmp(s, "lyra.wifi_awake", 15) == 0)
                    WifiAwakeRegisterDemand(key);
                s = (*e == ',') ? e + 1 : e;
            }
        }
    }
    /* status link: the row sub-label reflects a status slot's state name, and the
       per-state labels come from the linked status's declared states. */
    {
        const char* ssrc = NULL;
        const char* slabels = NULL;
        if (ModActionGetString(a, "status_source", arena, &ssrc, NULL, 0) == 0 && ssrc) {
            char skey[MOD_STATE_ID_LEN + 1];
            if (form_source_key(skey, sizeof(skey), a->mod->mod_id, ssrc) == 0)
                ModToggleSetStatusKey(key, skey);
        }
        if (ModActionGetString(a, "state_labels", arena, &slabels, NULL, 0) == 0 && slabels) {
            const char* s = slabels;
            int idx = 0;
            char buf[MOD_TOGGLE_STATE_LEN + 1];
            while (*s) {
                int n = 0;
                while (*s && *s != ',' && n < MOD_TOGGLE_STATE_LEN) buf[n++] = *s++;
                buf[n] = 0;
                while (*s && *s != ',') s++;   /* drop any overflow past the field */
                ModToggleSetStateLabel(key, idx++, buf);
                if (*s == ',') s++;
            }
        }
    }

    /* context: declaring a picker registers the generic context-list provider for
       this key. Long-pressing the row opens a list sourced from the setting's
       cross-process channel (the mod's daemon publishes options). "select" is the
       only kind today; an unknown kind is a declaration error (fail fast). */
    {
        const char* ckind = NULL;
        if (ModActionGetString(a, "context_kind", arena, &ckind, NULL, 0) == 0 && ckind) {
            if (strcmp(ckind, "select") != 0) {
                ModsLogf(L"    register_setting: unknown context.kind %S", ckind);
                return -1;
            }
            ModListChannelProviderRegister(key);
        }
    }

    ModsLogf(L"    register_setting: %S value_type=bool default=%d quick_toggle=%d",
             key, default_val, (int)qt);
    return 0;
}

/* ── register_status ─────────────────────────────────────────────────────
   Seeds an actor-written status slot (status/<mod>/<id>) to state 0 so an
   indicator binds to a live slot at boot. The actor (subsystem authority or
   daemon) stamps the live state + owner_pid at runtime. */
int apply_register_status(ModAction* a, ModsArena* arena) {
    const char* id = NULL;
    char        key[MOD_STATE_ID_LEN + 1];
    if (ModActionGetString(a, "id", arena, &id, NULL, 0) != 0 || !id) {
        ModsLogf(L"    register_status: id required");
        return -1;
    }
    if (form_key(key, sizeof(key), "status", a->mod->mod_id, id) != 0) {
        ModsLogf(L"    register_status: key status/%S/%S too long", a->mod->mod_id, id);
        return -1;
    }
    ModStateSeed(key, 0, 0);   /* off; the actor stamps live state + owner */
    ModsLogf(L"    register_status: %S seeded off", key);
    return 0;
}

/* ── add_status_icon ─────────────────────────────────────────────────────
   Declares a status icon to inject into the iconGrid. Pure in-process metadata:
   registers {token, key, scene} into the icon registry the icon-host injects
   from. The fragment's element id is "modicon_<token>" (the bare setting id,
   /-free); the controller reflects the namespaced state key owner/id. */
/* Parse "a,b,c" of signed ints into out[]; returns count, or -1 on a bad token. */
static int parse_int_csv(const char* s, signed char* out, int max) {
    int n = 0;
    while (*s && n < max) {
        int neg = 0, val = 0, got = 0;
        while (*s == ' ') s++;
        if (*s == '-') { neg = 1; s++; }
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; got = 1; }
        if (!got) return -1;
        out[n++] = (signed char)(neg ? -val : val);
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return n;
}

/* Parse "AARRGGBB,..." of hex words into out[]; returns count, or -1. */
static int parse_hex_csv(const char* s, DWORD* out, int max) {
    int n = 0;
    while (*s && n < max) {
        DWORD v = 0;
        int got = 0;
        while (*s == ' ') s++;
        for (;;) {
            char c = *s;
            DWORD d;
            if      (c >= '0' && c <= '9') d = (DWORD)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (DWORD)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (DWORD)(c - 'A' + 10);
            else break;
            v = (v << 4) | d; s++; got = 1;
        }
        if (!got) return -1;
        out[n++] = v;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return n;
}

int apply_add_status_icon(ModAction* a, ModsArena* arena) {
    const char* source = NULL;
    const char* scene = NULL;
    const char* frames_s = NULL;
    const char* tints_s = NULL;
    const char* token;
    char        key[MOD_STATE_ID_LEN + 1];
    signed char frame_of[MOD_ICON_STATES_MAX];
    DWORD       tint_of[MOD_ICON_STATES_MAX];
    int         nf, nt;
    if (ModActionGetString(a, "source", arena, &source, NULL, 0) != 0 || !source) {
        ModsLogf(L"    add_status_icon: source required");
        return -1;
    }
    if (ModActionGetString(a, "scene", arena, &scene, NULL, 0) != 0 || !scene) {
        ModsLogf(L"    add_status_icon: scene required");
        return -1;
    }
    if (ModActionGetString(a, "frames", arena, &frames_s, NULL, 0) != 0 || !frames_s ||
        ModActionGetString(a, "tints", arena, &tints_s, NULL, 0) != 0 || !tints_s) {
        ModsLogf(L"    add_status_icon: frames/tints required");
        return -1;
    }
    if (form_source_key(key, sizeof(key), a->mod->mod_id, source) != 0) {
        ModsLogf(L"    add_status_icon: bad source %S", source);
        return -1;
    }
    nf = parse_int_csv(frames_s, frame_of, MOD_ICON_STATES_MAX);
    nt = parse_hex_csv(tints_s, tint_of, MOD_ICON_STATES_MAX);
    if (nf <= 0 || nf != nt) {
        ModsLogf(L"    add_status_icon: frames/tints count mismatch %d/%d", nf, nt);
        return -1;
    }
    token = strchr(source, '/') + 1;   /* bare id after the role/, the modicon_<token> element id */
    ModIconsRegister(token, key, scene, frame_of, tint_of, nf);
    ModsLogf(L"    add_status_icon: token=%S key=%S states=%d scene=%S", token, key, nf, scene);
    return 0;
}

/* Parse an RGB ("RRGGBB") or ARGB ("AARRGGBB") hex string to 0xAARRGGBB; a
   6-digit value defaults alpha to 0xFF. Returns 0 on success. */
static int parse_argb(const char* s, DWORD* out) {
    DWORD v = 0;
    int   n = 0;
    if (!s || !out) return -1;
    for (; s[n]; n++) {
        char c = s[n];
        DWORD d;
        if      (c >= '0' && c <= '9') d = (DWORD)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (DWORD)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (DWORD)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    if (n == 6) v |= 0xFF000000u;
    else if (n != 8) return -1;
    *out = v;
    return 0;
}

/* ── tint_element ─────────────────────────────────────────────────────────
   Recolor an authored scene element (by id) and its subtree while a state slot is
   active. Registers the tint colour; the icon-host resolves the element per
   scene-create and drives ModUiTintSet/Clear (a render-time colour multiply). */
int apply_tint_element(ModAction* a, ModsArena* arena) {
    const char* element = NULL;
    const char* source  = NULL;
    const char* color   = NULL;
    char        key[MOD_STATE_ID_LEN + 1];
    DWORD       argb = 0;
    if (ModActionGetString(a, "element", arena, &element, NULL, 0) != 0 || !element) {
        ModsLogf(L"    tint_element: element required");
        return -1;
    }
    if (ModActionGetString(a, "source", arena, &source, NULL, 0) != 0 || !source) {
        ModsLogf(L"    tint_element: source required");
        return -1;
    }
    if (ModActionGetString(a, "color", arena, &color, NULL, 0) != 0 || !color) {
        ModsLogf(L"    tint_element: color required");
        return -1;
    }
    if (parse_argb(color, &argb) != 0) {
        ModsLogf(L"    tint_element: color %S not RRGGBB/AARRGGBB hex", color);
        return -1;
    }
    if (form_source_key(key, sizeof(key), a->mod->mod_id, source) != 0) {
        ModsLogf(L"    tint_element: bad source %S", source);
        return -1;
    }
    ModIconTintRegister(element, key, argb);
    ModsLogf(L"    tint_element: element=%S key=%S argb=0x%08x", element, key, argb);
    return 0;
}

/* Register a XUI scene URI for suppression while a setting is active. Manifest:
     { "type":"suppress_scene", "scene":"gem://Sync.xur", "state":"hide" }
   Runs in the host where the XuiSceneCreateEx proxy lives (gemstone by default;
   set target_proc "all"/"servicesd" for HUD scenes) so it registers into that
   host's own mods_scene_suppress table. The proxy then fails the create for the
   URI while "<mod_id>/<state>" is on, so the host's navigator skips the scene. */
int apply_suppress_scene(ModAction* a, ModsArena* arena) {
    const char* scene = NULL;
    const char* state = NULL;
    wchar_t     uri[80];
    char        key[MOD_STATE_ID_LEN + 1];
    int         i;
    if (ModActionGetString(a, "scene", arena, &scene, NULL, 0) != 0) {
        ModsLogf(L"    suppress_scene: missing 'scene'"); return -1;
    }
    if (ModActionGetString(a, "state", arena, &state, NULL, 0) != 0) {
        ModsLogf(L"    suppress_scene: missing 'state'"); return -1;
    }
    if (form_key(key, sizeof(key), "setting", a->mod->mod_id, state) != 0) {
        ModsLogf(L"    suppress_scene: key setting/%S/%S too long", a->mod->mod_id, state);
        return -1;
    }
    for (i = 0; scene[i] && i < (int)(sizeof(uri)/sizeof(uri[0])) - 1; i++)
        uri[i] = (wchar_t)(unsigned char)scene[i];
    uri[i] = 0;
    if (ModSceneSuppressAdd(uri, key) != 0) {
        ModsLogf(L"    suppress_scene: registry full (%S)", scene); return -1;
    }
    ModsLogf(L"    suppress_scene: %S gated by %S", scene, key);
    return 0;
}

