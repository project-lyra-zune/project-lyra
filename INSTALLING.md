# Installing Lyra

Three ways in. All of them put the platform in `\flash2\automation` and reboot; from the
next boot the device auto-loads Lyra and no installer is involved. See [README.md](README.md)
for the boot flow.

| Method | Needs | Notes |
| :--- | :--- | :--- |
| Browser | the Zune and Wi-Fi | Nothing else. No PC, no MTP, no XNA runtime. |
| Deploy Kit | a PC, [`zune-deploy`](https://github.com/gigalasr/zune-deploy), `~/.mtpz-data` | Leaves an installer app on the home screen. |
| XNA package | a PC, XNA Game Studio Connect / Visual Studio | Same payload as the Deploy Kit. |

## Method A: the browser

On a Zune HD on firmware **v4.5** joined to Wi-Fi, open the browser, go to
`install.zune.moe` and tap **Install**. The device reboots by itself when it is done.

The page writes `\Flash2\lyraboot.exe` and runs it. That bootstrap fetches the platform
`.zmod`, verifies it against a SHA-256 compiled into itself, unpacks it into
`\flash2\automation` and reboots. It writes `\flash2\automation\lyraboot.log` as it goes,
which is the first thing to read if an install does not complete.

Everything on `install.zune.moe` is served over plain HTTP. The v4.5 browser cannot complete
a handshake with a current TLS stack and carries no CA that signs for the domain, so the
pinned digest is the integrity anchor rather than the transport. Everything after the first
boot goes through `reposd`, which has its own TLS.

If the page reports that the attempt did not take, tap Install again. The entry depends on
winning a race in the browser's script engine; a lost race writes nothing. `?debug=1` shows
the per-stage log.

`\Flash2\lyraboot.exe` stays on flash afterwards so a reinstall costs nothing. It never runs
on its own, and it sits outside the automation root, so uninstalling Lyra leaves it.

### Reinstalling

Running the browser install on a device that already has Lyra repairs or updates it. The
bundle replaces only `zuxhook.dll`, `nativeapp.exe`, `lyra.json` and `platform/`; installed
mods, their settings and the logs are untouched. Binaries in use are renamed `.old` and swept
on the next boot.

It installs whatever version the page is pinned to and does not check what is already there,
so pointing it at a device running a newer platform downgrades that device.

If the page sits on **Installing** and the device never reboots, a previous `lyraboot` was
still running and the stub declined to overwrite it. Nothing was written. Wait a few seconds,
reload the page and tap Install again.

## Method B: Deploy Kit (`zune-deploy`)

```
zcli deploy --launch <path>/lyra-hd-deploykit
```

`zune-deploy` installs the `Zune.v4.0.Beta` runtime if absent, streams the payload to
`\gametitle\584E07D1\`, and launches it. Requires the device connected over USB and
enumerated, and `~/.mtpz-data` present.

## Method C: XNA package (`.ccgame`)

Deploy `lyra-hd.ccgame` through the XNA Game Studio Connect / Visual Studio path, or any tool
that installs XNA game packages. Same runtime profile and file layout as the Deploy Kit. The
stock Zune desktop software does not deploy it.
[`Xune`](https://github.com/xune-software/xune-releases) will provide this in a later release.

Both USB methods show an install splash with the current step beneath it (**Preparing**,
**Installing loader**, **Installing daemon**, **Installing mods**), then a short countdown
before rebooting. They also rename the XNA apps entry under "apps" to **"Uninstall Project Lyra"**, so a later
launch removes the platform.

## Confirming it worked

Open the **Mods** tab and check that it loads, then trigger the playback HUD: a new
quick-toggle button appears in the bottom right, where installed mods place their toggles.
On Wi-Fi the daemon answers on port `1337`.

## Uninstalling

Removal takes the device back to stock and reboots. The boot after removal clears
`\flash2\automation` before any Lyra process starts, so every Lyra file deletes cleanly.

- **From the Mods tab.** Open the Project Lyra row in the mod manager and tap **remove**, then
  confirm. This is the only route for a browser install, which creates no tile.
- **From the tile.** Relaunch the renamed **"Uninstall Project Lyra"** tile, if the device was
  installed from USB. It renames the tile back to **"Install Project Lyra"** so a later launch
  reinstalls.

The XNA installer app is left in place so Lyra can be reinstalled from it. To remove it too,
delete it from the homescreen the way you would any app.

## Troubleshooting

- **`ErrorNoDevice` / connection failures (USB).** The Zune must be connected and enumerated
  before deploying. Reconnect it and retry.
- **The browser install does not complete.** Read `\flash2\automation\lyraboot.log`. It records
  the fetch, the byte count, and any digest or unpack failure.

## Building the packages

Pre-built packages are on the [GitHub release](https://github.com/project-lyra-zune/project-lyra/releases).
To build them yourself see [BUILDING.md](BUILDING.md): the repo is source-only, and a release is
two steps, the device binaries on Windows 7 (`build.cmd`) then packaging on a .NET 8 machine
(`tools/packaging/build-release.sh` or `.cmd`), which outputs `dist/lyra-hd-deploykit/` and
`dist/lyra-hd.ccgame`.

The browser install is published separately by `tools/publish-repo.sh`, which regenerates the
feed and packages, stamps `src/lyraboot/boot_pin.h` from that feed, builds the page into
`repo/content/install/`, and deploys. It refuses to build a page whose `lyraboot.exe` does not
carry the pin being published; rebuild with `tools/win7-build.sh lyraboot` when it says so.
