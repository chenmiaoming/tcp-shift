#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/bbr3-rack-loss"
mkdir -p "$BUILD"

${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin -I"$ROOT/src" \
    "$ROOT/src/lwip/rack_tlp.c" \
    "$ROOT/src/lwip/bbr3_rack_evidence.c" \
    "$ROOT/src/tests/bbr3_rack_evidence_test.c" \
    -o "$BUILD/bbr3-rack-loss-contract"

"$BUILD/bbr3-rack-loss-contract" | tee "$BUILD/summary.txt"
grep -F 'bbr3_rack_loss_contract=ok mature_rack_only=ok ' \
    "$BUILD/summary.txt" >/dev/null
