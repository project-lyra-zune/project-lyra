#!/usr/bin/env sh
# Phase 2: assemble the Lyra release packages from the Phase 1 device binaries.
# Run on a machine with .NET 8 (XUIHelper), Python 3 + Pillow + rsvg-convert, and
# a C# compiler (mcs). The four Phase 1 binaries must be in place at the paths
# below (build them with build.cmd on Windows 7, then bring the built binaries
# over). Regenerates class blobs, .bgra, and .xur from source, then writes
# dist/lyra-hd-deploykit and dist/lyra-hd.ccgame. Extra args pass through.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
python3 "$ROOT/tools/packaging/build-release-packages.py" \
  --exploiter "$ROOT/src/exploiter/bin/Zune/Debug/exploiter.exe" \
  --nativeapp "$ROOT/src/nativeapp/bin/OpenZDK (ARMV4I)/Release/nativeapp.exe" \
  --zuxhook   "$ROOT/src/zuxhook/bin/OpenZDK (ARMV4I)/Release/zuxhook.dll" \
  --out "$ROOT/dist" "$@"
