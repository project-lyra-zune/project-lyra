#!/usr/bin/env bash
# Host tests for the windows-free half of the mod runtime. No device and no
# VS2008: these run anywhere with a C compiler, which is the point of keeping
# the logic and the flash I/O in separate translation units.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
src="$(cd "$here/.." && pwd)"
fmt="$(cd "$src/../zuxhook/formats" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

: "${CC:=cc}"
flags="-std=c89 -Wall -Wextra"

$CC $flags -I"$src" -o "$out/test_boot_state" \
    "$here/test_boot_state.c" \
    "$src/boot_state.c" "$src/mod_fault.c" "$src/mods_arena.c" "$src/mods_json.c"
"$out/test_boot_state"

$CC $flags -I"$src" -I"$fmt" -o "$out/test_apply_transaction" \
    "$here/test_apply_transaction.c" \
    "$fmt/mods_compose_ckpt.c" "$src/failed_set.c" "$src/mods_arena.c"
"$out/test_apply_transaction"
