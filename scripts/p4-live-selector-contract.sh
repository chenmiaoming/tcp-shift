#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p4-live-selector-contract"
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
[ -f "$BUILD/libtcp_shift_pacer.a" ] || {
    echo "missing built pacer archive" >&2
    exit 1
}

"$CC" -std=gnu11 -Wall -Wextra -Wpedantic -Werror \
    -I"$ROOT/src" \
    -I"$ROOT/.deps/lwip/src/include" \
    -I"$ROOT/.deps/lwip/contrib/ports/unix/port/include" \
    "$ROOT/src/tests/cc_live_selector_test.c" \
    "$ROOT/src/platform.c" \
    "$BUILD/libtcp_shift_lwip_cc_adapter.a" \
    "$BUILD/src/cc/libtcp_shift_cc.a" \
    "$BUILD/libtcp_shift_lwip.a" \
    "$BUILD/libtcp_shift_pacer.a" \
    -o "$OUT"

"$OUT" | tee "$BUILD/p4-live-selector-summary.txt"
grep -F 'cc_live_selector=ok selected=cubic ack_observations=1 srtt_updates=1 ' \
    "$BUILD/p4-live-selector-summary.txt" >/dev/null
