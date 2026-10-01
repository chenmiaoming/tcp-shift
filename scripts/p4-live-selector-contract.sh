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
for archive in \
    "$BUILD/libtcp_shift_lwip_tcp_memory.a" \
    "$BUILD/libtcp_shift_rack_tlp.a" \
    "$BUILD/libtcp_shift_prr.a"
do
    [ -f "$archive" ] || {
        echo "missing built archive: $archive" >&2
        exit 1
    }
done

"$CC" -std=gnu11 -Wall -Wextra -Wpedantic -Werror \
    -DTCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE=1 \
    -DTCP_SHIFT_EXPERIMENTAL_RACK_TLP=1 \
    -DTCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE=1 \
    -I"$ROOT/src" \
    -I"$ROOT/.deps/lwip/src/include" \
    -I"$ROOT/.deps/lwip/contrib/ports/unix/port/include" \
    "$ROOT/src/tests/cc_live_selector_test.c" \
    "$ROOT/src/platform.c" \
    "$BUILD/libtcp_shift_lwip_cc_adapter.a" \
    "$BUILD/libtcp_shift_lwip_tcp_memory.a" \
    "$BUILD/libtcp_shift_rack_tlp.a" \
    "$BUILD/libtcp_shift_prr.a" \
    "$BUILD/src/cc/libtcp_shift_cc.a" \
    "$BUILD/libtcp_shift_lwip.a" \
    "$BUILD/libtcp_shift_pacer.a" \
    -o "$OUT"

"$OUT" | tee "$BUILD/p4-live-selector-summary.txt"
grep -F 'cc_live_selector=ok selected=cubic ack_observations=2 srtt_updates=2 ' \
    "$BUILD/p4-live-selector-summary.txt" >/dev/null
