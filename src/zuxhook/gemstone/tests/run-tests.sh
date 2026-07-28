#!/usr/bin/env bash
# Host tests for the windows-free half of the gemstone mod UI. No device and no
# VS2008: the scenes themselves are full of engine addresses, so anything worth
# testing lives in a translation unit that has none.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
gem="$(cd "$here/.." && pwd)"
ipc="$(cd "$gem/../../../lyra/platform/mods-tab/src/reposd" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

: "${CC:=cc}"
flags="-std=c89 -Wall -Wextra -pedantic"

$CC $flags -I"$gem" -I"$ipc" -o "$out/test_browse_status" \
    "$here/test_browse_status.c" "$gem/browse_status.c"
"$out/test_browse_status"
