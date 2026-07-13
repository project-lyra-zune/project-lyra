# Lyra host tools

Host-side scripts for driving Lyra over Wi-Fi/USB and for offline
reverse-engineering of Zune HD firmware. Most device tools dial the `nativeapp`
daemon at a default IP of `192.168.0.100` on TCP `1337`; pass `--ip` to override.

Shared client libraries live with their consumers: `general/` holds `zune_repl.py`
(the REPL client), `lyra_proto.py`, `lyra_rdfile.py`, and `lyra_lsdir.py`; `re/`
holds `lyra_debug_common.py`; `disasm/` holds `xip_paths.py`. CLIs import these by
name, so run a tool from its own
directory (or by full path). Tools outside `general/` that need the REPL client
resolve it at `../general` automatically.

The modkit CLI (`mod-apply.py`) and its package live in `../modkit`, not here.

## Dependencies

The `general/` and `re/` tools are stdlib-only. Two groups need pip packages
(see `requirements.txt`): `disasm/` needs `capstone`; `capture/` needs `numpy`
and `Pillow`.

## general/ - transport and file I/O

| Tool | Purpose |
| :--- | :--- |
| `zune_repl.py` *(lib)* | REPL client: kernel/process memory, kcall, code-patch, xexec, file ops (nativeapp opcode 40). |
| `lyra_proto.py` *(lib)* | Protobuf (proto2/nanopb) varint + field codec for the file/RPC protocol. |
| `lyra_rdfile.py` *(lib)* | Read-file wire core (protobuf command 0x10). |
| `lyra_lsdir.py` *(lib)* | Streaming lsdir wire core (protobuf command 0x10, CMD_LSDIR). |
| `lyra-write-file.py` | Push a local file to the device (opcode 12). |
| `lyra-read-file.py` | Read device files to disk or stdout (RDFILE, resume + streaming). |
| `lyra-lsdir.py` | List a device directory (CMD_LSDIR). |
| `lyra-tree.py` | Recursively list a device filesystem tree. |
| `lyra-mkdir.py` | Create a directory (opcode 13). |
| `lyra-movefile.py` | Rename / move a file (opcode 17). |
| `lyra-rmfile.py` | Delete a file (opcode 14). |
| `lyra-rmdir.py` | Recursively remove a directory tree. |
| `snapshot-device.py` | Walk, hash, and mirror the device filesystem under a size cap. |
| `zune-reachability-monitor.py` | Poll reachability via ICMP ping + the 1337 Hello banner. |
| `probe-lyra-hd-listener.sh` | Connectivity smoke-test of the port-1337 Hello banner. |

## re/ - reverse-engineering (live device)

Inspection and debug tools speak the raw debug protocol via
`lyra_debug_common.py` (opcodes 1-8, 26). The cross-process tools
(`zune-touch-poc`, `zune-registry-walk`, `read-registry`,
`zune_repl.patch_target_code`/`xexec`) use the kernel-scratch helpers that
`nativeapp.exe` plants at boot via `kerncore_plant_helpers()`; kerncore
auto-replants them if clobbered, so no host-side planting step is needed.

| Tool | Purpose |
| :--- | :--- |
| `lyra_debug_common.py` *(lib)* | Shared raw-socket debug / process-control helpers. |
| `lyra-list-processes.py` | Walk the kernel process list (opcode 1). |
| `lyra-read-process-mem.py` | Read N words from a process VA (after attach). |
| `lyra-read-kernel-u32.py` | Read one kernel u32 (opcode 1). |
| `lyra-write-process-u32.py` | Write one u32 into a process VA (opcode 4). |
| `lyra-read-thread-regs.py` | Dump each thread's ARM register context (opcode 8). |
| `lyra-sample-proc-detail.py` | Sample a process's thread + module lists. |
| `lyra-dump-process-range.py` | Dump a process VA range (bulk opcode 9; `--chunk-size 4 --fill-failures` for per-word isolation). |
| `lyra-scan-process-pages.py` | Scan a process for readable page ranges. |
| `lyra-probe.py` | Probe a process's modules + exports (opcode 19). |
| `lyra-debug-attach.py` | Attach as debugger (opcode 5). |
| `lyra-debug-wait.py` | Wait for a debug event (opcode 6). |
| `lyra-debug-continue.py` | Continue a debug event (opcode 7). |
| `lyra-breakpoint-once.py` | Set one ARM breakpoint, wait for a single hit. |
| `lyra-bp-stackwalk.py` | Breakpoint + full regs + stack dump on hit. |
| `zune-touch-poc.py` | Synthesize a tap via `XuiProcessMouseMessage` (throwaway PoC). |
| `read-registry.py` | Read one live registry value. |
| `zune-registry-walk.py` | Walk an HKLM subtree via injected registry-enum shellcode. |
| `lyra-plugin-call.py` | Invoke a plugin DLL in the daemon (opcode 20). |
| `lyra-plugin-daemon.py` | Spawn / stop a background-daemon plugin (opcodes 21 / 22). |
| `lyra-plugin-trigger.py` | Trigger zuxhook's in-host plugin loader (opcode 18). |

### re/ eMMC and storage

The block driver is `libnvemmc.dll` over the NVIDIA SDMMC DDK, and `DSK1:` is the eMMC store.
The topology and raw-sector tools drive the CE Storage Manager and the disk IOCTLs. The
front-region tools reach the 215 MiB boot reserve that sits below the store by hooking the SDMMC
driver's `NvDdkHsmmcSendCommand` and remapping a read into it.

| Tool | Purpose |
| :--- | :--- |
| `enum-storage-topology.py` | Enumerate stores + partitions via the CE Storage Manager (`FindFirstStore` / `OpenStore` / `FindFirstPartition`); JSON out. |
| `find-sdmmc-seccount.py` | Relocate the NvDdk geometry struct by signature (per-boot base) and read the eMMC `EXT_CSD` `SEC_COUNT`. |
| `read-ext-csd.py` | Read the full 512-byte eMMC `EXT_CSD` via `CMD8 SEND_EXT_CSD` (ungate the SDMMC clock through NvRm, then `NvDdkHsmmcRead` with a hand-built CMD8 pCmd); reports boot/RPMB/GP provisioning. |
| `read-raw-emmc.py` | Read sector 0 of `DSK1:` (raw eMMC) via `CreateFile` + `DeviceIoControl(0x75C08)`. |
| `read-raw-emmc-range.py` | Read a range of raw `DSK1:` logical sectors to a file. |
| `read-emmc-front.py` | Read the reserved 215 MiB front region (BCT, encrypted bootloader, CE boot/recovery images) by remapping a `DSK1:` read via a `NvDdkHsmmcSendCommand` hook; `--full` dumps it all. |
| `survey-emmc-front.py` | Entropy + structure survey across the front region. |
| `probe-hsmmc-cmd-format.py` | Capture the `NvDdkHsmmcSendCommand` pCmd struct across reads to identify the command-index and block-address fields. |
| `hsmmc_remap.asm` *(shellcode src)* | ARM remap trampoline used by `read-emmc-front.py`; assembled on the Win7 VM (armasm), capstone-verified. |

## disasm/ - ARM / CE6 static + live disassembly

Needs `capstone` (`pip install capstone`).

| Tool | Purpose |
| :--- | :--- |
| `offline-disasm-any.py` | Offline disasm + xref for any v4.5 XIP module. |
| `xip-re.py` | Cross-module static RE for CE6 XIP ROM DLLs (symbol map, xrefs, call profiling). |
| `re-helper.py` | Live-device disasm of gemstone/xuidll code with IAT-thunk + symbol annotation (symbols from a per-build JSON via `--symbols`). |
| `scan-bl-callers.py` | Find `bl` call sites targeting a given VA, with arg-setup context. |
| `zune-dump-service-rpc.py` | Decode a CE6 stream-device service DLL's RPC surface (IOCTL dispatch / jump tables) from its exports. |
| `resolve-imports.py` | Resolve a DLL's ordinal imports to names via export tables. *(stdlib-only)* |
| `wstrings.py` | Dump UTF-16LE + ASCII strings with file offsets. *(stdlib-only)* |
| `xip_paths.py` *(lib)* | Locate the extracted XIP firmware tree (`LYRA_XIP_DIR` override). |

## xui/ - scene and skin authoring / inspection

The scene compiler (`.xui` to `.xur`) is XUIHelper-zune, driven by
`../modkit/mod-apply.py build`. These are the host-side inspection helpers.

| Tool | Purpose |
| :--- | :--- |
| `xuiz-extract.py` | Extract or list (`--list`) all resources from a `.gem` (XUIZ) skin container. |
| `xuib-inspect.py` | Inspect a compiled `.xur` (XUIB): sections, STRN strings, ARGB colors. |
| `dump_all_class_slots.py` | Dump every registered XUI class + its PropDef table (the property reference). |

## capture/ - screen

Needs `numpy` + `Pillow` (`pip install numpy Pillow`).

| Tool | Purpose |
| :--- | :--- |
| `zune-screencap.py` | Capture the live screen to a PNG (framebuffer read). |
| `zune-screencast.py` | Live screen mirror + click-to-touch, via the `screencast` mod's `screencastd` daemon (opcodes 21/22, `../mods/screencast`). |
