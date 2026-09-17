#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${TCP_SHIFT_SEND_ENVELOPE_BUILD:-"$ROOT/.build-cc-envelope"}
BINARY=${TCP_SHIFT_SEND_ENVELOPE_BINARY:-"$BUILD/tcp-shift-p5c-paced"}
CASE=${TCP_SHIFT_SEND_ENVELOPE_CASE:-low-bdp}
MODE=${TCP_SHIFT_SEND_ENVELOPE_MODE:-linux}
RTT_MS=${TCP_SHIFT_SEND_ENVELOPE_RTT_MS:-10}
RATE_MBIT=${TCP_SHIFT_SEND_ENVELOPE_RATE_MBIT:-20}
PAYLOAD_BYTES=${TCP_SHIFT_SEND_ENVELOPE_PAYLOAD_BYTES:-8388608}
QUEUE_PKTS=${TCP_SHIFT_SEND_ENVELOPE_QUEUE_PKTS:-35}
RATE_CAP=${TCP_SHIFT_SEND_ENVELOPE_RATE_CAP_BYTES_PER_SEC:-0}
OUT=${TCP_SHIFT_SEND_ENVELOPE_OUT:-"$ROOT/.build/cc-send-envelope/$CASE/$MODE"}

TCPDUMP_PID=
BENCH_PID=
mkdir -p "$OUT"

[ "$(id -u)" -eq 0 ] || {
    echo "send-envelope benchmark requires root" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing paced tcp-shift binary: $BINARY" >&2
    exit 1
}
command -v tcpdump >/dev/null 2>&1 || {
    echo "tcpdump is required for send-envelope benchmark" >&2
    exit 1
}

case "$MODE" in
    linux|linux-cap) ;;
    *) echo "mode must be linux or linux-cap" >&2; exit 1 ;;
esac
case "$RTT_MS:$RATE_MBIT:$PAYLOAD_BYTES:$QUEUE_PKTS:$RATE_CAP" in
    *[!0-9:]*) echo "numeric send-envelope parameters must be nonnegative integers" >&2; exit 1 ;;
esac
[ "$RTT_MS" -gt 0 ] && [ "$RATE_MBIT" -gt 0 ] && \
[ "$PAYLOAD_BYTES" -gt 0 ] && [ "$QUEUE_PKTS" -gt 0 ] || {
    echo "RTT/rate/payload/queue must be positive" >&2
    exit 1
}
if [ "$MODE" = linux-cap ] && [ "$RATE_CAP" -eq 0 ]; then
    echo "linux-cap requires TCP_SHIFT_SEND_ENVELOPE_RATE_CAP_BYTES_PER_SEC" >&2
    exit 1
fi

cleanup()
{
    set +e
    if [ -n "${BENCH_PID:-}" ] && kill -0 "$BENCH_PID" 2>/dev/null; then
        kill -TERM "$BENCH_PID" >/dev/null 2>&1 || true
        wait "$BENCH_PID" >/dev/null 2>&1 || true
    fi
    if [ -n "${TCPDUMP_PID:-}" ] && kill -0 "$TCPDUMP_PID" 2>/dev/null; then
        kill -INT "$TCPDUMP_PID" >/dev/null 2>&1 || true
        wait "$TCPDUMP_PID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

: > "$OUT/tcpdump.txt"
: > "$OUT/tcpdump.stderr"
: > "$OUT/benchmark.stdout"
: > "$OUT/benchmark.stderr"

# Capture before the benchmark creates its TUN. Linux cooked capture records the
# interface name, so the analyzer can later select exactly tscb<benchmark-pid>
# while excluding loopback/backend traffic and the Linux reference namespace.
tcpdump --time-stamp-precision=nano -tt -n -l -i any \
    'tcp src port 18740' > "$OUT/tcpdump.txt" 2> "$OUT/tcpdump.stderr" &
TCPDUMP_PID=$!
sleep 0.15

if [ "$MODE" = linux-cap ]; then
    env \
        TCP_SHIFT_PACING_QUALIFICATION=linux-cap \
        TCP_SHIFT_PACING_QUALIFICATION_MAX_BYTES_PER_SEC="$RATE_CAP" \
        TCP_SHIFT_CUBIC_BENCH_BINARY="$BINARY" \
        TCP_SHIFT_CUBIC_BENCH_CASE="send-envelope-$CASE-$MODE" \
        TCP_SHIFT_CUBIC_BENCH_RTT_MS="$RTT_MS" \
        TCP_SHIFT_CUBIC_BENCH_RATE_MBIT="$RATE_MBIT" \
        TCP_SHIFT_CUBIC_BENCH_LOSS_PCT=0 \
        TCP_SHIFT_CUBIC_BENCH_PAYLOAD_BYTES="$PAYLOAD_BYTES" \
        TCP_SHIFT_CUBIC_BENCH_QUEUE_PKTS="$QUEUE_PKTS" \
        TCP_SHIFT_CUBIC_BENCH_OUT="$OUT/benchmark" \
        sh "$ROOT/scripts/cubic-linux-benchmark.sh" \
        > "$OUT/benchmark.stdout" 2> "$OUT/benchmark.stderr" &
else
    env \
        TCP_SHIFT_PACING_QUALIFICATION=linux \
        TCP_SHIFT_CUBIC_BENCH_BINARY="$BINARY" \
        TCP_SHIFT_CUBIC_BENCH_CASE="send-envelope-$CASE-$MODE" \
        TCP_SHIFT_CUBIC_BENCH_RTT_MS="$RTT_MS" \
        TCP_SHIFT_CUBIC_BENCH_RATE_MBIT="$RATE_MBIT" \
        TCP_SHIFT_CUBIC_BENCH_LOSS_PCT=0 \
        TCP_SHIFT_CUBIC_BENCH_PAYLOAD_BYTES="$PAYLOAD_BYTES" \
        TCP_SHIFT_CUBIC_BENCH_QUEUE_PKTS="$QUEUE_PKTS" \
        TCP_SHIFT_CUBIC_BENCH_OUT="$OUT/benchmark" \
        sh "$ROOT/scripts/cubic-linux-benchmark.sh" \
        > "$OUT/benchmark.stdout" 2> "$OUT/benchmark.stderr" &
fi
BENCH_PID=$!
TRACE_IFACE="tscb$BENCH_PID"

if ! wait "$BENCH_PID"; then
    BENCH_PID=
    cat "$OUT/benchmark.stderr" >&2 || true
    exit 1
fi
BENCH_PID=

kill -INT "$TCPDUMP_PID" >/dev/null 2>&1 || true
wait "$TCPDUMP_PID" >/dev/null 2>&1 || true
TCPDUMP_PID=

python3 "$ROOT/scripts/cc-send-envelope-analyze.py" \
    --trace "$OUT/tcpdump.txt" \
    --interface "$TRACE_IFACE" \
    --rtt-ms "$RTT_MS" \
    --rate-mbit "$RATE_MBIT" \
    --mode "$MODE" \
    --runtime-stderr "$OUT/benchmark/tcp-shift/runtime.stderr" \
    --benchmark-summary "$OUT/benchmark/summary.txt" \
    | tee "$OUT/envelope.txt"

printf 'case=%s\nmode=%s\nrtt_ms=%s\nrate_mbit=%s\npayload_bytes=%s\nqueue_pkts=%s\nrate_cap_bytes_per_sec=%s\ntrace_interface=%s\n' \
    "$CASE" "$MODE" "$RTT_MS" "$RATE_MBIT" "$PAYLOAD_BYTES" "$QUEUE_PKTS" \
    "$RATE_CAP" "$TRACE_IFACE" > "$OUT/envelope.env"
