#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
BINARY=${TCP_SHIFT_P6_BBR_LONG_BINARY:-"$BUILD/tcp-shift-p6-bbr"}
RTT_MS=${TCP_SHIFT_P6_BBR_LONG_RTT_MS:-40}
RATE_MBIT=${TCP_SHIFT_P6_BBR_LONG_RATE_MBIT:-10}
PAYLOAD_BYTES=${TCP_SHIFT_P6_BBR_LONG_PAYLOAD_BYTES:-4194304}
OUT=${TCP_SHIFT_P6_BBR_LONG_OUT:-"$BUILD/p6-bbr-long-flow"}

TUN_NAME=${TCP_SHIFT_P6_BBR_LONG_TUN_NAME:-"tsp6lf$$"}
IFB_NAME=${TCP_SHIFT_P6_BBR_LONG_IFB_NAME:-"p6ifb$$"}
LWIP_IP=${TCP_SHIFT_P6_BBR_LONG_LWIP_IP:-10.246.0.2}
HOST_IP=${TCP_SHIFT_P6_BBR_LONG_HOST_IP:-10.246.0.1}
NETMASK=${TCP_SHIFT_P6_BBR_LONG_NETMASK:-255.255.255.252}
PUBLIC_PORT=${TCP_SHIFT_P6_BBR_LONG_PUBLIC_PORT:-18162}
BACKEND_PORT=${TCP_SHIFT_P6_BBR_LONG_BACKEND_PORT:-19162}

RUNTIME_PID=
BACKEND_PID=

mkdir -p "$OUT"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"

[ "$(id -u)" -eq 0 ] || {
    echo "P6 BBR long-flow harness must run as root for TUN, IFB and netem" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing internal BBR qualification binary: $BINARY" >&2
    exit 1
}
command -v ip >/dev/null 2>&1 || { echo "ip is required" >&2; exit 1; }
command -v tc >/dev/null 2>&1 || { echo "tc is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 1; }

case "$RTT_MS" in ''|*[!0-9]*) echo "RTT_MS must be an integer" >&2; exit 1;; esac
case "$RATE_MBIT" in ''|*[!0-9]*) echo "RATE_MBIT must be an integer" >&2; exit 1;; esac
case "$PAYLOAD_BYTES" in ''|*[!0-9]*) echo "PAYLOAD_BYTES must be an integer" >&2; exit 1;; esac
[ "$RTT_MS" -gt 0 ] && [ $((RTT_MS % 2)) -eq 0 ] || {
    echo "RTT_MS must be a positive even integer" >&2
    exit 1
}
[ "$RATE_MBIT" -gt 0 ] || { echo "RATE_MBIT must be positive" >&2; exit 1; }
[ "$PAYLOAD_BYTES" -gt 0 ] || { echo "PAYLOAD_BYTES must be positive" >&2; exit 1; }

HALF_RTT_MS=$((RTT_MS / 2))
BDP_BYTES=$((RATE_MBIT * RTT_MS * 125))
BDP_PKTS=$(((BDP_BYTES + 1459) / 1460))
# This is the clean long-flow gate, not the loss gate. netem owns the entire
# delayed/shaped path queue on IFB, so a one-BDP limit can turn BBR STARTUP's
# deliberate high inflight gain into artificial drop-tail loss. Keep eight
# BDPs of queue headroom here and require zero qdisc drops below; fast-loss and
# RTO remain qualified separately with explicit fault injection.
QUEUE_PKTS=${TCP_SHIFT_P6_BBR_LONG_QUEUE_PKTS:-$((BDP_PKTS * 8))}
[ "$QUEUE_PKTS" -ge 64 ] || QUEUE_PKTS=64

stop_pid()
{
    pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" >/dev/null 2>&1 || true
        wait "$pid" >/dev/null 2>&1 || true
    fi
}

cleanup()
{
    set +e
    stop_pid "${RUNTIME_PID:-}"
    stop_pid "${BACKEND_PID:-}"
    tc qdisc del dev "$TUN_NAME" root >/dev/null 2>&1 || true
    tc qdisc del dev "$TUN_NAME" ingress >/dev/null 2>&1 || true
    tc qdisc del dev "$IFB_NAME" root >/dev/null 2>&1 || true
    ip link del "$IFB_NAME" >/dev/null 2>&1 || true
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

python3 - "$BACKEND_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
length = int(sys.argv[2])
payload = bytes(((index * 73 + 19) & 0xFF) for index in range(length))
digest = hashlib.sha256(payload).hexdigest()

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready port={port}", flush=True)
conn, _ = server.accept()
conn.settimeout(90.0)
conn.sendall(payload)
conn.shutdown(socket.SHUT_WR)
while conn.recv(4096):
    pass
conn.close()
server.close()
print(f"backend-bytes={length} sha256={digest}", flush=True)
PY
BACKEND_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$BACKEND_PID" 2>/dev/null || {
        cat "$OUT/backend.stderr" >&2 || true
        echo "P6 BBR long-flow backend exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null || {
    echo "timed out waiting for P6 BBR long-flow backend" >&2
    exit 1
}

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" bbr-internal \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
RUNTIME_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p2: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null 2>&1 &&
       grep -F 'cc=bbr-internal' "$OUT/runtime.stdout" >/dev/null 2>&1; then
        break
    fi
    kill -0 "$RUNTIME_PID" 2>/dev/null || {
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "P6 BBR long-flow runtime exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
[ "$i" -lt 100 ] || {
    echo "timed out waiting for P6 BBR long-flow runtime" >&2
    exit 1
}

modprobe ifb >/dev/null 2>&1 || true
ip link add "$IFB_NAME" type ifb
ip link set "$IFB_NAME" up
tc qdisc add dev "$TUN_NAME" handle ffff: ingress
tc filter add dev "$TUN_NAME" parent ffff: protocol ip prio 1 u32 \
    match u32 0 0 action mirred egress redirect dev "$IFB_NAME"
tc qdisc replace dev "$IFB_NAME" root netem \
    delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
tc qdisc replace dev "$TUN_NAME" root netem \
    delay "${HALF_RTT_MS}ms" limit "$QUEUE_PKTS"

tc -s qdisc show dev "$TUN_NAME" > "$OUT/tun-qdisc-before.txt"
tc -s qdisc show dev "$IFB_NAME" > "$OUT/ifb-qdisc-before.txt"

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import hashlib
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
expected = int(sys.argv[3])
received = 0
digest = hashlib.sha256()
start = time.monotonic_ns()

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(90.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
    sock.connect((host, port))
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        received += len(chunk)
        digest.update(chunk)

elapsed = time.monotonic_ns() - start
if received != expected:
    raise SystemExit(f"received={received} expected={expected}")
goodput_mbps = received * 8.0 * 1000.0 / elapsed
print(
    f"bytes={received} elapsed_ns={elapsed} goodput_mbps={goodput_mbps:.6f} "
    f"sha256={digest.hexdigest()}"
)
PY

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stderr" >&2 || true
    echo "P6 BBR long-flow backend failed" >&2
    exit 1
fi
BACKEND_PID=

tc -s qdisc show dev "$TUN_NAME" > "$OUT/tun-qdisc-after.txt"
tc -s qdisc show dev "$IFB_NAME" > "$OUT/ifb-qdisc-after.txt"

ifb_drops=$(sed -n 's/.*(dropped \([0-9][0-9]*\),.*/\1/p' "$OUT/ifb-qdisc-after.txt" | head -n 1)
tun_drops=$(sed -n 's/.*(dropped \([0-9][0-9]*\),.*/\1/p' "$OUT/tun-qdisc-after.txt" | head -n 1)
[ -n "$ifb_drops" ] && [ "$ifb_drops" -eq 0 ] &&
[ -n "$tun_drops" ] && [ "$tun_drops" -eq 0 ] || {
    cat "$OUT/ifb-qdisc-after.txt" >&2 || true
    cat "$OUT/tun-qdisc-after.txt" >&2 || true
    echo "P6 BBR clean long-flow qdisc dropped packets: ifb=${ifb_drops:-missing} tun=${tun_drops:-missing}" >&2
    exit 1
}

sleep 0.2
kill -TERM "$RUNTIME_PID"
if ! wait "$RUNTIME_PID"; then
    RUNTIME_PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P6 BBR long-flow runtime failed" >&2
    exit 1
fi
RUNTIME_PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F 'cc=bbr-internal' "$OUT/runtime.stdout" >/dev/null
grep -F 'cc_bindings=1' "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_bind_failures=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_controller_errors=0' "$OUT/runtime.stderr" >/dev/null

events=$(grep -m1 ' cc_bindings=' "$OUT/runtime.stderr")
policy_updates=$(printf '%s\n' "$events" | sed -n 's/.* cc_policy_updates=\([0-9][0-9]*\).*/\1/p')
loss_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_loss_events=\([0-9][0-9]*\).*/\1/p')
timeout_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_timeout_events=\([0-9][0-9]*\).*/\1/p')
cwnd_bytes=$(printf '%s\n' "$events" | sed -n 's/.* cc_last_cwnd=\([0-9][0-9]*\).*/\1/p')
[ -n "$policy_updates" ] && [ "$policy_updates" -ge 1 ] &&
[ -n "$loss_events" ] && [ "$loss_events" -eq 0 ] &&
[ -n "$timeout_events" ] && [ "$timeout_events" -eq 0 ] &&
[ -n "$cwnd_bytes" ] && [ "$cwnd_bytes" -ge 1 ] || {
    echo "invalid BBR long-flow controller telemetry" >&2
    exit 1
}

delivery=$(grep -m1 'tcp-shift-p2-delivery:' "$OUT/runtime.stderr")
delivered_bytes=$(printf '%s\n' "$delivery" | sed -n 's/.* delivered_payload_bytes=\([0-9][0-9]*\).*/\1/p')
retransmit_events=$(printf '%s\n' "$delivery" | sed -n 's/.* retransmit_events=\([0-9][0-9]*\).*/\1/p')
metadata_failures=$(printf '%s\n' "$delivery" | sed -n 's/.* metadata_alloc_failures=\([0-9][0-9]*\).*/\1/p')
metadata_misses=$(printf '%s\n' "$delivery" | sed -n 's/.* metadata_misses=\([0-9][0-9]*\).*/\1/p')
live_slots=$(printf '%s\n' "$delivery" | sed -n 's/.* live_slots=\([0-9][0-9]*\).*/\1/p')
[ -n "$delivered_bytes" ] && [ "$delivered_bytes" -eq "$PAYLOAD_BYTES" ] &&
[ -n "$retransmit_events" ] && [ "$retransmit_events" -eq 0 ] &&
[ -n "$metadata_failures" ] && [ "$metadata_failures" -eq 0 ] &&
[ -n "$metadata_misses" ] && [ "$metadata_misses" -eq 0 ] &&
[ -n "$live_slots" ] && [ "$live_slots" -eq 0 ] || {
    echo "invalid BBR long-flow delivery telemetry" >&2
    exit 1
}

rate=$(grep -m1 'tcp-shift-p2-rate:' "$OUT/runtime.stderr")
valid_samples=$(printf '%s\n' "$rate" | sed -n 's/.* valid_samples=\([0-9][0-9]*\).*/\1/p')
max_rate=$(printf '%s\n' "$rate" | sed -n 's/.* max_rate_bytes_per_sec=\([0-9][0-9]*\).*/\1/p')
[ -n "$valid_samples" ] && [ "$valid_samples" -ge 1 ] &&
[ -n "$max_rate" ] && [ "$max_rate" -ge 1 ] || {
    echo "invalid BBR long-flow rate telemetry" >&2
    exit 1
}

pacing=$(grep -m1 'tcp-shift-p2-pacing:' "$OUT/runtime.stderr")
pacing_deferrals=$(printf '%s\n' "$pacing" | sed -n 's/.* deferrals=\([0-9][0-9]*\).*/\1/p')
pacing_resumes=$(printf '%s\n' "$pacing" | sed -n 's/.* resume_events=\([0-9][0-9]*\).*/\1/p')
pacing_errors=$(printf '%s\n' "$pacing" | sed -n 's/.* scheduler_errors=\([0-9][0-9]*\).*/\1/p')
pacing_tx_bytes=$(printf '%s\n' "$pacing" | sed -n 's/.* tx_bytes=\([0-9][0-9]*\).*/\1/p')
pacing_rate=$(printf '%s\n' "$pacing" | sed -n 's/.* last_rate_bytes_per_sec=\([0-9][0-9]*\).*/\1/p')
loop_errors=$(printf '%s\n' "$pacing" | sed -n 's/.* loop_callback_errors=\([0-9][0-9]*\).*/\1/p')
heap_current=$(printf '%s\n' "$pacing" | sed -n 's/.* heap_current=\([0-9][0-9]*\).*/\1/p')
[ -n "$pacing_deferrals" ] && [ "$pacing_deferrals" -ge 1 ] &&
[ -n "$pacing_resumes" ] && [ "$pacing_resumes" -ge 1 ] &&
[ -n "$pacing_errors" ] && [ "$pacing_errors" -eq 0 ] &&
[ -n "$pacing_tx_bytes" ] && [ "$pacing_tx_bytes" -eq "$PAYLOAD_BYTES" ] &&
[ -n "$pacing_rate" ] && [ "$pacing_rate" -ge 1 ] &&
[ -n "$loop_errors" ] && [ "$loop_errors" -eq 0 ] &&
[ -n "$heap_current" ] && [ "$heap_current" -eq 0 ] || {
    echo "invalid BBR long-flow shared-pacer telemetry" >&2
    exit 1
}

client_sha=$(sed -n 's/.* sha256=\([0-9a-f][0-9a-f]*\).*/\1/p' "$OUT/client.stdout")
backend_sha=$(sed -n 's/.* sha256=\([0-9a-f][0-9a-f]*\).*/\1/p' "$OUT/backend.stdout")
goodput=$(sed -n 's/.* goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.stdout")
[ -n "$client_sha" ] && [ "$client_sha" = "$backend_sha" ] && [ -n "$goodput" ] || {
    echo "P6 BBR long-flow payload/hash metrics missing or mismatched" >&2
    exit 1
}

printf 'p6_bbr_long_flow=ok base_rtt_ms=%s rate_mbit=%s bdp_bytes=%s queue_pkts=%s payload_bytes=%s goodput_mbps=%s cwnd_bytes=%s policy_updates=%s valid_rate_samples=%s max_rate_bytes_per_sec=%s pacing_deferrals=%s pacing_resumes=%s pacing_tx_bytes=%s qdisc_drops=%s/%s loss_events=%s timeout_events=%s payload_integrity=ok\n' \
    "$RTT_MS" "$RATE_MBIT" "$BDP_BYTES" "$QUEUE_PKTS" "$PAYLOAD_BYTES" \
    "$goodput" "$cwnd_bytes" "$policy_updates" "$valid_samples" "$max_rate" \
    "$pacing_deferrals" "$pacing_resumes" "$pacing_tx_bytes" \
    "$ifb_drops" "$tun_drops" "$loss_events" "$timeout_events" | tee "$OUT/summary.txt"

printf 'base_rtt_ms=%s\nrate_mbit=%s\nbdp_bytes=%s\nqueue_pkts=%s\npayload_bytes=%s\n' \
    "$RTT_MS" "$RATE_MBIT" "$BDP_BYTES" "$QUEUE_PKTS" "$PAYLOAD_BYTES" \
    > "$OUT/path.env"
echo "P6 internal BBR long-flow shared-pacer qualification passed"
