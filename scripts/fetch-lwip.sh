#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/.lwip-baseline"

DEPS="$ROOT/.deps"
BUILD="$ROOT/.build"
LWIP_DIR="$DEPS/lwip"
mkdir -p "$DEPS" "$BUILD"

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

cat > "$BUILD/upstream.env" <<EOF
LWIP_REPOSITORY=$LWIP_REPOSITORY
LWIP_COMMIT=$LWIP_COMMIT
LWIP_ACTUAL_COMMIT=$ACTUAL
EOF

(
    cd "$LWIP_DIR"
    sha256sum \
        src/core/tcp.c \
        src/core/tcp_in.c \
        src/core/tcp_out.c \
        src/include/lwip/tcp.h \
        src/include/lwip/priv/tcp_priv.h
) > "$BUILD/lwip-critical.sha256"

printf 'lwIP baseline: %s\n' "$ACTUAL"
