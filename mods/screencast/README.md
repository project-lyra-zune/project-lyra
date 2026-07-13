# screencast

Mirrors the Zune HD's screen over WiFi and lets you control it remotely. A
boot-spawned daemon (`screencastd.exe`) reads the live display front buffer from
the NvRm carveout (via kerncore), and serves it one of two ways, chosen from the
quick-toggle's long-press picker:

- **Browser** (default): an MJPEG stream over HTTP. Open `http://<device-ip>:8080`
  in any browser to watch live; click or drag on the image to control the device
  (taps are injected through the native ZAM input ring). The quick-toggle row
  shows the address, e.g. `View at 192.168.0.100:8080`.
- **Desktop**: the binary RGB565 delta protocol the `zune-screencast.py` desktop
  client speaks, on port 1339.

Sharing runs only while the **Screen share** quick-toggle is on; the daemon holds
a WiFi Awake lease for the session. The `sharing` status shows Off / Ready (server
up, no client) / Live (a client is connected).

Its JPEG encode comes from the shared `src/ce-common` ce_image (native imaging.dll);
its ModStateBlock toggle/status and the picker channel come from the shared
`src/mod-runtime` (mod_state + mod_list_channel); its framebuffer read and touch
injection from the canonical `src/kerncore`.

## Layout

```
manifest.json                capabilities: the Screen share quick-toggle (with the
                             Browser/Desktop mode picker), the sharing status +
                             icon, the screencastd daemon
assets/*.png                 quick-toggle + status-icon art
src/screencast/
  screencast_main.cpp        screencastd.exe entry: toggle-gated, picker-driven  -> screencastd.exe
  plugin_screencast.cpp      RunDaemon plugin entry (both frontends, dev/SDK)     -> plugin-screencast.dll
  screencast_engine.cpp      framebuffer capture + RGB565 delta + ZAM touch (shared core)
  screencast_http.cpp        browser frontend: MJPEG stream, viewer page, tap POST
  screencast_delta.cpp       desktop frontend: the zune-screencast.py binary protocol
  screencast_serve.cpp       run-both helper (used by the picker-less plugin entry)
  viewer_html.h              the embedded browser viewer page
  screencast_keys.h          ModStateBlock keys + serving config
build/                       nmake makefile + build bat
```

## Two entries, one engine

`screencastd.exe` is the shipped daemon; `plugin-screencast.dll` (exporting
`RunDaemon`) loads the same engine inside the nativeapp REPL for reboot-free
iteration, and is the SDK's screen-mirror plugin example. Only `screencastd.exe`
ships in the `.zmod`.

## Building

Both artifacts build on the Windows-CE VM (Phase 1); the build links the shared
`src/ce-common` ce_image lib (built from source by the top-level `build.cmd`),
`src/mod-runtime`, and `src/kerncore`. From `build/`:

```
build_screencast.bat                -> screencastd.exe + plugin-screencast.dll
```

Copy `screencastd.exe` to the mod root, then publish through the repo
(`tools/publish-repo.sh`), which packages the `.zmod`.
