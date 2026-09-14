#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
PAYLOAD_BYTES=${TCP_SHIFT_P5_PAYLOAD_BYTES:-262144}
METADATA_BYTES_PER_SLOT=${TCP_SHIFT_P5_METADATA_BYTES_PER_SLOT:-56}
MODE=${1:-normal}

fail()
{
    echo "P5 delivery/rate qualification failed: $*" >&2
    exit 1
}

field_from_line()
{
    prefix=$1
    key=$2
    file=$3
    value=$(awk -v prefix="$prefix" -v key="$key" '
        $1 == prefix {
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

delivery_field()
{
    field_from_line 'tcp-shift-p2-delivery:' "$1" "$2"
}

rate_field()
{
    field_from_line 'tcp-shift-p2-rate:' "$1" "$2"
}

check_delivery_and_rate()
{
    runtime=$1
    expect_retransmit=$2

    grep -F 'tcp-shift-p2-delivery:' "$runtime" >/dev/null ||
        fail "runtime did not emit delivery diagnostics"
    grep -F 'tcp-shift-p2-rate:' "$runtime" >/dev/null ||
        fail "runtime did not emit rate diagnostics"

    delivered=$(delivery_field delivered_payload_bytes "$runtime")
    first_tx=$(delivery_field first_tx_events "$runtime")
    retransmit=$(delivery_field retransmit_events "$runtime")
    acked=$(delivery_field acked_segment_events "$runtime")
    alloc_fail=$(delivery_field metadata_alloc_failures "$runtime")
    misses=$(delivery_field metadata_misses "$runtime")
    abandoned=$(delivery_field metadata_abandoned_slots "$runtime")
    clock_errors=$(delivery_field clock_errors "$runtime")
    regressions=$(delivery_field timestamp_regressions "$runtime")
    last_tx=$(delivery_field last_tx_ns "$runtime")
    last_ack=$(delivery_field last_ack_ns "$runtime")
    bytes_per_slot=$(delivery_field metadata_bytes_per_slot "$runtime")
    live=$(delivery_field live_slots "$runtime")
    peak=$(delivery_field peak_slots_per_flow "$runtime")
    capacity=$(delivery_field peak_capacity_slots_per_flow "$runtime")

    samples=$(rate_field samples "$runtime")
    valid=$(rate_field valid_samples "$runtime")
    invalid=$(rate_field invalid_samples "$runtime")
    retrans_samples=$(rate_field retransmitted_samples "$runtime")
    max_rate=$(rate_field max_rate_bytes_per_sec "$runtime")
    interval=$(rate_field last_interval_ns "$runtime")
    ack_interval=$(rate_field last_ack_interval_ns "$runtime")

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
    [ "$bytes_per_slot" -eq "$METADATA_BYTES_PER_SLOT" ] ||
        fail "delivery slot size $bytes_per_slot != expected P5 layout $METADATA_BYTES_PER_SLOT"
    [ "$live" -eq 0 ] || fail "live metadata after natural teardown=$live"
    [ "$peak" -gt 0 ] || fail "no peak metadata residency observed"
    [ "$capacity" -ge "$peak" ] ||
        fail "capacity $capacity below peak live slots $peak"
    [ "$capacity" -le 90 ] ||
        fail "metadata capacity $capacity exceeds current TCP_SND_QUEUELEN=90"

    [ "$samples" -gt 0 ] || fail "no rate samples"
    [ "$valid" -gt 0 ] || fail "no valid rate samples"
    [ "$invalid" -eq 0 ] || fail "invalid rate samples=$invalid"
    [ "$max_rate" -gt 0 ] || fail "maximum delivery rate is zero"
    [ "$interval" -gt 0 ] || fail "last rate interval is zero"
    [ "$ack_interval" -gt 0 ] || fail "last ACK interval is zero"

    if [ "$expect_retransmit" -eq 0 ]; then
        [ "$retransmit" -eq 0 ] ||
            fail "unexpected retransmission metadata events=$retransmit"
    else
        [ "$retransmit" -gt 0 ] ||
            fail "fault workload did not reuse segment metadata on retransmit"
    fi
    if [ "$MODE" = rto ]; then
        [ "$retrans_samples" -gt 0 ] ||
            fail "RTO workload did not produce a retransmitted rate candidate"
    fi

    printf 'mode=%s payload_bytes=%s first_tx_events=%s retransmit_events=%s acked_segment_events=%s delivered_payload_bytes=%s metadata_bytes_per_slot=%s peak_slots_per_flow=%s peak_capacity_slots_per_flow=%s live_slots=%s rate_samples=%s valid_rate_samples=%s invalid_rate_samples=%s retransmitted_rate_samples=%s max_rate_bytes_per_sec=%s last_interval_ns=%s last_ack_interval_ns=%s sampler=ok\n' \
        "$MODE" "$delivered" "$first_tx" "$retransmit" "$acked" \
        "$delivered" "$bytes_per_slot" "$peak" "$capacity" "$live" \
        "$samples" "$valid" "$invalid" "$retrans_samples" "$max_rate" \
        "$interval" "$ack_interval"
}

case "$MODE" in
    normal)
        TCP_SHIFT_P2_PAYLOAD_BYTES="$PAYLOAD_BYTES" \
            sh "$ROOT/scripts/p2-bridge-smoke.sh"
        check_delivery_and_rate "$BUILD/p2-bridge-ci/runtime.stderr" 0 \
            > "$BUILD/p5-delivery-normal-summary.txt"
        cat "$BUILD/p5-delivery-normal-summary.txt"
        ;;
    fast-loss|rto)
        TCP_SHIFT_P4_PAYLOAD_BYTES="$PAYLOAD_BYTES" \
            sh "$ROOT/scripts/p4-cc-recovery-smoke.sh" "$MODE"
        check_delivery_and_rate "$BUILD/p4-${MODE}-ci/runtime.stderr" 1 \
            > "$BUILD/p5-delivery-${MODE}-summary.txt"
        cat "$BUILD/p5-delivery-${MODE}-summary.txt"
        ;;
    *)
        fail "usage: $0 {normal|fast-loss|rto}"
        ;;
esac
