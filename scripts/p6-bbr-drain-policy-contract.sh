#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p6-bbr-drain-policy-contract"
BINARY="$BUILD/tcp-shift-p6-bbr-drain-policy-contract"

rm -rf "$BUILD"
mkdir -p "$BUILD"

${CC:-cc} \
    -std=c11 \
    -Wall -Wextra -Wpedantic -Werror \
    -ffreestanding -fno-builtin \
    -I"$ROOT/src" \
    "$ROOT/src/cc/bbr.c" \
    "$ROOT/src/cc/bbr_drain.c" \
    "$ROOT/src/tests/bbr_drain_policy_test.c" \
    -o "$BINARY"

"$BINARY" | tee "$BUILD/summary.txt"
grep -F 'bbr_drain_policy=ok pacing_gain=88/256 pacing_margin=99/100 ' \
    "$BUILD/summary.txt" >/dev/null
grep -F 'cwnd_gain=739/256 drain_target=1bdp transition=startup-drain-probebw' \
    "$BUILD/summary.txt" >/dev/null

undefined=$(nm -u "$BINARY" | sed '/^[[:space:]]*$/d' || true)
[ -z "$undefined" ] || {
    printf '%s\n' "$undefined" >&2
    echo 'BBR Drain contract has external symbols' >&2
    exit 1
}

printf '%s\n' \
    'p6f_bbr_core_reference=linux-mainline-tcp_bbr' \
    'p6f_inflight_semantics=transport-prior-inflight-vs-base-bdp' \
    | tee "$BUILD/reference.txt"
