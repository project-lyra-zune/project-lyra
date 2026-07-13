"""modkit: Zune HD mod host tooling.

The authority is the on-device C runtime in `zuxhook` (documented in the
wiki, https://wiki.zune.moe): it lowers declarative manifests
(`requires`/`provides`/`settings`/`status_icons`/`daemons`) and authored
`actions[]` into capability execution at boot. Host tooling does NOT execute
capabilities; it validates manifests structurally, builds assets (`.xui` ->
`.xur`, class blobs), and deploys raw mod directories for zuxhook to apply.

Public API:
  Mod.from_dir(path)         : load + structurally parse a mod from disk
  deploy_mods(mods, ctx)     : mirror mod dirs to the device (zuxhook owns enabled.json)
  restart_gemstone(ctx)      : re-run zuxhook Phase 2 without a reboot (dev)
  ApplyContext(device_ip=)   : device-deploy context
"""
from .manifest import Mod, Action, ManifestError, resolve_args
from .deploy import ApplyContext, deploy_mods, restart_gemstone

__all__ = [
    "Mod", "Action", "ManifestError", "resolve_args",
    "ApplyContext", "deploy_mods", "restart_gemstone",
]
