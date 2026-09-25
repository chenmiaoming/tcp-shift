#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/.lwip-baseline"

DEPS="$ROOT/.deps"
BUILD="$ROOT/.build"
LWIP_DIR="$DEPS/lwip"
PATCH="$ROOT/patches/lwip-p4-cc-hooks.patch"
SACK_PATCH="$ROOT/patches/lwip-sack-recovery.patch"
mkdir -p "$DEPS" "$BUILD"

if [ ! -d "$LWIP_DIR/.git" ]; then
    git clone --filter=blob:none --no-checkout "$LWIP_REPOSITORY" "$LWIP_DIR"
fi

[ -f "$PATCH" ] || {
    echo "missing lwIP integration patch: $PATCH" >&2
    exit 1
}
[ -f "$SACK_PATCH" ] || {
    echo "missing lwIP SACK recovery patch: $SACK_PATCH" >&2
    exit 1
}

git -C "$LWIP_DIR" remote set-url origin "$LWIP_REPOSITORY"
git -C "$LWIP_DIR" fetch --depth=1 origin "$LWIP_COMMIT"
git -C "$LWIP_DIR" checkout --detach "$LWIP_COMMIT"
# The checkout is a generated dependency workspace. Reset any previously
# applied project patch so repeated builds always start from the exact pin.
git -C "$LWIP_DIR" reset --hard "$LWIP_COMMIT" >/dev/null

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

# Record the pristine pinned sources before applying the project integration
# patch. This preserves upstream provenance independently from tcp-shift's
# intentionally narrow CC hook surface.
(
    cd "$LWIP_DIR"
    sha256sum \
        src/core/tcp.c \
        src/core/tcp_in.c \
        src/core/tcp_out.c \
        src/include/lwip/tcp.h \
        src/include/lwip/priv/tcp_priv.h
) > "$BUILD/lwip-critical.sha256"

PATCH_SHA256=$(sha256sum "$PATCH" | awk '{print $1}')
SACK_PATCH_SHA256=$(sha256sum "$SACK_PATCH" | awk '{print $1}')
git -C "$LWIP_DIR" apply --check "$PATCH"
git -C "$LWIP_DIR" apply "$PATCH"
if ! git -C "$LWIP_DIR" apply --check "$SACK_PATCH"; then
    echo "sender SACK patch context after P4:" >&2
    nl -ba "$LWIP_DIR/src/core/tcp_out.c" | sed -n '1635,1670p' >&2
    exit 1
fi
git -C "$LWIP_DIR" apply "$SACK_PATCH"
git -C "$LWIP_DIR" diff --check
cat > "$BUILD/lwip-patch.env" <<EOF
LWIP_PATCH=patches/lwip-p4-cc-hooks.patch
LWIP_PATCH_SHA256=$PATCH_SHA256
LWIP_SACK_PATCH=patches/lwip-sack-recovery.patch
LWIP_SACK_PATCH_SHA256=$SACK_PATCH_SHA256
EOF

git -C "$LWIP_DIR" diff -- src/core/tcp.c src/core/tcp_in.c src/core/tcp_out.c \
    > "$BUILD/lwip-p4-cc-hooks.applied.diff"
cp "$BUILD/lwip-p4-cc-hooks.applied.diff" "$BUILD/lwip-project-patches.applied.diff"

printf 'lwIP baseline: %s\n' "$ACTUAL"
printf 'lwIP P4 hook patch: %s\n' "$PATCH_SHA256"
printf 'lwIP sender SACK patch: %s\n' "$SACK_PATCH_SHA256"
