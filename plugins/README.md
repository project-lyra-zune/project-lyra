# plugins

Device plugin DLLs the nativeapp daemon (and zuxhook's in-host loader) load at
runtime. A plugin is a small WinCE ARM DLL that exports one or both entry shapes
and can call the shared kernel R/W core directly.

`plugin-hello` is a template. Current real-world plugin examples live with their mod,
because each shares an engine with the mod's shipped daemon rather than being
copied here: the screen-mirror `RunDaemon` plugin is `plugin-screencast.dll` in
`mods/screencast/` (shared with `screencastd.exe`, driven by
`tools/capture/zune-screencast.py`), and a heavyweight `RunDaemon` example (TLS,
live audio capture, mDNS, an HTTP server) is `plugin-castaudio.cpp` in
`mods/zune-cast/` (shared with `castd.exe`).

## Entry contract

A plugin exports one or more of these, depending on which loader path it targets:

```c
// One-shot, daemon side (opcode 20). Reads optional arg bytes, writes a response
// into out, returns 0 on success.
extern "C" __declspec(dllexport) int Run(
    const void* arg, int arg_len, void* out, int out_max, int* out_used);

// Fire-and-forget, gemstone side (opcode 18). Activation inside the UI host.
extern "C" __declspec(dllexport) int Activate(void);

// Long-running background daemon (opcode 21). Runs on a nativeapp-tracked thread
// until stop_event is signalled (opcode 22), then returns.
extern "C" __declspec(dllexport) int RunDaemon(
    const void* arg, int arg_len, HANDLE stop_event);
```

`plugin-hello` implements `Run` + `Activate` so one DLL exercises both one-shot
paths. The screencast plugin implements `RunDaemon`: it serves its own socket in a
loop and polls `stop_event` each iteration.

### Stopping / deactivating

There is no separate `Deactivate` export. A `RunDaemon` plugin is deactivated
**cooperatively**: opcode 22 signals the `stop_event` HANDLE passed into
`RunDaemon`, and the plugin must observe it (poll with `WaitForSingleObject(...,
0)` in its loop, or wait on it in `select`/`accept` timeouts) and return promptly,
releasing any threads and sockets it owns on the way out. `Run` and `Activate`
plugins are one-shot and need no stop path. Winsock lifetime is owned by nativeapp,
so a daemon plugin must not `WSAStartup`/`WSACleanup`.

The loader tools are `tools/general/lyra-plugin-call.py` (opcode 20, one-shot
`Run`), `tools/general/lyra-plugin-daemon.py` (opcodes 21/22, spawn/stop a
`RunDaemon` plugin), and `tools/general/lyra-plugin-trigger.py` (opcode 18,
zuxhook's in-host `Activate`).

## Authoring a new plugin

1. Copy `plugin-hello/` to `plugin-<name>/` and rename the `.cpp` and `.vcproj`.
2. Keep the `..\..\src\kerncore` reference in the `.vcproj` so a plain clone
   resolves the shared core with no path setup. Include `kerncore` headers to use
   the arbitrary kernel R/W primitive.
3. Implement whichever entry shape fits: `Run` (one-shot), `Activate` (in-host
   fire-and-forget), or `RunDaemon` (background; honor `stop_event`).

## Build

From a Windows shell in the repo root (see `../BUILDING.md` for the toolchain):

```
tools\build-local.cmd plugin <name>
```

This builds `plugins\plugin-<name>\plugin-<name>.vcproj` with the
`Release|OpenZDK (ARMV4I)` configuration, leaving the artifact at:

```
plugins\plugin-<name>\bin\OpenZDK (ARMV4I)\Release\plugin-<name>.dll
```
