#!/usr/bin/env sh
# Publish a Lyra mod catalog: regenerate feed.json + the .zmod packages from every
# feature mod (system mods excluded), then deploy the static site (repo/) to your
# repo host over rsync+ssh and rebuild its docker-compose container. A feature mod
# with native components must have its device binaries built (Phase 1) and in place
# before publishing; see BUILDING.md.
#
# Configure via env:
#   LYRA_REPO_HOST      required. ssh target, e.g. user@repo.example.com
#   LYRA_REPO_STACK     required. remote path holding the compose.yaml + content/
#   LYRA_REPO_BASE_URL  optional. base URL baked into the feed (default below)
#   LYRA_INSTALL_URL    optional. plaintext host for the browser install (default below)
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

BASE_URL=${LYRA_REPO_BASE_URL:-https://repo.zune.moe}
INSTALL_URL=${LYRA_INSTALL_URL:-http://install.zune.moe}
HOST=${LYRA_REPO_HOST:-}
STACK=${LYRA_REPO_STACK:-}
if [ -z "$HOST" ] || [ -z "$STACK" ]; then
  echo "publish-repo: set LYRA_REPO_HOST (user@host) and LYRA_REPO_STACK (remote path)." >&2
  echo "  optional: LYRA_REPO_BASE_URL (default $BASE_URL)" >&2
  exit 2
fi

echo ">> regenerating feed + packages from the feature catalog"
python3 "$ROOT/modkit/mod-apply.py" feed --all \
  --out "$ROOT/repo/content" --base-url "$BASE_URL"

FEED="$ROOT/repo/content/feed.json"
PIN_HDR="$ROOT/src/lyraboot/boot_pin.h"
BOOT_EXE="$ROOT/src/lyraboot/bin/lyraboot.exe"

echo ">> pinning the browser bootstrap to the published platform"
python3 "$ROOT/tools/stamp-lyraboot-pin.py" \
  --feed "$FEED" --header "$PIN_HDR" --base-url "$INSTALL_URL"

PLAT_VER=$(python3 -c 'import json,sys;print(next(m["version"] for m in json.load(open(sys.argv[1]))["mods"] if m["mod_id"]=="lyra"))' "$FEED")
PLAT_SHA=$(python3 -c 'import json,sys;print(next(m["sha256"] for m in json.load(open(sys.argv[1]))["mods"] if m["mod_id"]=="lyra"))' "$FEED")

echo ">> building the install page (platform $PLAT_VER)"
python3 "$ROOT/install/build-page.py" \
  --version "$PLAT_VER" \
  --pin-url "$INSTALL_URL/lyra-$PLAT_VER.zmod" \
  --pin-sha "$PLAT_SHA" \
  --image "$BOOT_EXE" \
  -o "$ROOT/repo/content/install"

# The device fetches the bundle from the plaintext host, not from the HTTPS repo it
# cannot negotiate, so it is served from here as well as from /mods/.
cp "$ROOT/repo/content/mods/lyra-$PLAT_VER.zmod" "$ROOT/repo/content/install/lyra-$PLAT_VER.zmod"

echo ">> deploying repo to $HOST:$STACK"
rsync -az --delete --exclude .git --exclude .gitignore \
  "$ROOT/repo/" "$HOST:$STACK/"

echo ">> rebuilding the container"
ssh "$HOST" "cd '$STACK' && sudo -n docker compose up -d --build"

echo ">> published: $BASE_URL/feed.json"
echo ">> published: $INSTALL_URL/ (browser install, platform $PLAT_VER)"
