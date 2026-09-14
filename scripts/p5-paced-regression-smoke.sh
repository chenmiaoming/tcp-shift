#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
BINARY=${TCP_SHIFT_P5C_BINARY:-"$BUILD/tcp-shift-p5c-paced"}
MODE=${1:-}

[ -x "$BINARY" ] || {
    echo "missing P5c paced binary: $BINARY" >&2
    exit 1
}

case "$MODE" in
    multi)
        TCP_SHIFT_P2_BINARY="$BINARY" \
        TCP_SHIFT_P2_CONC_FLOW_COUNT=${TCP_SHIFT_P5C_FLOW_COUNT:-4} \
        TCP_SHIFT_P2_CONC_PAYLOAD_BYTES=${TCP_SHIFT_P5C_FLOW_PAYLOAD_BYTES:-65536} \
        TCP_SHIFT_P2_CONC_TUN_NAME=${TCP_SHIFT_P5C_MULTI_TUN_NAME:-tsp5multi0} \
        TCP_SHIFT_P2_CONC_PUBLIC_PORT=${TCP_SHIFT_P5C_MULTI_PUBLIC_PORT:-18161} \
        TCP_SHIFT_P2_CONC_BACKEND_PORT=${TCP_SHIFT_P5C_MULTI_BACKEND_PORT:-19161} \
            sh "$ROOT/scripts/p2-concurrency-smoke.sh"
        RUNTIME="$BUILD/p2-concurrency-ci/runtime.stderr"
        OUT="$BUILD/p5c-paced-multi-summary.txt"
        ;;
    fast-loss|rto)
        [ "$(id -u)" -eq 0 ] || {
            echo "P5c $MODE recovery qualification must run as root" >&2
            exit 1
        }
        TCP_SHIFT_P4_BINARY="$BINARY" \
        TCP_SHIFT_P4_PAYLOAD_BYTES=${TCP_SHIFT_P5C_RECOVERY_PAYLOAD_BYTES:-131072} \
            sh "$ROOT/scripts/p4-cc-recovery-smoke.sh" "$MODE"
        RUNTIME="$BUILD/p4-$MODE-ci/runtime.stderr"
        OUT="$BUILD/p5c-paced-$MODE-summary.txt"
        ;;
    *)
        echo "usage: $0 <multi|fast-loss|rto>" >&2
        exit 2
        ;;
esac

python3 - "$RUNTIME" "$OUT" "$MODE" <<'PY'
import json
import sys

path, out, mode = sys.argv[1:]
lines = open(path, encoding="utf-8").read().splitlines()


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

main = parse_line("tcp-shift-p2:")
pacing = parse_line("tcp-shift-p2-pacing:")

for key in ("scheduler_errors", "stale_releases", "loop_callback_errors"):
    if pacing.get(key) != 0:
        raise SystemExit(f"{mode}: {key} must be zero, got {pacing.get(key)}")
if main.get("cc_controller_errors") != 0:
    raise SystemExit(
        f"{mode}: controller errors must be zero, got {main.get('cc_controller_errors')}"
    )
if pacing.get("last_rate_bytes_per_sec") != 65536:
    raise SystemExit(
        f"{mode}: expected 65536 B/s policy, got {pacing.get('last_rate_bytes_per_sec')}"
    )
if pacing.get("timerfd_creates") != 1:
    raise SystemExit(
        f"{mode}: expected one timerfd, got {pacing.get('timerfd_creates')}"
    )
if pacing.get("heap_current") != 0:
    raise SystemExit(f"{mode}: heap did not drain: {pacing.get('heap_current')}")
for key in ("deferrals", "resume_events", "scheduled_events", "released_events"):
    if pacing.get(key, 0) <= 0:
        raise SystemExit(f"{mode}: expected positive {key}, got {pacing.get(key)}")
if pacing.get("released_events") != pacing.get("resume_events"):
    raise SystemExit(
        f"{mode}: release/resume mismatch "
        f"{pacing.get('released_events')} != {pacing.get('resume_events')}"
    )
if pacing.get("max_lateness_ns", 0) > 250_000_000:
    raise SystemExit(
        f"{mode}: max pacing lateness exceeded 250 ms: {pacing.get('max_lateness_ns')}"
    )

if mode == "multi":
    if main.get("bridge_peak_active_flows", 0) < 2:
        raise SystemExit(
            f"multi: expected concurrent bridge flows, got {main.get('bridge_peak_active_flows')}"
        )
    if pacing.get("heap_peak", 0) < 2:
        raise SystemExit(
            f"multi: expected overlapping pacing deadlines, heap_peak={pacing.get('heap_peak')}"
        )
elif mode == "fast-loss":
    if main.get("cc_loss_events", 0) < 1:
        raise SystemExit("fast-loss: no controller loss event")
    if main.get("cc_timeout_events") != 0:
        raise SystemExit(
            f"fast-loss: unexpected timeout events={main.get('cc_timeout_events')}"
        )
elif mode == "rto":
    if main.get("cc_timeout_events", 0) < 1:
        raise SystemExit("rto: no controller timeout event")

summary = {
    "mode": mode,
    "pacing_rate_bytes_per_sec": pacing["last_rate_bytes_per_sec"],
    "deferrals": pacing["deferrals"],
    "resume_events": pacing["resume_events"],
    "scheduled_events": pacing["scheduled_events"],
    "released_events": pacing["released_events"],
    "cancelled_events": pacing.get("cancelled_events", 0),
    "heap_peak": pacing.get("heap_peak", 0),
    "heap_current": pacing["heap_current"],
    "max_lateness_ns": pacing.get("max_lateness_ns", 0),
    "loss_events": main.get("cc_loss_events", 0),
    "timeout_events": main.get("cc_timeout_events", 0),
    "controller_errors": main["cc_controller_errors"],
    "scheduler_errors": pacing["scheduler_errors"],
    "stale_releases": pacing["stale_releases"],
}
with open(out, "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(summary, sort_keys=True))
PY

cat "$OUT"
echo "P5c paced $MODE regression qualification passed"
