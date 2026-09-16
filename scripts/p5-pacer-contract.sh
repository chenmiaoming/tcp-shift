#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p5-pacer-contract"
BINARY="$BUILD/tcp-shift-p5-pacer-contract"
QUANTUM_BINARY="$BUILD/tcp-shift-flow-pacer-quantum-contract"

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

${CC:-cc} \
    -std=c11 -D_GNU_SOURCE \
    -Wall -Wextra -Wpedantic -Werror \
    -I"$ROOT/src" \
    "$ROOT/src/runtime/pacer.c" \
    "$ROOT/src/tests/flow_pacer_quantum_test.c" \
    -o "$QUANTUM_BINARY"

"$QUANTUM_BINARY" | tee "$BUILD/quantum-summary.txt"
grep -F 'flow_pacer_quantum=ok quantum_bytes=2000 batch_segments=2 ' \
    "$BUILD/quantum-summary.txt" >/dev/null
grep -F 'long_term_spacing_ms=2 stale_credit_cleared=1 strict_mode=1' \
    "$BUILD/quantum-summary.txt" >/dev/null
