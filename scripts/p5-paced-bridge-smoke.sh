#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
BINARY=${TCP_SHIFT_P5C_BINARY:-"$BUILD/tcp-shift-p5c-paced"}
PAYLOAD_BYTES=${TCP_SHIFT_P5C_PAYLOAD_BYTES:-65536}
P2_OUT="$BUILD/p2-bridge-ci"
SUMMARY="$BUILD/p5c-paced-summary.txt"

[ -x "$BINARY" ] || {
    echo "missing P5c paced binary: $BINARY" >&2
    exit 1
}

TCP_SHIFT_P2_BINARY="$BINARY" \
TCP_SHIFT_P2_TUN_NAME=${TCP_SHIFT_P5C_TUN_NAME:-tsp5pace0} \
TCP_SHIFT_P2_PUBLIC_PORT=${TCP_SHIFT_P5C_PUBLIC_PORT:-18160} \
TCP_SHIFT_P2_BACKEND_PORT=${TCP_SHIFT_P5C_BACKEND_PORT:-19160} \
TCP_SHIFT_P2_PAYLOAD_BYTES="$PAYLOAD_BYTES" \
    sh "$ROOT/scripts/p2-bridge-smoke.sh"

python3 - "$P2_OUT/runtime.stderr" "$SUMMARY" "$PAYLOAD_BYTES" <<'PY'
import json
import sys

path, out, payload_text = sys.argv[1:]
payload = int(payload_text)
lines = open(path, encoding="utf-8").read().splitlines()


def parse_line(prefix):
    for line in lines:
        if line.startswith(prefix):
            result = {}
            for token in line[len(prefix):].strip().split():
                if "=" not in token:
                    continue
                key, value = token.split("=", 1)
                result[key] = int(value)
            return result
    raise SystemExit(f"missing runtime line: {prefix}")

main = parse_line("tcp-shift-p2:")
pacing = parse_line("tcp-shift-p2-pacing:")

required_zero = [
    "scheduler_errors",
    "stale_releases",
    "loop_callback_errors",
]
for key in required_zero:
    if pacing.get(key) != 0:
        raise SystemExit(f"{key} must be zero, got {pacing.get(key)}")

if main.get("cc_controller_errors") != 0:
    raise SystemExit(
        f"controller errors must be zero, got {main.get('cc_controller_errors')}"
    )
if main.get("bridge_backend_failures") != 0 or main.get("bridge_public_errors") != 0:
    raise SystemExit("bridge errors observed during paced transfer")
if main.get("bridge_backend_to_public_bytes") != payload:
    raise SystemExit(
        "paced payload mismatch: "
        f"expected={payload} got={main.get('bridge_backend_to_public_bytes')}"
    )

rate = pacing.get("last_rate_bytes_per_sec")
if rate != 65536:
    raise SystemExit(f"unexpected qualification pacing rate: {rate}")
for key in (
    "deferrals",
    "resume_events",
    "tx_events",
    "loop_pacing_wakeups",
    "loop_release_callbacks",
    "timer_expirations",
    "scheduled_events",
    "released_events",
):
    if pacing.get(key, 0) <= 0:
        raise SystemExit(f"expected positive {key}, got {pacing.get(key)}")

if pacing.get("timerfd_creates") != 1:
    raise SystemExit(
        f"expected exactly one timerfd, got {pacing.get('timerfd_creates')}"
    )
if pacing.get("heap_current") != 0:
    raise SystemExit(f"pacer heap did not drain: {pacing.get('heap_current')}")
if pacing.get("heap_peak", 0) <= 0:
    raise SystemExit("pacer heap was never populated")
if pacing.get("released_events") != pacing.get("resume_events"):
    raise SystemExit(
        "released/resume mismatch: "
        f"released={pacing.get('released_events')} resume={pacing.get('resume_events')}"
    )
if pacing.get("loop_release_callbacks") != pacing.get("resume_events"):
    raise SystemExit(
        "callback/resume mismatch: "
        f"callbacks={pacing.get('loop_release_callbacks')} "
        f"resume={pacing.get('resume_events')}"
    )
if pacing.get("max_lateness_ns", 0) > 250_000_000:
    raise SystemExit(
        f"pacing lateness exceeded 250 ms: {pacing.get('max_lateness_ns')} ns"
    )

summary = {
    "payload_bytes": payload,
    "pacing_rate_bytes_per_sec": rate,
    "deferrals": pacing["deferrals"],
    "resume_events": pacing["resume_events"],
    "scheduled_events": pacing["scheduled_events"],
    "released_events": pacing["released_events"],
    "timer_expirations": pacing["timer_expirations"],
    "timerfd_creates": pacing["timerfd_creates"],
    "heap_peak": pacing["heap_peak"],
    "heap_current": pacing["heap_current"],
    "max_lateness_ns": pacing["max_lateness_ns"],
    "scheduler_errors": pacing["scheduler_errors"],
    "stale_releases": pacing["stale_releases"],
    "controller_errors": main["cc_controller_errors"],
}
with open(out, "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(summary, sort_keys=True))
PY

cat "$SUMMARY"
echo "P5c integrated fixed-rate paced bridge smoke passed"
