#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p6-delivery-snapshot-contract"
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
    "$ROOT/src/tests/bbr_delivery_snapshot_test.c" \
    "$ROOT/src/platform.c" \
    "$BUILD/libtcp_shift_lwip_cc_adapter.a" \
    "$BUILD/src/cc/libtcp_shift_cc.a" \
    "$BUILD/libtcp_shift_lwip.a" \
    "$BUILD/libtcp_shift_pacer.a" \
    -o "$OUT"

"$OUT" | tee "$BUILD/p6-delivery-snapshot-summary.txt"
grep -F 'bbr_delivery_snapshot=ok ' \
    "$BUILD/p6-delivery-snapshot-summary.txt" >/dev/null
grep -F 'errors=0 ' "$BUILD/p6-delivery-snapshot-summary.txt" >/dev/null
grep -F 'retransmitted_samples=3 retransmit_events=3 ' \
    "$BUILD/p6-delivery-snapshot-summary.txt" >/dev/null
grep -F 'sack_events=1 ' "$BUILD/p6-delivery-snapshot-summary.txt" >/dev/null
grep -E 'sack_payload_bytes=[1-9][0-9]* ' \
    "$BUILD/p6-delivery-snapshot-summary.txt" >/dev/null
