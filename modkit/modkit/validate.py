"""Structural manifest validation, shared by the CLI (`validate`) and the
packager (`package` / `feed`).

The on-device C runtime in zuxhook is the authority; this mirrors its checks so
an authoring error fails on the host before a reboot rather than being silently
dropped on-device. Because packaging calls `structural_check` too, `validate` and
`feed` can never disagree: nothing can be published that `validate` rejects.
"""
from __future__ import annotations

from .manifest import Mod, LYRA_PLATFORM_ID

# Keys a settings[] object may carry. The runtime (zuxhook's lower_settings +
# apply_register_setting) reads exactly these; anything else is an authoring typo
# and is rejected here so it fails before a reboot rather than being silently
# ignored on-device.
_SETTING_KEYS = frozenset({
    "id", "type", "label", "default", "persist",
    "quick_toggle", "quick_icon", "disabled_label", "enabled_label",
    "holds", "status", "context",
})
# Context picker kinds the runtime understands (mods_list_channel_provider).
_CONTEXT_KINDS = frozenset({"select"})

# The complete top-level manifest vocabulary. An unknown key is an authoring typo
# (or a field the runtime will silently drop), so it is rejected here. Metadata
# keys (author/description/category/art_url) feed the repo catalog; the arrays are
# authored actions[] plus the declarative v2 fields zuxhook lowers on-device.
_TOP_LEVEL_KEYS = frozenset({
    "mod_id", "name", "version", "author", "description", "category",
    "art_url", "depends_on", "experimental", "changelog",
    "actions", "requires", "provides", "settings", "status", "status_icons",
    "daemons", "persistent",
    "platform_files",   # the lyra platform manifest only
})
# The catalog row/detail render the description from a fixed device buffer
# (REPO_DESC_LEN = 256 wide chars incl NUL, in reposd/repo_ipc.h), so 255 is the hard
# ceiling. Keep it to a few sentences: a summary, not a spec.
MAX_DESCRIPTION_LEN = 255
# The detail page's "What's new" block, carried in a fixed device buffer
# (REPO_CHANGELOG_LEN = 512 wide chars incl NUL, in reposd/repo_ipc.h).
MAX_CHANGELOG_LEN = 511


def structural_check(mod: Mod, raw: dict) -> list[str]:
    """Pre-flight manifest check mirroring zuxhook's C validation (the on-device
    boot log stays authoritative). Catches the same authoring errors before a
    reboot: shape, required fields, target_proc on declarations, slot<->setting
    correspondence, and duplicate ids/slots."""
    errs: list[str] = []
    for k in raw:
        if k not in _TOP_LEVEL_KEYS:
            errs.append(f"unknown top-level key '{k}' "
                        f"(allowed: {sorted(_TOP_LEVEL_KEYS)})")
    # `lyra` is the reserved platform id; only the platform manifest (with platform_files)
    # may claim it, so a feature mod can never route down the platform path.
    if raw.get("mod_id") == LYRA_PLATFORM_ID and not raw.get("platform_files"):
        errs.append(f"'{LYRA_PLATFORM_ID}' is the reserved platform id; a feature mod cannot use it")
    for strk in ("name", "version", "author", "category", "art_url"):
        v = raw.get(strk)
        if v is not None and not isinstance(v, str):
            errs.append(f"'{strk}' must be a string")
    cat = raw.get("category")
    if isinstance(cat, str) and not cat.strip():
        errs.append("'category' must be a non-empty string")
    desc = raw.get("description")
    if isinstance(desc, str) and len(desc) > MAX_DESCRIPTION_LEN:
        errs.append(f"'description' is {len(desc)} chars; "
                    f"limit is {MAX_DESCRIPTION_LEN}")
    # changelog: optional "what's new" shown on the detail when an update is available.
    clog = raw.get("changelog")
    if clog is not None and not isinstance(clog, str):
        errs.append("'changelog' must be a string")
    elif isinstance(clog, str) and len(clog) > MAX_CHANGELOG_LEN:
        errs.append(f"'changelog' is {len(clog)} chars; limit is {MAX_CHANGELOG_LEN}")
    # experimental: an author-declared maturity flag. The mod is still published; the
    # feed carries the flag so the Browse detail can mark it.
    exp = raw.get("experimental")
    if exp is not None and not isinstance(exp, bool):
        errs.append("'experimental' must be a boolean")
    for i, a in enumerate(mod.actions):
        if not a.type:
            errs.append(f"action[{i}]: missing 'type'")
    for key in ("requires", "provides", "persistent"):
        v = raw.get(key)
        if v is not None and not (isinstance(v, list) and all(isinstance(x, str) for x in v)):
            errs.append(f"'{key}' must be a list of strings")
    for arr in ("settings", "status_icons", "daemons"):
        v = raw.get(arr)
        if v is not None and not isinstance(v, list):
            errs.append(f"'{arr}' must be an array")

    # High-level declarations never name a process; the runtime owns routing.
    for arr in ("settings", "status", "status_icons", "daemons"):
        for i, s in enumerate(raw.get(arr) or []):
            if isinstance(s, dict) and "target_proc" in s:
                errs.append(f"{arr}[{i}]: must not set target_proc (runtime owns routing)")

    setting_ids: set[str] = set()
    for i, s in enumerate(raw.get("settings") or []):
        if not isinstance(s, dict):
            errs.append(f"settings[{i}]: not an object")
            continue
        sid = s.get("id")
        if not isinstance(sid, str) or not sid:
            errs.append(f"settings[{i}]: missing 'id'")
        elif sid in setting_ids:
            errs.append(f"settings[{i}]: duplicate id '{sid}'")
        else:
            setting_ids.add(sid)
        t = s.get("type")
        if t is not None and t != "bool":
            errs.append(f"settings[{i}]: unsupported type '{t}'")
        holds = s.get("holds")
        if holds is not None:
            known = {"wifi_awake", "volume_state"}
            if not isinstance(holds, list) or not all(isinstance(h, str) for h in holds):
                errs.append(f"settings[{i}]: 'holds' must be a list of subsystem names")
            else:
                for h in holds:
                    if h not in known:
                        errs.append(f"settings[{i}]: holds '{h}' is not a known subsystem {sorted(known)}")
        # context: an optional long-press picker. {"kind": "select"} is the only
        # shape the runtime registers a provider for.
        ctx_decl = s.get("context")
        if ctx_decl is not None:
            if not isinstance(ctx_decl, dict):
                errs.append(f"settings[{i}]: 'context' must be an object")
            else:
                kind = ctx_decl.get("kind")
                if kind not in _CONTEXT_KINDS:
                    errs.append(f"settings[{i}]: context.kind must be one of {sorted(_CONTEXT_KINDS)}")
                for k in ctx_decl:
                    if k != "kind":
                        errs.append(f"settings[{i}]: unknown context key '{k}'")
        # Reject unknown keys so an authoring typo (e.g. 'quik_icon') is caught
        # here instead of being silently dropped by the runtime.
        for k in s:
            if k not in _SETTING_KEYS:
                errs.append(f"settings[{i}]: unknown key '{k}' (allowed: {sorted(_SETTING_KEYS)})")

    # status[]: actor-written effect outputs. Each declares its state vocabulary.
    status_ids: set[str] = set()
    status_nstates: dict[str, int] = {}
    for i, s in enumerate(raw.get("status") or []):
        if not isinstance(s, dict):
            errs.append(f"status[{i}]: not an object")
            continue
        sid = s.get("id")
        if not isinstance(sid, str) or not sid:
            errs.append(f"status[{i}]: missing 'id'")
        elif sid in status_ids:
            errs.append(f"status[{i}]: duplicate id '{sid}'")
        else:
            status_ids.add(sid)
        states = s.get("states")
        if (not isinstance(states, list) or not (1 <= len(states) <= 8)
                or not all(isinstance(x, str) and x for x in states)):
            errs.append(f"status[{i}]: 'states' must be 1..8 non-empty strings")
        elif isinstance(sid, str):
            status_nstates[sid] = len(states)

    # A setting's `status` link must name a declared status[] output.
    for i, s in enumerate(raw.get("settings") or []):
        if isinstance(s, dict) and s.get("status") is not None:
            st = s.get("status")
            if not isinstance(st, str) or st not in status_ids:
                errs.append(f"settings[{i}]: status '{st}' has no matching status[] declaration")

    def _check_source(ctx: str, src):
        # An indicator binds to a control (setting/) or an actor-written status/
        # slot; the role is the source's prefix, validated against a declaration.
        if not isinstance(src, str) or "/" not in src:
            errs.append(f"{ctx}: source '{src}' must be setting/<id> or status/<id>")
            return None
        role, _, bare = src.partition("/")
        if role == "setting":
            if bare not in setting_ids:
                errs.append(f"{ctx}: source '{src}' has no matching setting")
        elif role == "status":
            if bare not in status_ids:
                errs.append(f"{ctx}: source '{src}' has no matching status[]")
        else:
            errs.append(f"{ctx}: source '{src}' role must be 'setting' or 'status'")
            return None
        return (role, bare)

    seen_sources: set[str] = set()
    for i, s in enumerate(raw.get("status_icons") or []):
        if not isinstance(s, dict):
            errs.append(f"status_icons[{i}]: not an object")
            continue
        src = s.get("source")
        parsed = _check_source(f"status_icons[{i}]", src)
        if isinstance(src, str):
            if src in seen_sources:
                errs.append(f"status_icons[{i}]: duplicate source '{src}'")
            seen_sources.add(src)
        # Each frames[k] entry: null/"" = hidden, a "@" image ref = its own
        # frame, or a hex colour = tint the base "image". A colour entry needs a
        # base image to recolour.
        def _is_hex_color(x):
            return (isinstance(x, str) and len(x) in (6, 8)
                    and all(c in "0123456789abcdefABCDEF" for c in x))
        frames = s.get("frames")
        image = s.get("image")
        if not isinstance(frames, list) or not (1 <= len(frames) <= 8):
            errs.append(f"status_icons[{i}]: 'frames' must be an array of 1..8 "
                        f"entries (null, a hex colour, or an image ref per state)")
        else:
            has_color = False
            for k, fr in enumerate(frames):
                if fr is None or fr == "":
                    continue
                if isinstance(fr, str) and fr.startswith(("@/", "@asset/", "@mod/")):
                    continue
                if _is_hex_color(fr):
                    has_color = True
                    continue
                errs.append(f"status_icons[{i}].frames[{k}]: '{fr}' must be null, "
                            f"a hex colour (RRGGBB/AARRGGBB), or a @/, @asset/ or @mod/ image ref")
            if not any(isinstance(fr, str) and fr for fr in frames):
                errs.append(f"status_icons[{i}]: at least one frame must be visible")
            if has_color and not (isinstance(image, str)
                                  and image.startswith(("@/", "@asset/", "@mod/"))):
                errs.append(f"status_icons[{i}]: a hex-colour frame needs a base "
                            f"\"image\" (@/, @asset/ or @mod/ ref)")
            if parsed and parsed[0] == "status" and parsed[1] in status_nstates \
                    and len(frames) != status_nstates[parsed[1]]:
                errs.append(f"status_icons[{i}]: frames length {len(frames)} != "
                            f"status '{parsed[1]}' state count {status_nstates[parsed[1]]}")

    # tint_element actions also carry a `source`; validate the same way.
    for i, a in enumerate(raw.get("actions") or []):
        if isinstance(a, dict) and a.get("type") == "tint_element":
            _check_source(f"actions[{i}] (tint_element)", a.get("source"))

    for i, s in enumerate(raw.get("daemons") or []):
        if not isinstance(s, dict) or not s.get("binary"):
            errs.append(f"daemons[{i}]: missing 'binary'")
    return errs
