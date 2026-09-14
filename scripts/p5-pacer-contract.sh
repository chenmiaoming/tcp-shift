#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p5-pacer-contract"
BINARY="$BUILD/tcp-shift-p5-pacer-contract"

mkdir -p "$BUILD"

${CC:-cc} \
    -std=c11 -D_GNU_SOURCE \
    -Wall -Wextra -Wpedantic -Werror \
    -I"$ROOT/src" \
    "$ROOT/src/runtime/pacer.c" \
    "$ROOT/src/tests/pacer_scheduler_test.c" \
    -o "$BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'pacer_contract=ok ' "$BUILD/summary.txt" >/dev/null
