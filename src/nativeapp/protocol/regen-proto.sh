#!/usr/bin/env bash
# Regenerate msg.pb.{h,c} from msg.proto + msg.options.
# Generator version must match the vendored runtime, or the descriptors diverge.
set -euo pipefail

PINNED_NANOPB=0.4.9.1
cd "$(dirname "$0")"

command -v protoc >/dev/null || { echo "protoc not found on PATH" >&2; exit 1; }

have=$(python3 -c 'import importlib.metadata as m; print(m.version("nanopb"))' 2>/dev/null || true)
if [ "$have" != "$PINNED_NANOPB" ]; then
    echo "nanopb $PINNED_NANOPB required, found '${have:-none}'." >&2
    echo "Install it:  pip install nanopb==$PINNED_NANOPB" >&2
    exit 1
fi

gen=$(python3 -c 'import os,nanopb; print(os.path.join(os.path.dirname(nanopb.__file__),"generator","nanopb_generator.py"))')

# -L: quote-include, since pb.h sits beside the generated files.
python3 "$gen" -L '#include "%s"' msg.proto
echo "Regenerated msg.pb.h and msg.pb.c"
