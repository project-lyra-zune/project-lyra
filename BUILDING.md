# Building Project Lyra

Building a release is a **two-step process**, because the toolchains don't overlap:
the device binaries need Windows 7 + Visual Studio 2008, and packaging the mods needs
XUIHelper, which targets **.NET 8** and does not run on Windows 7.

1. **Phase 1: device binaries (Windows 7).** Build the four binaries.
2. **Phase 2: packaging (.NET 8 machine).** Regenerate the mod artifacts from source
   and assemble the release packages.

The repository holds **source only**: no compiled binaries, scenes, class blobs, or
splash assets are committed (see `.gitignore`). Phase 2 regenerates all of them. If you
just want to run Lyra, grab the prebuilt packages from the latest
[GitHub release](https://github.com/magicisinthehole/project-lyra/releases); you do not
need to build anything.

## The device binaries

| Binary | Project | Toolchain |
| :--- | :--- | :--- |
| `nativeapp.exe` | `src/nativeapp` (WinCE ARMV4I) | VS2008 + OpenZDK + NVIDIA Tegra CE6 SDK |
| `zuxhook.dll` | `src/zuxhook` (WinCE ARMV4I) | VS2008 + OpenZDK |
| `exploiter.exe` | `src/exploiter` (XNA, managed) | VS2008 + XNA Game Studio 3.1 |
| `reposd.exe` | `lyra/platform/mods-tab/src/reposd` (WinCE ARMV4I) | VS2008 + OpenZDK |

`reposd.exe` is the mods-tab component mod's own native daemon; Phase 1 copies it into
`lyra/platform/mods-tab/`, where Phase 2 bundles it. The mod-management UI (Browse +
Manage + the unified detail) lives in `zuxhook.dll`; there is no separate browse DLL.

## Phase 1: device binaries (Windows 7)

Prerequisites:

- **Visual Studio 2008** (VC9). The build driver is
  `C:\Program Files (x86)\Microsoft Visual Studio 9.0\Common7\IDE\devenv.com`.
- **OpenZDK** so the `OpenZDK (ARMV4I)` platform is available to VC9.
- **XNA Game Studio 3.1** (builds the `exploiter` XNA title).
- **NVIDIA Tegra CE6 SDK** (`cgc.exe` / `shaderfix.exe` compile the install-splash
  shaders). `build-local.cmd` finds it at the standard install path; if it is elsewhere,
  set **`NV_WINCE_T2_PLAT`** to its root (the folder holding `host_bin\`).

From a Windows shell in the repo root:

```
build.cmd
```

This builds all four binaries and leaves them at:

```
src\nativeapp\bin\OpenZDK (ARMV4I)\Release\nativeapp.exe
src\zuxhook\bin\OpenZDK (ARMV4I)\Release\zuxhook.dll
src\exploiter\bin\Zune\Debug\exploiter.exe
lyra\platform\mods-tab\reposd.exe
```

To build one target:

```
tools\build-local.cmd nativeapp
tools\build-local.cmd zuxhook
tools\build-local.cmd exploiter
```

`reposd` builds with nmake via its own script
(`lyra\platform\mods-tab\src\reposd\build_reposd.bat`). A failing devenv build writes
`BuildLog.htm` under the project's `obj\...\` directory.

## Phase 2: packaging (.NET 8 machine)

Run on Windows 10+/macOS/Linux with:

- **.NET 8** (XUIHelper compiles `.xui` scenes to `.xur`).
- **Python 3** with **Pillow** and **rsvg-convert** (bake the install-splash `.bgra`).
- A **C# compiler** (`csc` on Windows, `mcs` on unix; builds the `.ccgame` resource).

Bring the four Phase 1 binaries to their paths above (if Phase 1 ran on a different
machine, copy them over), then:

```
tools/packaging/build-release.sh        # macOS / Linux
tools\packaging\build-release.cmd       # Windows
```

This regenerates the mod artifacts from source: the gemstone class blobs
(`python -m modkit.build_gemmod_manager` and siblings, validated against `zuxhook.dll`),
the install-splash `.bgra` (`bake-installer-assets.py`), and the scene `.xur`
(XUIHelper). Then it writes both release forms:

```
dist/lyra-hd-deploykit/    a zune-deploy Deploy Kit folder
dist/lyra-hd.ccgame        an XNA game package
```

See [INSTALLING.md](INSTALLING.md) for deploying either form. The explicit command the
helpers run is:

```
python tools/packaging/build-release-packages.py \
  --exploiter <exploiter.exe> --nativeapp <nativeapp.exe> --zuxhook <zuxhook.dll> \
  --out dist
```

`--mods` selects which mods to bundle (default: `mods-tab wifi-power`, the Lyra system
mods; feature mods like marketplace-redirect install from the repo). For local
iteration, `--skip-mod-build` reuses on-disk `.xur`
and `--skip-artifact-regen` reuses on-disk class blobs + `.bgra`.

## Layout note

`src/` holds the Lyra platform (`nativeapp`, `zuxhook`, `exploiter`, `kerncore`, `shared`) and
the shared libraries any mod may use (`ce-common`, `mod-runtime`). `kerncore` is the kernel
R/W primitive and `shared` holds the small sources both platform binaries compile in
(`title_name`, `device_reboot`). Third-party deps are
vendored as submodules under their consumer's `deps/`: **wolfSSL** at
`src/ce-common/deps/wolfssl` (ce-common's HTTPS/TLS), and **zlib** at
`lyra/platform/mods-tab/src/reposd/deps/zlib` (reposd's `.zmod` unzip). So before a first
build, fetch them:

```
git submodule update --init --recursive
```

wolfSSL is built from source (`build.cmd` runs `src\ce-common\build\build_wolfssl.bat` if
the lib is absent) rather than committed; the result caches under `src\ce-common\out\`
(gitignored). A mod's own native source lives under the mod, e.g.
`lyra/platform/mods-tab/src/reposd/`, and reaches the shared `src/` libraries via a
relative `..\..\src` climb. The platform native projects reference the shared code via
`..\kerncore` and `..\shared`.
