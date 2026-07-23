# Lyra capabilities

Every action a mod performs is a platform capability that Lyra applies at boot. This is the
list of capabilities a mod can use, and how Lyra decides whether a mod will run on a given
version.

## Compatibility is automatic

You declare what your mod *does* (its `actions`, and the `settings` / `status` / `status_icons`
/ `daemons` that lower to actions). You do not declare which Lyra version you need. When a mod
is packaged, its capability footprint is computed from the manifest and travels in the feed; a
device installs the mod only if its Lyra provides every capability in that footprint.

Lyra only ever adds capabilities within a major version, so a mod built for one release runs on
every later release in the same major line unchanged. When a capability is missing, the mod
manager blocks the install and says which case it is:

- the device's Lyra is older than the mod needs: **Update Lyra** (a newer Lyra provides it),
- a newer Lyra dropped a revision the mod was built against: **this mod needs an update**,
- nothing available provides it at all: the block names the missing capability.

A mod can also depend on another mod, by using a capability under that mod's namespace rather
than `lyra.*`. Lyra resolves the dependency against your installed mods and the feed: if the
provider is not installed but available, the mod manager offers to install it together with the
mod, in dependency order; if nothing provides it, the install is blocked naming the capability.

## Naming and revisions

Every capability is namespaced by who provides it. Platform capabilities live in the reserved
`lyra.*` namespace (`lyra.inject_settings_row`); a capability one mod provides to another is
named by that mod's own provenance (`myaudioplugin.reverb`) and may never use `lyra.*`. The
namespace tells the device where to resolve a capability: `lyra.*` against the platform,
anything else against the other installed mods.

A capability also has a revision, and the packager handles it for you. Each platform capability
advertises a compatibility window; when your mod is packaged, every `lyra.*` capability it uses
is stamped with the platform's current revision automatically. You write no revision by hand.
All platform capabilities are currently at revision 1.

The two string forms differ by their number count, and you rarely write either:

- a **point** (something you require) is `name` or `name@r`, one number: the revision you need,
- a **range** (something you provide) is `name` or `name@lo:hi`, both bounds: the window you
  serve.

You only write a capability string by hand in two cases, both in the manifest's `requires`: a
revision you must pin (`lyra.inject_settings_row@2`), or a capability your mod reaches from
inside a shipped binary that the manifest cannot otherwise reveal. A mod you author that provides
a capability to other mods lists it in `provides` as a range under your own namespace
(`myaudioplugin.reverb@1:2`).

## Capabilities

`Phase` is the boot pass that applies the action: 1 is the compositor (file I/O, no kernel), 2
is servicesd/gemstone (XUI surfaces and kernel-backed actions). Subsystems are activated on
demand for a mod that uses them. All capabilities are currently at revision 1.

### Actions, Phase 1 (compositor)

| Capability | Purpose |
| :--- | :--- |
| `lyra.gem_add_entry` | Add a gemstone package entry |
| `lyra.gem_add_entry_bytes` | Add a gemstone package entry from inline bytes |
| `lyra.gem_replace_entry` | Replace an existing gemstone package entry |
| `lyra.gem_remove_entry` | Remove a gemstone package entry |
| `lyra.xus_add_string` | Add a localized string table entry |
| `lyra.xus_set_string` | Set an existing localized string |
| `lyra.registry_write` | Write a device registry value |
| `lyra.write_blob_bytes` | Write a file to flash from inline bytes |
| `lyra.spawn_daemon` | Launch a mod daemon process at boot |

### Actions, Phase 2 (servicesd / gemstone)

| Capability | Purpose |
| :--- | :--- |
| `lyra.register_setting` | Declare a settings toggle |
| `lyra.register_status` | Declare an actor-written status output |
| `lyra.add_status_icon` | Bind a status/setting to a HUD status icon |
| `lyra.tint_element` | Recolor a scene element from a status/setting |
| `lyra.register_visuals` | Register custom visual resources |
| `lyra.register_xui_class` | Register a custom XUI scene class |
| `lyra.inject_menu_entry` | Add an entry to a native menu |
| `lyra.inject_settings_row` | Add a row to the native Settings list |
| `lyra.suppress_scene` | Suppress a native scene |
| `lyra.patch_bytes` | Patch bytes at a kernel VA |
| `lyra.kcall` | Call a kernel routine |
| `lyra.require_kernel_value` | Assert a kernel value before applying |
| `lyra.read_kernel_va` | Read a kernel VA into a back-reference |
| `lyra.require_back_ref_range` | Assert a back-reference is in range |
| `lyra.require_back_ref_equal` | Assert a back-reference equals a value |
| `lyra.install_function_hook` | Detour a function |
| `lyra.load_module` | Load a mod DLL into a target process |

### Subsystems

| Capability | Purpose |
| :--- | :--- |
| `lyra.wifi_awake` | Keep Wi-Fi powered under battery |
| `lyra.volume_state` | Publish volume as a shared state slot |
