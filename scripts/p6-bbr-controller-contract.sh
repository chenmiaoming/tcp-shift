#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p6-bbr-controller-contract"
BINARY="$BUILD/tcp-shift-p6-bbr-controller-contract"

rm -rf "$BUILD"
mkdir -p "$BUILD"

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr.c" \
    "$ROOT/src/cc/bbr_pacing.c" \
    "$ROOT/src/cc/bbr_drain.c" \
    "$ROOT/src/cc/bbr_probe.c" \
    "$ROOT/src/cc/bbr_controller.c" \
    "$ROOT/src/tests/bbr_controller_test.c" \
    -o "$BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'bbr_controller_lifecycle=ok modes=startup-drain-probebw-probertt-probebw ' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'public_ops=disabled loss_timeout=pending cycle_seed=external' \
    "$BUILD/summary.txt" >/dev/null

printf '%s\n' \
    'p6h_bbr_controller=internal-ack-lifecycle-only' \
    'p6h_public_registry=disabled' \
    'p6h_recovery_gate=loss-and-timeout-semantics-required-before-ops' \
    | tee "$BUILD/reference.txt"
