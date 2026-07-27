<p align="center">
  <img src="assets/lyra.png" alt="Project Lyra" width="420">
</p>

<p align="center">
  <a href="https://ko-fi.com/magichole"><img alt="Support on Ko-fi" src="https://img.shields.io/badge/Ko--fi-support-FF5E5B?logo=ko-fi&logoColor=white"></a>
  <a href="https://buymeacoffee.com/magichole"><img alt="Support on Buy Me a Coffee" src="https://img.shields.io/badge/Buy%20Me%20a%20Coffee-support-FFDD00?logo=buymeacoffee&logoColor=black"></a>
</p>

# Project Lyra

A persistent jailbreak and modding platform for the **Zune HD** (firmware v4.5).

Lyra uses CUB3D's [**zuneslayer exploit**](https://github.com/CUB3D/zuneslayer) to establish a permanent, self-loading modding environment. A number of tools are provided as part of this system to freely manipulate and extend the device firmware:

- **`nativeapp`**: the native payload. It gains kernel privileges through the
  `libnmvwavedev.dll` arbitrary write and exposes an arbitrary kernel read/write
  daemon (`kerncore`) plus an RPC server reachable over Wi-Fi.
- **`zuxhook`**: an in-process hook DLL that the device auto-loads on boot. It
  patches the stock UI shell (gemstone) at runtime, loads `nativeapp`, and hosts
  **the modkit runtime**: it lowers mod manifests and executes their capabilities
  on-device at boot.
- **modkit**: a declarative mod format plus host-side tooling (`tools/`) for
  validating, building, and deploying mods.

Documentation: **<https://wiki.zune.moe>** (in progress).

## Installing

Two ways in, both landing on the same self-loading platform. Full instructions are
in **[INSTALLING.md](INSTALLING.md)**.

- **From the device's own browser, with no PC.** On a Zune HD on v4.5 and Wi-Fi, open
  **<http://install.zune.moe>** and tap Install. The page writes a bootstrap to flash and
  runs it; the bootstrap fetches the platform package, checks it against a SHA-256
  compiled into itself, unpacks it and reboots.
- **From a PC.** A release's Deploy Kit or `.ccgame` carries the same payload and
  installs in a single one-time **XNA** launch.

The XNA mechanism, for the second path:

1. A package bundles the exploiter plus the payload (`nativeapp.exe`, `zuxhook.dll`,
   and the system mods). Deployed via XNA and launched once, `nativeapp` sees it was
   started from the title's `Content` directory and runs in **install mode**: it
   copies the payload into `flash2\automation` (plain flash I/O, no exploit) and
   reports **"Lyra successfully installed."**
2. On reboot the device **auto-loads `zuxhook`** from `flash2\automation`. Its
   watchdog spawns `nativeapp` from there; launched outside the XNA title,
   `nativeapp` runs in **daemon mode**: it triggers the `libnmvwavedev.dll` kernel
   exploit, plants the `kerncore` R/W gadget, and serves the RPC daemon on TCP 1337.
3. From here XNA is no longer involved. The platform is persistent and the **Mods**
   tab is present on the homescreen.

Installing over XNA also renames the Apps tile to **"Uninstall Project Lyra"**, so the
same tile becomes the uninstaller.

## Removing Lyra

Lyra has two removal paths. Both take the device back to stock and reboot; the boot after
removal clears `\flash2\automation` before any Lyra process starts, so even resident files
delete cleanly.

- **The tile.** Relaunch the renamed **"Uninstall Project Lyra"** tile. It removes the
  platform and renames the tile back to **"Install Project Lyra"**, so a later launch
  reinstalls.
- **The Mods tab.** Open the Project Lyra row in the mod manager and tap **remove**, then
  confirm. Use this when the XNA installer app has been deleted.

The XNA installer app is kept so Lyra can be reinstalled. Delete it from the homescreen
yourself if you want it gone.

## Mods

Mods install on the device itself: **Mods > browse**, which reads a feed over Wi-Fi and
installs from it. The default feed is **<https://repo.zune.moe>**, built from `mods/` by
`tools/publish-repo.sh`; a feed is a static `feed.json` plus `.zmod` files, so anyone can
host one.

Writing a mod means a `manifest.json` declaring what it needs, and optionally a daemon or
DLL of your own. Mods reach the platform through the **mod SDK** (`sdk/`): two files,
`lyra.h` and `lyra_client.c`, giving 24 calls for shared state, status publishing, the
picker UI and kernel access. A mod compiles none of Lyra's internals, so a platform update
cannot break it by moving something inside itself. The SDK ships with each release as
`lyra-sdk-<version>.zip` and its README is the reference; **[CAPABILITIES.md](CAPABILITIES.md)**
covers what a manifest may declare.

## Credits

Lyra is built on [zuneslayer](https://github.com/CUB3D/zuneslayer) by CUB3D. The
`nativeapp` payload began as zuneslayer's Zune HD native exe, and Lyra keeps its
kernel read/write primitive (`kerncore`, built on the `libnmvwavedev.dll` bug)
along with its nanopb wire protocol. The XNA launcher under `src/exploiter/` comes
from the OpenZDK / DevelopmentFront delivery template by way of zuneslayer,
vendored here so a clone builds on its own. zuneslayer also carries the browser-ROP
entrypoint (CVE-2019-1367), the alternative to Lyra's one-time XNA launch.

[THIRD-PARTY.md](THIRD-PARTY.md) has the full upstream and vendored-code credits.

## Layout

| Path | What it is |
| :--- | :--- |
| `src/kerncore/` | Arbitrary kernel R/W primitive (shared by `nativeapp`, `zuxhook`, plugins) |
| `src/shared/` | Small sources shared by `nativeapp` and `zuxhook`: `title_name` (the zmdb Apps-tile rename) and `device_reboot` |
| `src/nativeapp/` | The native payload: kernel R/W daemon + Wi-Fi RPC server |
| `src/zuxhook/` | Auto-loaded hook DLL: persistence, gemstone UI hooks, **modkit runtime** |
| `src/exploiter/` | XNA launcher that starts `nativeapp` on first run (vendored; see Credits) |
| `modkit/` | The modkit: authoring/deploy CLI (`mod-apply.py`) + the `modkit` Python package |
| `sdk/` | The mod SDK a third-party mod builds against: `lyra.h` + `lyra_client.c` |
| `src/mod-runtime/` | The runtime behind those calls, shared by every platform binary |
| `lyra/` | The Lyra platform bundle: `manifest.json` plus the platform component mods under `platform/` (`mods-tab`, the mod-management UI; `wifi-power`, the Wi-Fi keep-alive, `clock-date`, adds a date picker to the clock settings so the device clock can be set accurately for https). Versioned and updated as one entity. |
| `mods/` | Feature mods: `marketplace-redirect` (redirects the Marketplace endpoints to the [**ZuneNet community servers**](https://github.com/ZuneDev/ZuneNetApi)), `playnext` (adds play next queuing to context menus, placing selected music after the current track), `night-mode` (a quick toggle that washes the UI red), `screencast` (mirror the screen over Wi-Fi), `zune-cast` (stream audio to a Chromecast). `youtube` lives here too but is marked experimental: music works, video is unfinished. |
| `plugins/` | Device plugin DLLs the nativeapp daemon loads at runtime. `plugin-hello` is the template; plugin examples that share a mod's engine live with the mod (`screencast`, `zune-cast`). |
| `repo/` | The mod feed site: `feed.json` + `.zmod` packages, published by `tools/publish-repo.sh` |
| `install/` | The browser-install page served at `install.zune.moe` |
| `src/lyraboot/` | The bootstrap that page runs: fetches, verifies and unpacks the platform |
| `tools/` | Host tools: `general/` (transport, file I/O), `re/` (reverse-engineering), `disasm/` (ARM/CE6), `xui/` (skin authoring), `capture/` (screen) |
| `wiki/` | Documentation (submodule; the source for <https://wiki.zune.moe>, in progress) |

## Building

See [BUILDING.md](BUILDING.md). The device targets are ARM (ARMV4I), built with the
OpenZDK toolchain under Visual Studio on Windows 7. Prebuilt binaries are published
with each GitHub release.

## Deploying a mod

```
python modkit/mod-apply.py validate <mod_dir>
python modkit/mod-apply.py apply    <mod_dir> --ip <device-ip>
```

`--ip` defaults to `192.168.0.100`.

`zuxhook`'s C runtime applies mods at boot; the CLI only validates, builds assets,
and pushes the raw mod directory. Architecture is documented in the `wiki/`
(<https://wiki.zune.moe>).

## License

MIT. See [LICENSE](LICENSE). Vendored third-party components keep their own
licenses (THIRD-PARTY.md).
