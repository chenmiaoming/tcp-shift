#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/bbr3-probe"
mkdir -p "$BUILD"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin -I"$ROOT/src" \
    "$ROOT/src/cc/bbr3_model.c" \
    "$ROOT/src/cc/bbr3_probe.c" \
    "$ROOT/src/tests/bbr3_probe_test.c" \
    -o "$BUILD/bbr3-probe-contract"

"$BUILD/bbr3-probe-contract" | tee "$BUILD/summary.txt"
grep -F 'bbr3_probe_contract=ok cycle=down,cruise,refill,up ' \
    "$BUILD/summary.txt" >/dev/null
