#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
BINARY=${TCP_SHIFT_P6_BBR_MULTI_BINARY:-"$BUILD/tcp-shift-p6-bbr"}
FLOWS=${TCP_SHIFT_P6_BBR_MULTI_FLOWS:-4}
RTT_MS=${TCP_SHIFT_P6_BBR_MULTI_RTT_MS:-40}
RATE_MBIT=${TCP_SHIFT_P6_BBR_MULTI_RATE_MBIT:-10}
PAYLOAD_BYTES=${TCP_SHIFT_P6_BBR_MULTI_PAYLOAD_BYTES:-2097152}
OUT=${TCP_SHIFT_P6_BBR_MULTI_OUT:-"$BUILD/p6-bbr-multiflow"}

TUN_NAME=${TCP_SHIFT_P6_BBR_MULTI_TUN_NAME:-"tsp6mf$$"}
IFB_NAME=${TCP_SHIFT_P6_BBR_MULTI_IFB_NAME:-"p6mifb$$"}
LWIP_IP=${TCP_SHIFT_P6_BBR_MULTI_LWIP_IP:-10.247.0.2}
HOST_IP=${TCP_SHIFT_P6_BBR_MULTI_HOST_IP:-10.247.0.1}
NETMASK=${TCP_SHIFT_P6_BBR_MULTI_NETMASK:-255.255.255.252}
PUBLIC_PORT=${TCP_SHIFT_P6_BBR_MULTI_PUBLIC_PORT:-18163}
BACKEND_PORT=${TCP_SHIFT_P6_BBR_MULTI_BACKEND_PORT:-19163}

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
    echo "P6 BBR multi-flow harness must run as root for TUN, IFB and netem" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing internal BBR qualification binary: $BINARY" >&2
    exit 1
}
command -v ip >/dev/null 2>&1 || { echo "ip is required" >&2; exit 1; }
command -v tc >/dev/null 2>&1 || { echo "tc is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 1; }

for value_name in FLOWS RTT_MS RATE_MBIT PAYLOAD_BYTES; do
    eval value=\$$value_name
    case "$value" in ''|*[!0-9]*) echo "$value_name must be an integer" >&2; exit 1;; esac
done
[ "$FLOWS" -ge 2 ] || { echo "FLOWS must be at least 2" >&2; exit 1; }
[ "$RTT_MS" -gt 0 ] && [ $((RTT_MS % 2)) -eq 0 ] || {
    echo "RTT_MS must be a positive even integer" >&2
    exit 1
}
[ "$RATE_MBIT" -gt 0 ] || { echo "RATE_MBIT must be positive" >&2; exit 1; }
[ "$PAYLOAD_BYTES" -gt 0 ] || { echo "PAYLOAD_BYTES must be positive" >&2; exit 1; }

HALF_RTT_MS=$((RTT_MS / 2))
BDP_BYTES=$((RATE_MBIT * RTT_MS * 125))
BDP_PKTS=$(((BDP_BYTES + 1459) / 1460))
QUEUE_PKTS=${TCP_SHIFT_P6_BBR_MULTI_QUEUE_PKTS:-$((BDP_PKTS * 8))}
[ "$QUEUE_PKTS" -ge 64 ] || QUEUE_PKTS=64
WIRE_BYTES_PER_FLOW=$((PAYLOAD_BYTES + 12))
TOTAL_WIRE_BYTES=$((WIRE_BYTES_PER_FLOW * FLOWS))

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

python3 - "$BACKEND_PORT" "$FLOWS" "$PAYLOAD_BYTES" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import struct
import sys
import threading

port = int(sys.argv[1])
flows = int(sys.argv[2])
payload_bytes = int(sys.argv[3])

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(flows)
print(f"backend-ready port={port} flows={flows}", flush=True)

connections = []
for _ in range(flows):
    conn, _ = server.accept()
    conn.settimeout(90.0)
    connections.append(conn)

start = threading.Barrier(flows)
results = [None] * flows


def worker(index, conn):
    start.wait()
    payload = bytes((((offset * 73) + 19 + index * 17) & 0xFF)
                    for offset in range(payload_bytes))
    header = struct.pack("!IQ", index, payload_bytes)
    digest = hashlib.sha256(payload).hexdigest()
    conn.sendall(header)
    conn.sendall(payload)
    conn.shutdown(socket.SHUT_WR)
    while conn.recv(4096):
        pass
    conn.close()
    results[index] = digest


threads = [
    threading.Thread(target=worker, args=(index, conn), daemon=True)
    for index, conn in enumerate(connections)
]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
server.close()

for index, digest in enumerate(results):
    if digest is None:
        raise SystemExit(f"missing backend result flow={index}")
    print(
        f"backend-flow={index} payload_bytes={payload_bytes} "
        f"wire_bytes={payload_bytes + 12} sha256={digest}",
        flush=True,
    )
print(
    f"backend-complete flows={flows} payload_bytes_per_flow={payload_bytes} "
    f"total_wire_bytes={(payload_bytes + 12) * flows}",
    flush=True,
)
PY
BACKEND_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$BACKEND_PID" 2>/dev/null || {
        cat "$OUT/backend.stderr" >&2 || true
        echo "P6 BBR multi-flow backend exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
grep -F "backend-ready port=$BACKEND_PORT flows=$FLOWS" "$OUT/backend.stdout" >/dev/null || {
    echo "timed out waiting for P6 BBR multi-flow backend" >&2
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
        echo "P6 BBR multi-flow runtime exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
[ "$i" -lt 100 ] || {
    echo "timed out waiting for P6 BBR multi-flow runtime" >&2
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

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$FLOWS" "$PAYLOAD_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import hashlib
import json
import socket
import struct
import sys
import threading
import time

host = sys.argv[1]
port = int(sys.argv[2])
flows = int(sys.argv[3])
expected_payload = int(sys.argv[4])
barrier = threading.Barrier(flows)
lock = threading.Lock()
results = []


def recv_exact(sock, length):
    chunks = []
    remaining = length
    while remaining:
        chunk = sock.recv(min(65536, remaining))
        if not chunk:
            raise RuntimeError(f"early EOF with {remaining} bytes remaining")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def worker(client_index):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(90.0)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
        barrier.wait()
        start_ns = time.monotonic_ns()
        sock.connect((host, port))
        header = recv_exact(sock, 12)
        flow_id, payload_bytes = struct.unpack("!IQ", header)
        if payload_bytes != expected_payload:
            raise RuntimeError(
                f"flow={flow_id} payload={payload_bytes} expected={expected_payload}"
            )
        payload = recv_exact(sock, payload_bytes)
        tail = sock.recv(1)
        if tail:
            raise RuntimeError(f"flow={flow_id} unexpected tail data")
        elapsed_ns = time.monotonic_ns() - start_ns

    expected = bytes((((offset * 73) + 19 + flow_id * 17) & 0xFF)
                     for offset in range(payload_bytes))
    digest = hashlib.sha256(payload).hexdigest()
    expected_digest = hashlib.sha256(expected).hexdigest()
    if digest != expected_digest:
        raise RuntimeError(
            f"flow={flow_id} hash mismatch got={digest} expected={expected_digest}"
        )
    goodput_mbps = (payload_bytes + 12) * 8.0 * 1000.0 / elapsed_ns
    with lock:
        results.append(
            {
                "client_index": client_index,
                "flow_id": flow_id,
                "elapsed_ns": elapsed_ns,
                "wire_bytes": payload_bytes + 12,
                "goodput_mbps": goodput_mbps,
                "sha256": digest,
            }
        )


threads = [threading.Thread(target=worker, args=(i,)) for i in range(flows)]
start_all_ns = time.monotonic_ns()
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
elapsed_all_ns = time.monotonic_ns() - start_all_ns

if len(results) != flows:
    raise SystemExit(f"completed={len(results)} expected={flows}")
flow_ids = sorted(item["flow_id"] for item in results)
if flow_ids != list(range(flows)):
    raise SystemExit(f"unexpected flow ids: {flow_ids}")

throughputs = [item["goodput_mbps"] for item in results]
sum_x = sum(throughputs)
sum_x2 = sum(value * value for value in throughputs)
jain = (sum_x * sum_x) / (flows * sum_x2) if sum_x2 else 0.0
aggregate_mbps = flows * (expected_payload + 12) * 8.0 * 1000.0 / elapsed_all_ns

for item in sorted(results, key=lambda value: value["flow_id"]):
    print(
        f"client-flow={item['flow_id']} elapsed_ns={item['elapsed_ns']} "
        f"wire_bytes={item['wire_bytes']} goodput_mbps={item['goodput_mbps']:.6f} "
        f"sha256={item['sha256']}"
    )
print(
    f"client-complete flows={flows} total_wire_bytes={flows * (expected_payload + 12)} "
    f"elapsed_all_ns={elapsed_all_ns} aggregate_goodput_mbps={aggregate_mbps:.6f} "
    f"min_flow_goodput_mbps={min(throughputs):.6f} "
    f"max_flow_goodput_mbps={max(throughputs):.6f} jain_fairness={jain:.6f}"
)
with open(sys.argv[0] if False else "/dev/null", "w"):
    pass
PY

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stderr" >&2 || true
    cat "$OUT/client.stderr" >&2 || true
    echo "P6 BBR multi-flow backend failed" >&2
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
    echo "P6 BBR multi-flow qdisc dropped packets: ifb=${ifb_drops:-missing} tun=${tun_drops:-missing}" >&2
    exit 1
}

sleep 0.2
kill -TERM "$RUNTIME_PID"
if ! wait "$RUNTIME_PID"; then
    RUNTIME_PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P6 BBR multi-flow runtime failed" >&2
    exit 1
fi
RUNTIME_PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "bridge_peak_active_flows=$FLOWS" "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_active_flows=0' "$OUT/runtime.stderr" >/dev/null
grep -F "cc_bindings=$FLOWS" "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_bind_failures=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_controller_errors=0' "$OUT/runtime.stderr" >/dev/null

events=$(grep -m1 ' cc_bindings=' "$OUT/runtime.stderr")
loss_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_loss_events=\([0-9][0-9]*\).*/\1/p')
timeout_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_timeout_events=\([0-9][0-9]*\).*/\1/p')
[ -n "$loss_events" ] && [ "$loss_events" -eq 0 ] &&
[ -n "$timeout_events" ] && [ "$timeout_events" -eq 0 ] || {
    echo "multi-flow clean path entered recovery: loss=${loss_events:-missing} timeout=${timeout_events:-missing}" >&2
    exit 1
}

delivery=$(grep -m1 'tcp-shift-p2-delivery:' "$OUT/runtime.stderr")
delivered_bytes=$(printf '%s\n' "$delivery" | sed -n 's/.* delivered_payload_bytes=\([0-9][0-9]*\).*/\1/p')
retransmit_events=$(printf '%s\n' "$delivery" | sed -n 's/.* retransmit_events=\([0-9][0-9]*\).*/\1/p')
live_slots=$(printf '%s\n' "$delivery" | sed -n 's/.* live_slots=\([0-9][0-9]*\).*/\1/p')
[ -n "$delivered_bytes" ] && [ "$delivered_bytes" -eq "$TOTAL_WIRE_BYTES" ] &&
[ -n "$retransmit_events" ] && [ "$retransmit_events" -eq 0 ] &&
[ -n "$live_slots" ] && [ "$live_slots" -eq 0 ] || {
    echo "invalid BBR multi-flow delivery telemetry" >&2
    exit 1
}

pacing=$(grep -m1 'tcp-shift-p2-pacing:' "$OUT/runtime.stderr")
pacing_deferrals=$(printf '%s\n' "$pacing" | sed -n 's/.* deferrals=\([0-9][0-9]*\).*/\1/p')
pacing_resumes=$(printf '%s\n' "$pacing" | sed -n 's/.* resume_events=\([0-9][0-9]*\).*/\1/p')
pacing_errors=$(printf '%s\n' "$pacing" | sed -n 's/.* scheduler_errors=\([0-9][0-9]*\).*/\1/p')
pacing_tx_bytes=$(printf '%s\n' "$pacing" | sed -n 's/.* tx_bytes=\([0-9][0-9]*\).*/\1/p')
loop_errors=$(printf '%s\n' "$pacing" | sed -n 's/.* loop_callback_errors=\([0-9][0-9]*\).*/\1/p')
heap_current=$(printf '%s\n' "$pacing" | sed -n 's/.* heap_current=\([0-9][0-9]*\).*/\1/p')
heap_peak=$(printf '%s\n' "$pacing" | sed -n 's/.* heap_peak=\([0-9][0-9]*\).*/\1/p')
[ -n "$pacing_deferrals" ] && [ "$pacing_deferrals" -ge "$FLOWS" ] &&
[ -n "$pacing_resumes" ] && [ "$pacing_resumes" -ge "$FLOWS" ] &&
[ -n "$pacing_errors" ] && [ "$pacing_errors" -eq 0 ] &&
[ -n "$pacing_tx_bytes" ] && [ "$pacing_tx_bytes" -eq "$TOTAL_WIRE_BYTES" ] &&
[ -n "$loop_errors" ] && [ "$loop_errors" -eq 0 ] &&
[ -n "$heap_current" ] && [ "$heap_current" -eq 0 ] &&
[ -n "$heap_peak" ] && [ "$heap_peak" -ge 2 ] || {
    echo "invalid BBR multi-flow shared-pacer telemetry" >&2
    exit 1
}

aggregate=$(sed -n 's/.* aggregate_goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.stdout" | tail -n 1)
min_flow=$(sed -n 's/.* min_flow_goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.stdout" | tail -n 1)
max_flow=$(sed -n 's/.* max_flow_goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.stdout" | tail -n 1)
jain=$(sed -n 's/.* jain_fairness=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.stdout" | tail -n 1)
[ -n "$aggregate" ] && [ -n "$min_flow" ] && [ -n "$max_flow" ] && [ -n "$jain" ] || {
    echo "missing BBR multi-flow client metrics" >&2
    exit 1
}

printf 'p6_bbr_multiflow=ok flows=%s base_rtt_ms=%s rate_mbit=%s bdp_bytes=%s queue_pkts=%s payload_bytes_per_flow=%s total_wire_bytes=%s aggregate_goodput_mbps=%s min_flow_goodput_mbps=%s max_flow_goodput_mbps=%s jain_fairness=%s heap_peak=%s pacing_deferrals=%s pacing_resumes=%s qdisc_drops=%s/%s loss_events=%s timeout_events=%s payload_integrity=ok\n' \
    "$FLOWS" "$RTT_MS" "$RATE_MBIT" "$BDP_BYTES" "$QUEUE_PKTS" \
    "$PAYLOAD_BYTES" "$TOTAL_WIRE_BYTES" "$aggregate" "$min_flow" "$max_flow" "$jain" \
    "$heap_peak" "$pacing_deferrals" "$pacing_resumes" "$ifb_drops" "$tun_drops" \
    "$loss_events" "$timeout_events" | tee "$OUT/summary.txt"

echo "P6 internal BBR multi-flow shared-pacer qualification passed"
