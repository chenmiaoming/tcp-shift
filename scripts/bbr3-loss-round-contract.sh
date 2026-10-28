#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/bbr3-loss-round"
mkdir -p "$BUILD"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin -I"$ROOT/src" \
    "$ROOT/src/lwip/bbr3_loss_round.c" \
    "$ROOT/src/tests/bbr3_loss_round_test.c" \
    -o "$BUILD/bbr3-loss-round-contract"

"$BUILD/bbr3-loss-round-contract" | tee "$BUILD/summary.txt"
grep -F 'bbr3_loss_round_contract=ok compatible_flight=ok ' \
    "$BUILD/summary.txt" >/dev/null
