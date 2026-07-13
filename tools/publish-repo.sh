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
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

BASE_URL=${LYRA_REPO_BASE_URL:-https://repo.zune.moe}
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

echo ">> deploying repo to $HOST:$STACK"
rsync -az --delete --exclude .git --exclude .gitignore \
  "$ROOT/repo/" "$HOST:$STACK/"

echo ">> rebuilding the container"
ssh "$HOST" "cd '$STACK' && sudo -n docker compose up -d --build"

echo ">> published: $BASE_URL/feed.json"
