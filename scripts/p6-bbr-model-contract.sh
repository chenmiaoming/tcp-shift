#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p6-bbr-model-contract"
BINARY="$BUILD/tcp-shift-p6-bbr-model-contract"

rm -rf "$BUILD"
mkdir -p "$BUILD"

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr.c" \
    "$ROOT/src/tests/bbr_model_test.c" \
    -o "$BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'bbr_model_contract=ok mode=startup ' "$BUILD/summary.txt" >/dev/null
grep -F 'filter_rounds=10 ' "$BUILD/summary.txt" >/dev/null
grep -F 'ignored_app_limited=1 ' "$BUILD/summary.txt" >/dev/null
grep -F 'full_bw_count=3 full_bw_reached=1 ' "$BUILD/summary.txt" >/dev/null
grep -F 'round_count=6 ' "$BUILD/summary.txt" >/dev/null

printf '%s\n' \
    'p6_bbr_core_reference=linux-mainline-tcp_bbr' \
    'p6_bbr_secondary_reference=draft-ietf-ccwg-bbr-06' \
    | tee "$BUILD/reference.txt"
