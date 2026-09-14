#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
BINARY=${TCP_SHIFT_P5C_BINARY:-"$BUILD/tcp-shift-p5c-paced"}
PAYLOAD_BYTES=${TCP_SHIFT_P5C_HIGH_BDP_PAYLOAD_BYTES:-262144}
DELAY_MS=${TCP_SHIFT_P5C_HIGH_BDP_DELAY_MS:-750}
SOCKET_TIMEOUT_SECONDS=${TCP_SHIFT_P5C_HIGH_BDP_TIMEOUT_SECONDS:-60}
POLICY_RATE_BYTES_PER_SEC=65536
QUALIFIED_WINDOW_BYTES=32768
P2_OUT="$BUILD/p2-bridge-ci"
SUMMARY="$BUILD/p5c-paced-high-bdp-summary.txt"

[ "$(id -u)" -eq 0 ] || {
    echo "P5c high-BDP qualification must run as root for netem" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing P5c paced binary: $BINARY" >&2
    exit 1
}
case "$DELAY_MS" in
    ''|*[!0-9]*)
        echo "high-BDP delay must be a positive integer" >&2
        exit 1
        ;;
esac
[ "$DELAY_MS" -gt 0 ] || {
    echo "high-BDP delay must be positive" >&2
    exit 1
}

nominal_bdp_bytes=$((POLICY_RATE_BYTES_PER_SEC * DELAY_MS / 1000))
[ "$nominal_bdp_bytes" -gt "$QUALIFIED_WINDOW_BYTES" ] || {
    echo "high-BDP fixture is too small: nominal_bdp=$nominal_bdp_bytes window=$QUALIFIED_WINDOW_BYTES" >&2
    exit 1
}

TCP_SHIFT_P2_BINARY="$BINARY" \
TCP_SHIFT_P2_TUN_NAME=${TCP_SHIFT_P5C_HIGH_BDP_TUN_NAME:-tsp5bdp0} \
TCP_SHIFT_P2_PUBLIC_PORT=${TCP_SHIFT_P5C_HIGH_BDP_PUBLIC_PORT:-18162} \
TCP_SHIFT_P2_BACKEND_PORT=${TCP_SHIFT_P5C_HIGH_BDP_BACKEND_PORT:-19162} \
TCP_SHIFT_P2_PAYLOAD_BYTES="$PAYLOAD_BYTES" \
TCP_SHIFT_P2_NETEM_DELAY_MS="$DELAY_MS" \
TCP_SHIFT_P2_SOCKET_TIMEOUT_SECONDS="$SOCKET_TIMEOUT_SECONDS" \
TCP_SHIFT_P2_CAPTURE_RUNTIME_CPU=1 \
    sh "$ROOT/scripts/p2-bridge-smoke.sh"

[ -s "$P2_OUT/runtime-cpu.txt" ] || {
    echo "missing high-BDP runtime CPU sample" >&2
    exit 1
}
[ -s "$P2_OUT/netem-after.txt" ] || {
    echo "missing high-BDP netem diagnostics" >&2
    exit 1
}

python3 - "$P2_OUT/runtime.stderr" "$P2_OUT/runtime-cpu.txt" \
    "$SUMMARY" "$PAYLOAD_BYTES" "$DELAY_MS" "$nominal_bdp_bytes" \
    "$QUALIFIED_WINDOW_BYTES" <<'PY'
import json
import sys

runtime_path, cpu_path, out_path, payload_text, delay_text, bdp_text, window_text = sys.argv[1:]
payload = int(payload_text)
delay_ms = int(delay_text)
nominal_bdp = int(bdp_text)
window_bytes = int(window_text)
lines = open(runtime_path, encoding="utf-8").read().splitlines()


def parse_line(prefix):
    for line in lines:
        if line.startswith(prefix):
            result = {}
            for token in line[len(prefix):].strip().split():
                if "=" not in token:
                    continue
                key, value = token.split("=", 1)
                try:
                    result[key] = int(value)
                except ValueError:
                    pass
            return result
    raise SystemExit(f"missing runtime line: {prefix}")


def parse_tokens(path):
    result = {}
    for token in open(path, encoding="utf-8").read().split():
        key, value = token.split("=", 1)
        result[key] = int(value)
    return result

main = parse_line("tcp-shift-p2:")
pacing = parse_line("tcp-shift-p2-pacing:")
cpu = parse_tokens(cpu_path)

if nominal_bdp <= window_bytes:
    raise SystemExit(
        f"fixture is not high-BDP relative to qualified window: {nominal_bdp} <= {window_bytes}"
    )
if main.get("bridge_backend_to_public_bytes") != payload:
    raise SystemExit(
        f"high-BDP payload mismatch: expected={payload} got={main.get('bridge_backend_to_public_bytes')}"
    )
if main.get("bridge_backend_failures") != 0 or main.get("bridge_public_errors") != 0:
    raise SystemExit("bridge errors observed during high-BDP transfer")
if main.get("cc_controller_errors") != 0:
    raise SystemExit(f"controller errors={main.get('cc_controller_errors')}")
if main.get("cc_loss_events") != 0 or main.get("cc_timeout_events") != 0:
    raise SystemExit(
        "unexpected recovery event in delay-only qualification: "
        f"loss={main.get('cc_loss_events')} timeout={main.get('cc_timeout_events')}"
    )

for key in ("scheduler_errors", "stale_releases", "loop_callback_errors"):
    if pacing.get(key) != 0:
        raise SystemExit(f"{key} must be zero, got {pacing.get(key)}")
if pacing.get("last_rate_bytes_per_sec") != 65536:
    raise SystemExit(
        f"expected 65536 B/s pacing policy, got {pacing.get('last_rate_bytes_per_sec')}"
    )
if pacing.get("pacing_tx_bytes", pacing.get("tx_bytes")) not in (None, payload):
    raise SystemExit(
        f"paced tx bytes mismatch: expected={payload} got={pacing.get('pacing_tx_bytes', pacing.get('tx_bytes'))}"
    )
if pacing.get("tx_bytes") != payload:
    raise SystemExit(f"pacing tx bytes mismatch: expected={payload} got={pacing.get('tx_bytes')}")
if pacing.get("timerfd_creates") != 1:
    raise SystemExit(f"expected one timerfd, got {pacing.get('timerfd_creates')}")
if pacing.get("heap_current") != 0:
    raise SystemExit(f"pacer heap did not drain: {pacing.get('heap_current')}")
for key in (
    "deferrals",
    "resume_events",
    "loop_pacing_wakeups",
    "timer_expirations",
    "scheduled_events",
    "released_events",
    "released_bytes",
):
    if pacing.get(key, 0) <= 0:
        raise SystemExit(f"expected positive {key}, got {pacing.get(key)}")
if pacing.get("released_events") != pacing.get("resume_events"):
    raise SystemExit(
        f"release/resume mismatch: {pacing.get('released_events')} != {pacing.get('resume_events')}"
    )
if pacing.get("max_lateness_ns", 0) > 250_000_000:
    raise SystemExit(
        f"high-BDP pacing lateness exceeded 250 ms: {pacing.get('max_lateness_ns')}"
    )

clk_tck = cpu["clk_tck"]
elapsed_ticks = cpu["elapsed_ticks"]
elapsed_ns = cpu["elapsed_ns"]
if clk_tck <= 0 or elapsed_ns <= 0:
    raise SystemExit(f"invalid CPU sample: {cpu}")
wall_seconds = elapsed_ns / 1_000_000_000
cpu_seconds = elapsed_ticks / clk_tck
cpu_percent = 100.0 * cpu_seconds / wall_seconds
if wall_seconds < 3.0:
    raise SystemExit(f"high-BDP sample too short: {wall_seconds:.3f}s")
if cpu_percent >= 50.0:
    raise SystemExit(
        f"runtime CPU indicates spin under high-BDP load: {cpu_percent:.2f}%"
    )

summary = {
    "payload_bytes": payload,
    "ack_path_delay_ms": delay_ms,
    "nominal_bdp_bytes": nominal_bdp,
    "qualified_window_bytes": window_bytes,
    "pacing_rate_bytes_per_sec": pacing["last_rate_bytes_per_sec"],
    "deferrals": pacing["deferrals"],
    "resume_events": pacing["resume_events"],
    "scheduled_events": pacing["scheduled_events"],
    "released_events": pacing["released_events"],
    "released_bytes": pacing["released_bytes"],
    "timer_expirations": pacing["timer_expirations"],
    "timerfd_creates": pacing["timerfd_creates"],
    "heap_peak": pacing.get("heap_peak", 0),
    "heap_current": pacing["heap_current"],
    "max_lateness_ns": pacing.get("max_lateness_ns", 0),
    "runtime_elapsed_ticks": elapsed_ticks,
    "runtime_wall_seconds": round(wall_seconds, 6),
    "runtime_cpu_seconds": round(cpu_seconds, 6),
    "runtime_cpu_percent": round(cpu_percent, 3),
    "loss_events": main.get("cc_loss_events", 0),
    "timeout_events": main.get("cc_timeout_events", 0),
    "controller_errors": main["cc_controller_errors"],
    "scheduler_errors": pacing["scheduler_errors"],
    "stale_releases": pacing["stale_releases"],
}
with open(out_path, "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(summary, sort_keys=True))
PY

cat "$P2_OUT/runtime-cpu.txt"
cat "$P2_OUT/netem-after.txt"
cat "$SUMMARY"
echo "P5c sustained high-BDP paced qualification passed"
