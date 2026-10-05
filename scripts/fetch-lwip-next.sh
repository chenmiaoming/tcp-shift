#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPOSITORY=https://github.com/lwip-tcpip/lwip.git
DEPS="$ROOT/.deps"
BUILD="$ROOT/.build"
LWIP_DIR="$DEPS/lwip"
P4_PATCH="$ROOT/patches/lwip-p4-cc-hooks.patch"
SACK_PATCH="$ROOT/patches/lwip-sack-recovery.patch"
ECN_PATCH="$ROOT/patches/lwip-ecn.patch"

mkdir -p "$DEPS" "$BUILD"

for patch in "$P4_PATCH" "$SACK_PATCH" "$ECN_PATCH"; do
    [ -f "$patch" ] || {
        echo "missing lwIP integration patch: $patch" >&2
        exit 1
    }
done

rm -rf "$LWIP_DIR"
git clone --depth=1 "$REPOSITORY" "$LWIP_DIR"

ACTUAL=$(git -C "$LWIP_DIR" rev-parse HEAD)
cat > "$BUILD/upstream.env" <<EOF
LWIP_REPOSITORY=$REPOSITORY
LWIP_ACTUAL_COMMIT=$ACTUAL
LWIP_CANARY_MODE=current-upstream
EOF

(
    cd "$LWIP_DIR"
    sha256sum \
        src/core/tcp.c \
        src/core/tcp_in.c \
        src/core/tcp_out.c \
        src/include/lwip/tcp.h \
        src/include/lwip/priv/tcp_priv.h
) > "$BUILD/lwip-next-critical.sha256"

apply_patch()
{
    name=$1
    patch=$2

    if ! git -C "$LWIP_DIR" apply --check "$patch"; then
        echo "lwIP next canary: $name no longer applies to $ACTUAL" >&2
        git -C "$LWIP_DIR" status --short >&2 || true
        exit 1
    fi
    git -C "$LWIP_DIR" apply "$patch"
}

apply_patch P4 "$P4_PATCH"
apply_patch SACK_RACK "$SACK_PATCH"
apply_patch ECN "$ECN_PATCH"

git -C "$LWIP_DIR" diff --check
git -C "$LWIP_DIR" diff -- \
    src/core/tcp.c src/core/tcp_in.c src/core/tcp_out.c \
    > "$BUILD/lwip-next-project-patches.diff"

cat > "$BUILD/lwip-next-patches.env" <<EOF
LWIP_P4_PATCH_SHA256=$(sha256sum "$P4_PATCH" | awk '{print $1}')
LWIP_SACK_PATCH_SHA256=$(sha256sum "$SACK_PATCH" | awk '{print $1}')
LWIP_ECN_PATCH_SHA256=$(sha256sum "$ECN_PATCH" | awk '{print $1}')
EOF

cat "$BUILD/upstream.env"
cat "$BUILD/lwip-next-patches.env"
printf 'lwIP next patch chain: apply=ok commit=%s\n' "$ACTUAL"
