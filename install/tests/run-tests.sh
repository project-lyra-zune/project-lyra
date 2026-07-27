#!/usr/bin/env bash
# Host tests for the browser install: the stub's own tests, then the page/stub handoff.
# Needs clang, node and unicorn (installed into a cached venv by the stub's runner).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
install="$(cd "$here/.." && pwd)"

"$install/stub/tests/run-tests.sh"

echo
command -v node >/dev/null || { echo "node not found; it runs the page's own decoder" >&2; exit 1; }
"$install/stub/tests/.venv/bin/python" "$here/test_install_chain.py"
