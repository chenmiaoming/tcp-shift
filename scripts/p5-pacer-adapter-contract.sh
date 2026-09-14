#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p5-pacer-adapter-contract"
CC=${CC:-cc}

[ -f "$BUILD/libtcp_shift_lwip_cc_adapter.a" ] || {
    echo "missing built lwIP CC adapter archive" >&2
    exit 1
}
[ -f "$BUILD/src/cc/libtcp_shift_cc.a" ] || {
    echo "missing built CC archive" >&2
    exit 1
}
[ -f "$BUILD/libtcp_shift_lwip.a" ] || {
    echo "missing built lwIP archive" >&2
    exit 1
}

"$CC" -std=gnu11 -Wall -Wextra -Wpedantic -Werror \
    -I"$ROOT/src" \
    -I"$ROOT/.deps/lwip/src/include" \
    -I"$ROOT/.deps/lwip/contrib/ports/unix/port/include" \
    "$ROOT/src/tests/pacer_adapter_lifecycle_test.c" \
    "$ROOT/src/platform.c" \
    "$BUILD/libtcp_shift_lwip_cc_adapter.a" \
    "$BUILD/src/cc/libtcp_shift_cc.a" \
    "$BUILD/libtcp_shift_lwip.a" \
    -o "$OUT"

"$OUT"
