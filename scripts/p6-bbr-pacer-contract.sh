#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p6-bbr-pacer-contract"
BINARY="$BUILD/tcp-shift-p6-bbr-pacer-contract"

rm -rf "$BUILD"
mkdir -p "$BUILD"

${CC:-cc} \
    -std=gnu11 \
    -Wall -Wextra -Wpedantic -Werror \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr.c" \
    "$ROOT/src/cc/bbr_pacing.c" \
    "$ROOT/src/cc/transport_pacing.c" \
    "$ROOT/src/runtime/pacer.c" \
    "$ROOT/src/tests/bbr_pacer_integration_test.c" \
    -o "$BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'bbr_pacer_integration=ok nominal_rtt_ms=1 ' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'controller_rate_precedence=1 zero_catch_up=1 ' \
    "$BUILD/summary.txt" >/dev/null

printf '%s\n' \
    'bbr_initial_pacing_reference=linux-v6.17-tcp_bbr-bbr_init_pacing_rate_from_rtt' \
    'transport_pacer=shared-per-flow-virtual-clock' \
    | tee "$BUILD/reference.txt"
