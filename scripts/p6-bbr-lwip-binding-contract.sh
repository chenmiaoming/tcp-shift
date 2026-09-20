#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p6-bbr-lwip-binding-contract"
CC=${CC:-cc}

for archive in \
    "$BUILD/libtcp_shift_lwip_cc_adapter.a" \
    "$BUILD/libtcp_shift_lwip_tcp_memory.a" \
    "$BUILD/src/cc/libtcp_shift_cc.a" \
    "$BUILD/libtcp_shift_lwip.a" \
    "$BUILD/libtcp_shift_pacer.a"
do
    [ -f "$archive" ] || {
        echo "missing built archive: $archive" >&2
        exit 1
    }
done

"$CC" -std=gnu11 -Wall -Wextra -Wpedantic -Werror \
    -I"$ROOT/src" \
    -I"$ROOT/.deps/lwip/src/include" \
    -I"$ROOT/.deps/lwip/contrib/ports/unix/port/include" \
    "$ROOT/src/tests/bbr_lwip_binding_test.c" \
    "$ROOT/src/platform.c" \
    "$BUILD/libtcp_shift_lwip_cc_adapter.a" \
    "$BUILD/libtcp_shift_lwip_tcp_memory.a" \
    "$BUILD/src/cc/libtcp_shift_cc.a" \
    "$BUILD/libtcp_shift_lwip.a" \
    "$BUILD/libtcp_shift_pacer.a" \
    -o "$OUT"

"$OUT" | tee "$BUILD/p6-bbr-lwip-binding-summary.txt"
grep -F 'bbr_lwip_binding=ok public_registry=disabled sidecar=pcb-ext-2 ' \
    "$BUILD/p6-bbr-lwip-binding-summary.txt" >/dev/null
grep -F 'ack_delivery_sample=ok pacing=nonzero scheduler_exec=ok sndbuf_hint=3xcwnd passive_open_hint=deferred recovery=controller-owned' \
    "$BUILD/p6-bbr-lwip-binding-summary.txt" >/dev/null
grep -F 'rto_post_loss_inflight=0 pacing_after_rto=preserved' \
    "$BUILD/p6-bbr-lwip-binding-summary.txt" >/dev/null
