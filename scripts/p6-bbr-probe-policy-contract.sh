#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p6-bbr-probe-policy-contract"
BINARY="$BUILD/tcp-shift-p6-bbr-probe-policy-contract"

rm -rf "$BUILD"
mkdir -p "$BUILD"

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr.c" \
    "$ROOT/src/cc/bbr_probe.c" \
    "$ROOT/src/tests/bbr_probe_policy_test.c" \
    -o "$BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'bbr_probe_policy=ok cycle=320,192,256 cwnd_gain=512/256 ' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'probe_rtt=200ms transition=probebw-probertt-probebw cycle_seed=external' \
    "$BUILD/summary.txt" >/dev/null

printf '%s\n' \
    'p6g_bbr_core_reference=linux-mainline-tcp_bbr' \
    'p6g_probebw_cycle=1.25,0.75,1,1,1,1,1,1' \
    'p6g_probertt_contract=min-rtt-expiry+four-packets+200ms+one-round' \
    'p6g_cycle_randomness=caller-supplied-seed' \
    | tee "$BUILD/reference.txt"
