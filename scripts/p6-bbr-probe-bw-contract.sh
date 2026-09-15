#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p6-bbr-probe-bw-contract"
BINARY="$BUILD/tcp-shift-p6-bbr-probe-bw-contract"

rm -rf "$BUILD"
mkdir -p "$BUILD"

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr.c" \
    "$ROOT/src/cc/bbr_probe_bw.c" \
    "$ROOT/src/tests/bbr_probe_bw_test.c" \
    -o "$BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'bbr_probe_bw=ok cycle=5/4,3/4,1,1,1,1,1,1 ' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'cwnd_gain=2 pacing_margin=99/100 timer=min_rtt inflight=prior_bytes' \
    "$BUILD/summary.txt" >/dev/null

printf '%s\n' \
    'p6g_bbr_core_reference=linux-mainline-tcp_bbr' \
    'p6g_cycle_state=controller-owned' \
    'p6g_random_start=caller-owned' \
    'p6g_external_symbol_gate=p4-whole-archive' \
    | tee "$BUILD/reference.txt"
