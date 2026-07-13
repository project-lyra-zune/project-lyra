# Installing Lyra

TEMPORARY: When installing via zune-deploy, use this fork for now: https://github.com/magicisinthehole/zune-deploy

Lyra installs through a one-time XNA launch. You deploy a package containing the
exploiter plus the Lyra payload (`nativeapp.exe`, `zuxhook.dll`, and the bundled
mods) to the device, launch it once, and the payload mirrors itself into
`\flash2\automation`. From the next boot on, the device auto-loads Lyra and XNA is
no longer involved. See [README.md](README.md) for the boot flow.

Each release ships the same payload in **two interchangeable package forms**. Pick
whichever matches the deploy tool you have; the on-device result is identical.

| Form | File | Deploy with |
| :--- | :--- | :--- |
| Deploy Kit | `lyra-hd-deploykit/` (folder) | [`zune-deploy`](https://github.com/gigalasr/zune-deploy) over USB-MTP |
| XNA package | `lyra-hd.ccgame` | the XNA deployment toolchain (XNA Game Studio Connect / Visual Studio) |

The `.ccgame` is a standard XNA game package and is tool-agnostic. The stock Zune
desktop software does not deploy it; the XNA Game Studio Connect / Visual Studio
deploy path does. [`Xune`](https://github.com/xune-software/xune-releases) will provide this capability in a later release.

## Requirements

- A Zune HD on firmware **v4.5**, connected over USB.
- The release packages (from the [GitHub release](https://github.com/magicisinthehole/project-lyra/releases), or built locally, see [Building the packages](#building-the-packages)).
- For the Deploy Kit path: `zune-deploy` and its `~/.mtpz-data` MTP auth file.

## Method A: Deploy Kit (`zune-deploy`)

```
zcli deploy --launch <path>/lyra-hd-deploykit
```

`zune-deploy` installs the `Zune.v4.0.Beta` runtime if absent, streams the payload
to `\gametitle\584E07D1\`, and launches it.

## Method B: XNA package (`.ccgame`)

Deploy `lyra-hd.ccgame` with the XNA Game Studio Connect / Visual Studio deployment
path (or any tool that installs XNA game packages). It carries the same runtime
profile (`Zune.v4.0.Beta`) and the same file layout as the Deploy Kit.

## What happens on the device

1. The launched title shows the Lyra install splash screen with the
   current step beneath it (**Preparing → Installing loader → Installing daemon → Installing mods**) as it copies the payload into
   `\flash2\automation`. When the copy finishes it shows **"Rebooting..."** with a
   short countdown, then reboots the device automatically.
2. On reboot, gemstone (the Zune HD UI) auto-loads `zuxhook` from
   `\flash2\automation`, which spawns the `nativeapp` daemon and applies the mods.
3. Once on the homescreen, the **Mods** tab is present and the bundled system mods
   are active.

To confirm, open the **Mods** tab and check that it loads, then trigger the playback
HUD: a new quick-toggle button appears in the bottom right, where installed mods place
their toggles. When the device is on Wi-Fi, the daemon answers on port `1337`.

## Troubleshooting

- **`ErrorNoDevice` / connection failures.** The Zune must be connected over USB and
  enumerated before deploying. Reconnect it and retry.

## Building the packages

Pre-built packages are available from the [GitHub release](https://github.com/magicisinthehole/project-lyra/releases). To build them yourself, see [BUILDING.md](BUILDING.md): the repo is source-only, and building a release is two steps: the device binaries on Windows 7 (`build.cmd`), then packaging on a .NET 8 machine (`tools/packaging/build-release.sh` or `.cmd`), which outputs `dist/lyra-hd-deploykit/` and `dist/lyra-hd.ccgame`.
