#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/bbr3-controller"
mkdir -p "$BUILD"
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin -I"$ROOT/src" \
    "$ROOT/src/cc/bbr3_model.c" \
    "$ROOT/src/cc/bbr3_probe.c" \
    "$ROOT/src/cc/bbr3_congestion.c" \
    "$ROOT/src/cc/bbr3_controller.c" \
    "$ROOT/src/tests/bbr3_controller_test.c" \
    -o "$BUILD/bbr3-controller-contract"
"$BUILD/bbr3-controller-contract" | tee "$BUILD/summary.txt"
grep -F 'bbr3_controller_contract=ok startup_drain=ok ' "$BUILD/summary.txt" >/dev/null
