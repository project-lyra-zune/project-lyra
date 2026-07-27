#!/usr/bin/env bash
# Assemble the stub, then run it under an ARM emulator with the coredll calls
# intercepted. Needs clang and unicorn, installed into a cached venv.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
stub="$(cd "$here/.." && pwd)"
venv="$here/.venv"

python3 "$stub/build-stub.py"

if [ ! -x "$venv/bin/python" ]; then
    echo ">> creating test venv (unicorn)"
    python3 -m venv "$venv"
    "$venv/bin/pip" install --quiet unicorn
fi

"$venv/bin/python" "$here/test_stub_emulation.py"
