#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p6-bbr-startup-policy-contract"
BINARY="$BUILD/tcp-shift-p6-bbr-startup-policy-contract"

rm -rf "$BUILD"
mkdir -p "$BUILD"

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr.c" \
    "$ROOT/src/tests/bbr_startup_policy_test.c" \
    -o "$BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'bbr_startup_policy=ok gain=739/256 pacing_margin=99/100 ' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'bdp_rounding=ceil min_cwnd_packets=4 saturation=ok' \
    "$BUILD/summary.txt" >/dev/null

printf 'p6e_bbr_core_reference=linux-mainline-tcp_bbr\n' \
    | tee "$BUILD/reference.txt"
