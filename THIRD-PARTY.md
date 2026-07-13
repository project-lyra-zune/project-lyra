# Third-party code and provenance

Project Lyra is MIT-licensed (see [LICENSE](LICENSE)). It builds on the following
upstream work and vendors the following third-party components, each under its own
terms.

## Upstream

### zuneslayer - by CUB3D
<https://github.com/magicisinthehole/zuneslayer>

Lyra's `nativeapp` is adapted from **zuneslayer**, the Zune kernel-exploit suite by
**CUB3D**. CUB3D's work provides the arbitrary kernel read/write
primitive in `kerncore` (the `libnmvwavedev.dll` bug that repurposes `GetExitCodeThread`
→ `GetFSHeapInfo`), the Wi-Fi RPC daemon and its opcode protocol, and the code-execution
entrypoints Lyra builds on, including the XNA/EDT kernel exploit and the alternative CVE-2019-1367
browser ROP chain.

### OpenZDK
<http://zunedevwiki.org/>

Two things come from OpenZDK (Release 1, 2010-04-14, by *itsnotabigtruck*). Its Zune HD
headers and import libraries are the ARMV4I SDK / toolchain required to build `nativeapp`
and `zuxhook` (Simplified BSD; see [BUILDING.md](BUILDING.md)). Its project **template** is
what the native path was first built on: the `nativeapp` skeleton (`nativeapp.vcproj` +
`wWinMain`), and the XNA "exploiter" delivery launcher (`ZuneBoards.DevelopmentFront.NativeAppLauncher`),
now vendored in `src/exploiter` so the whole project builds from this repo. That template is
dedicated to the public domain: its banner reads *"Copyright (c) 2010 itsnotabigtruck. No rights
reserved."* (CC0).

### CodePug - Zune Web Server (BSD 2-Clause)
<https://www.codepug.com/wiki/zune-web-server>

nativeapp's socket accept-loop scaffold was seeded from CodePug's "Zune Web Server" (the
`Server()` / `connection()` loop in its `httpd.h`), reaching Lyra via zuneslayer; the
`SuppressReboot()` helper in `nativeapp/xutility.h` is kept from it verbatim. The verbatim
CodePug helpers that once rode along (`getIpAddress`, `MultiCharToUniChar`, the "CodePug
WebServer Started" banner) have been removed, and the connection handler here is an RPC
opcode dispatcher, not CodePug's HTTP handler.

CodePug's source headers carry the **BSD 2-Clause** license (its wiki page's "public domain"
note notwithstanding, the in-file terms govern). The full notice is retained in
[`nativeapp/CodePug-LICENSE.txt`](nativeapp/CodePug-LICENSE.txt), and the `Copyright (c) 2010
CodePug` line is kept in `nativeapp/xutility.h`.

### CVE-2019-1367
<https://googleprojectzero.github.io/0days-in-the-wild//0day-RCAs/2019/CVE-2019-1367.html>

The jscript engine vulnerability behind zuneslayer's browser entrypoint. Referenced
for lineage only; the chain itself lives in zuneslayer.

## Vendored components (kept in-tree, under their own licenses)

| Component | Location | License | License file |
| :--- | :--- | :--- | :--- |
| **nanopb** (runtime) | `nativeapp/protocol/pb*.{c,h}` | zlib (Petteri Aimonen) | `nativeapp/protocol/LICENSE.txt` |
| **Khronos EGL/GLES2 headers** | `nativeapp/khronos/` | MIT (Khronos Group) | in each header |
| **CodePug Zune Web Server** | `nativeapp/xutility.h` | BSD 2-Clause (CodePug) | `nativeapp/CodePug-LICENSE.txt` |
| **XNA exploiter** (NativeAppLauncher) | `src/exploiter/` | Public domain / CC0 (itsnotabigtruck) | banner in each `.cs` |

`nativeapp/protocol/msg.pb.*` and `msg.proto` are Lyra's own protobuf schema and its
nanopb-generated output, not part of nanopb.
