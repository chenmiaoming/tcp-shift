#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p6-bbr-controller-contract"
BINARY="$BUILD/tcp-shift-p6-bbr-controller-contract"
RECOVERY_BINARY="$BUILD/tcp-shift-p6-bbr-recovery-contract"

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

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr_recovery.c" \
    "$ROOT/src/tests/bbr_recovery_test.c" \
    -o "$RECOVERY_BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'bbr_controller_lifecycle=ok modes=startup-drain-probebw-probertt-probebw ' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'public_ops=disabled loss_timeout=pending cycle_seed=external' \
    "$BUILD/summary.txt" >/dev/null

"$RECOVERY_BINARY" | tee "$BUILD/recovery-summary.txt"
grep -F 'bbr_recovery=ok packet_conservation=first_round ' \
    "$BUILD/recovery-summary.txt" >/dev/null
grep -F 'restore=prior_cwnd sndbuf_expand=3x units=bytes' \
    "$BUILD/recovery-summary.txt" >/dev/null

printf '%s\n' \
    'p6h_bbr_controller=internal-ack-lifecycle-only' \
    'p6h_public_registry=disabled' \
    'p6i_bbr_recovery=packet-conservation-model-qualified' \
    'p6i_transport_recovery_observation=pending' \
    'p6i_timeout_semantics=pending' \
    | tee "$BUILD/reference.txt"
