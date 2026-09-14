#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
PAYLOAD_BYTES=${TCP_SHIFT_P5_PAYLOAD_BYTES:-262144}
MODE=${1:-normal}

fail()
{
    echo "P5 delivery-ledger qualification failed: $*" >&2
    exit 1
}

field()
{
    key=$1
    file=$2
    value=$(awk -v key="$key" '
        $1 == "tcp-shift-p2-delivery:" {
            for (i = 2; i <= NF; i++) {
                split($i, pair, "=")
                if (pair[1] == key) {
                    value = pair[2]
                }
            }
        }
        END {
            if (value ~ /^[0-9]+$/) {
                print value
            }
        }
    ' "$file")
    case "$value" in
        ''|*[!0-9]*) fail "missing/invalid ${key} in $file" ;;
    esac
    printf '%s\n' "$value"
}

check_delivery()
{
    runtime=$1
    expect_retransmit=$2

    grep -F 'tcp-shift-p2-delivery:' "$runtime" >/dev/null ||
        fail "runtime did not emit delivery diagnostics"

    delivered=$(field delivered_payload_bytes "$runtime")
    first_tx=$(field first_tx_events "$runtime")
    retransmit=$(field retransmit_events "$runtime")
    acked=$(field acked_segment_events "$runtime")
    alloc_fail=$(field metadata_alloc_failures "$runtime")
    misses=$(field metadata_misses "$runtime")
    abandoned=$(field metadata_abandoned_slots "$runtime")
    clock_errors=$(field clock_errors "$runtime")
    regressions=$(field timestamp_regressions "$runtime")
    last_tx=$(field last_tx_ns "$runtime")
    last_ack=$(field last_ack_ns "$runtime")
    bytes_per_slot=$(field metadata_bytes_per_slot "$runtime")
    live=$(field live_slots "$runtime")
    peak=$(field peak_slots_per_flow "$runtime")
    capacity=$(field peak_capacity_slots_per_flow "$runtime")

    [ "$delivered" -eq "$PAYLOAD_BYTES" ] ||
        fail "delivered payload $delivered != $PAYLOAD_BYTES"
    [ "$first_tx" -gt 0 ] || fail "no first-transmit metadata events"
    [ "$acked" -eq "$first_tx" ] ||
        fail "unique first-tx/acked segment mismatch: $first_tx/$acked"
    [ "$alloc_fail" -eq 0 ] || fail "metadata allocation failures=$alloc_fail"
    [ "$misses" -eq 0 ] || fail "metadata misses=$misses"
    [ "$abandoned" -eq 0 ] || fail "metadata abandoned slots=$abandoned"
    [ "$clock_errors" -eq 0 ] || fail "clock errors=$clock_errors"
    [ "$regressions" -eq 0 ] || fail "timestamp regressions=$regressions"
    [ "$last_tx" -gt 0 ] || fail "last tx timestamp missing"
    [ "$last_ack" -ge "$last_tx" ] ||
        fail "last ack timestamp $last_ack precedes last tx $last_tx"
    [ "$bytes_per_slot" -eq 32 ] ||
        fail "delivery slot size changed from 32 bytes to $bytes_per_slot"
    [ "$live" -eq 0 ] || fail "live metadata after natural teardown=$live"
    [ "$peak" -gt 0 ] || fail "no peak metadata residency observed"
    [ "$capacity" -ge "$peak" ] ||
        fail "capacity $capacity below peak live slots $peak"
    [ "$capacity" -le 90 ] ||
        fail "metadata capacity $capacity exceeds current TCP_SND_QUEUELEN=90"

    if [ "$expect_retransmit" -eq 0 ]; then
        [ "$retransmit" -eq 0 ] ||
            fail "unexpected retransmission metadata events=$retransmit"
    else
        [ "$retransmit" -gt 0 ] ||
            fail "fault workload did not reuse segment metadata on retransmit"
    fi

    printf 'mode=%s payload_bytes=%s first_tx_events=%s retransmit_events=%s acked_segment_events=%s delivered_payload_bytes=%s metadata_bytes_per_slot=%s peak_slots_per_flow=%s peak_capacity_slots_per_flow=%s live_slots=%s ledger=ok\n' \
        "$MODE" "$delivered" "$first_tx" "$retransmit" "$acked" \
        "$delivered" "$bytes_per_slot" "$peak" "$capacity" "$live"
}

case "$MODE" in
    normal)
        TCP_SHIFT_P2_PAYLOAD_BYTES="$PAYLOAD_BYTES" \
            sh "$ROOT/scripts/p2-bridge-smoke.sh"
        check_delivery "$BUILD/p2-bridge-ci/runtime.stderr" 0 \
            > "$BUILD/p5-delivery-normal-summary.txt"
        cat "$BUILD/p5-delivery-normal-summary.txt"
        ;;
    fast-loss|rto)
        TCP_SHIFT_P4_PAYLOAD_BYTES="$PAYLOAD_BYTES" \
            sh "$ROOT/scripts/p4-cc-recovery-smoke.sh" "$MODE"
        check_delivery "$BUILD/p4-${MODE}-ci/runtime.stderr" 1 \
            > "$BUILD/p5-delivery-${MODE}-summary.txt"
        cat "$BUILD/p5-delivery-${MODE}-summary.txt"
        ;;
    *)
        fail "usage: $0 {normal|fast-loss|rto}"
        ;;
esac
