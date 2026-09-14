#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p4-adapter-p2-evidence"

mkdir -p "$OUT"
: > "$OUT/workloads.txt"

fail()
{
    echo "P4 adapter P2 evidence failed: $*" >&2
    exit 1
}

value_from_line()
{
    key=$1
    line=$2
    value=$(printf '%s\n' "$line" | sed -n "s/.* ${key}=\([0-9][0-9]*\).*/\1/p")
    [ -n "$value" ] || value=$(printf '%s\n' "$line" | sed -n "s/^${key}=\([0-9][0-9]*\).*/\1/p")
    printf '%s' "$value"
}

count=0
for stderr in \
    "$BUILD/p2-bridge-ci/runtime.stderr" \
    "$BUILD/p2-backpressure-ci/runtime.stderr" \
    "$BUILD/p2-halfclose-ci/runtime.stderr" \
    "$BUILD/p2-backend-recovery-ci/runtime.stderr" \
    "$BUILD/p2-reset-recovery-ci/runtime.stderr" \
    "$BUILD/p2-concurrency-ci/runtime.stderr" \
    "$BUILD/p2-active-shutdown-ci/runtime.stderr" \
    "$BUILD/p2-reuse-rss-ci/runtime.stderr" \
    "$BUILD/p2-ipv6-bridge-ci/runtime.stderr"
do
    [ -f "$stderr" ] || fail "missing runtime diagnostics: $stderr"
    line=$(grep -m1 ' bridge_accepts=' "$stderr" || true)
    [ -n "$line" ] || fail "missing bridge metrics in $stderr"

    accepts=$(value_from_line bridge_accepts "$line")
    bindings=$(value_from_line cc_bindings "$line")
    bind_failures=$(value_from_line cc_bind_failures "$line")
    ack_events=$(value_from_line cc_ack_events "$line")
    controller_errors=$(value_from_line cc_controller_errors "$line")

    [ -n "$accepts" ] || fail "missing bridge_accepts in $stderr"
    [ -n "$bindings" ] || fail "missing cc_bindings in $stderr"
    [ -n "$bind_failures" ] || fail "missing cc_bind_failures in $stderr"
    [ -n "$ack_events" ] || fail "missing cc_ack_events in $stderr"
    [ -n "$controller_errors" ] || fail "missing cc_controller_errors in $stderr"

    [ "$accepts" -gt 0 ] || fail "workload accepted no public flow: $stderr"
    [ "$bindings" -eq "$accepts" ] || \
        fail "controller bindings $bindings != bridge accepts $accepts in $stderr"
    [ "$bind_failures" -eq 0 ] || \
        fail "controller bind failures=$bind_failures in $stderr"
    [ "$ack_events" -gt 0 ] || \
        fail "controller observed no ACK policy event in $stderr"
    [ "$controller_errors" -eq 0 ] || \
        fail "controller errors=$controller_errors in $stderr"

    printf '%s accepts=%s bindings=%s ack_events=%s bind_failures=0 controller_errors=0\n' \
        "$(basename "$(dirname "$stderr")")" "$accepts" "$bindings" "$ack_events" \
        | tee -a "$OUT/workloads.txt"
    count=$((count + 1))
done

[ "$count" -eq 9 ] || fail "expected 9 P2 workloads, saw $count"
printf 'p4_adapter_p2_workloads=%u controller_ownership=ok\n' "$count" \
    | tee "$OUT/summary.txt"
echo "P4 integrated controller ownership across P2 workloads passed"
