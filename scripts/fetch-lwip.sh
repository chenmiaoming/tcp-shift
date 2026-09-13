#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/.lwip-baseline"

DEPS="$ROOT/.deps"
LWIP_DIR="$DEPS/lwip"
mkdir -p "$DEPS"

if [ ! -d "$LWIP_DIR/.git" ]; then
    git clone --filter=blob:none --no-checkout "$LWIP_REPOSITORY" "$LWIP_DIR"
fi

git -C "$LWIP_DIR" remote set-url origin "$LWIP_REPOSITORY"
git -C "$LWIP_DIR" fetch --depth=1 origin "$LWIP_COMMIT"
git -C "$LWIP_DIR" checkout --detach "$LWIP_COMMIT"

ACTUAL=$(git -C "$LWIP_DIR" rev-parse HEAD)
if [ "$ACTUAL" != "$LWIP_COMMIT" ]; then
    echo "lwIP baseline mismatch: expected $LWIP_COMMIT, got $ACTUAL" >&2
    exit 1
fi

printf 'lwIP baseline: %s\n' "$ACTUAL"
