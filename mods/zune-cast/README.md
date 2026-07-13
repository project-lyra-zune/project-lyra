# zune-cast

Streams the Zune HD's live decoded audio to a Chromecast device over WiFi. A boot-spawned
daemon (`castd.exe`) taps the AVP audio path, serves the stream over a local HTTP
server, and drives the receiver through the CASTV2 control channel (protobuf over
wolfSSL TLS). Casting runs only while the **Cast audio** quick-toggle is on; the
daemon holds a WiFi Awake lease for the session and mirrors on-screen volume to the
receiver through the `volume_state` subsystem. Long-press the toggle to pick the
output device from those discovered on the network.

Its networking, TLS, CA bundle, and image decode come from the tree's shared
`src/ce-common`; its kernel audio tap comes from `src/kerncore`.

## Layout

```
manifest.json                  capabilities: the Cast quick-toggle (with the
                               long-press device picker), the casting status +
                               status icon, the castd daemon
assets/*.png                   quick-toggle + status-icon art
src/plugin-castaudio/
  cast_main.cpp                castd.exe entry (boot daemon)          -> castd.exe
  plugin-castaudio.cpp         RunDaemon plugin entry (iteration)     -> plugin-castaudio.dll
  cast_core.cpp                cast session orchestration (shared core)
  castv2_client.cpp            CASTV2 receiver control over wolfSSL TLS
  http_media.cpp               local HTTP media server for the receiver
  avp_capture.cpp              live decoded-PCM tap (kerncore)
  mdns.cpp                     receiver discovery
  zdk.cpp / zme.cpp            metadata + media-engine hooks
  mod_state.c                  quick-toggle read + casting-status publish
  cast_channel.c               device-selection channel (long-press picker)
build/                         nmake makefile + build bat
```

## Two entries, one engine

`castd.exe` (`cast_main.cpp`) is the shipped daemon; `plugin-castaudio.dll`
(`plugin-castaudio.cpp`, exporting `RunDaemon`) loads the identical engine inside
the nativeapp REPL for reboot-free iteration, and serves as the SDK's advanced
`RunDaemon` plugin example (see `../../plugins/README.md`). Only `castd.exe` ships
in the `.zmod`.

## Building

Both artifacts build in a Windows 7 enviroment (Phase 1); the build links the shared
`src/ce-common` libs (wolfSSL + ce_image, built from source by the top-level
`build.cmd`) and `src/kerncore`. From `build/`:

```
build_plugin-castaudio.bat          -> castd.exe + plugin-castaudio.dll
```

Copy `castd.exe` to the mod root, then publish through the repo
(`tools/publish-repo.sh`), which packages the `.zmod`.
