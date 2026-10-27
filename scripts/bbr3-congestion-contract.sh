#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/bbr3-congestion"
mkdir -p "$BUILD"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin -I"$ROOT/src" \
    "$ROOT/src/cc/bbr3_model.c" \
    "$ROOT/src/cc/bbr3_congestion.c" \
    "$ROOT/src/tests/bbr3_congestion_test.c" \
    -o "$BUILD/bbr3-congestion-contract"

"$BUILD/bbr3-congestion-contract" | tee "$BUILD/summary.txt"
grep -F 'bbr3_congestion_contract=ok loss_round_once=ok ' \
    "$BUILD/summary.txt" >/dev/null
