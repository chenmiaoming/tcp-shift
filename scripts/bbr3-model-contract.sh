#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/bbr3-model"
mkdir -p "$BUILD"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin -I"$ROOT/src" \
    "$ROOT/src/cc/bbr3_model.c" \
    "$ROOT/src/tests/bbr3_model_test.c" \
    -o "$BUILD/bbr3-model-contract"

"$BUILD/bbr3-model-contract" | tee "$BUILD/summary.txt"
grep -F 'bbr3_model_contract=ok separate_v1=ok cycle_filter=2 ' \
    "$BUILD/summary.txt" >/dev/null
