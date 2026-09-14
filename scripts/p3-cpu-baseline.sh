#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p3-cpu-baseline"
BINARY=${TCP_SHIFT_P3_BINARY:-"$BUILD/tcp-shift-p2"}
RUNTIME_NS=${TCP_SHIFT_P3_CPU_RUNTIME_NS:-tsp3cpur}
CLIENT_NS=${TCP_SHIFT_P3_CPU_CLIENT_NS:-tsp3cpuc}
TUN_NAME=${TCP_SHIFT_P3_CPU_TUN_NAME:-tsp3cpu0}
LWIP_IP=${TCP_SHIFT_P3_CPU_LWIP_IP:-10.245.0.2}
NETMASK=${TCP_SHIFT_P3_CPU_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P3_CPU_HOST_IP:-10.245.0.1}
PUBLIC_PORT=${TCP_SHIFT_P3_CPU_PUBLIC_PORT:-18104}
BACKEND_PORT=${TCP_SHIFT_P3_CPU_BACKEND_PORT:-19104}
RUNTIME_VETH=${TCP_SHIFT_P3_CPU_RUNTIME_VETH:-tsp3cpur0}
CLIENT_VETH=${TCP_SHIFT_P3_CPU_CLIENT_VETH:-tsp3cpuc0}
RUNTIME_LINK_IP=${TCP_SHIFT_P3_CPU_RUNTIME_LINK_IP:-192.0.2.17/30}
CLIENT_LINK_IP=${TCP_SHIFT_P3_CPU_CLIENT_LINK_IP:-192.0.2.18/30}
RUNTIME_GATEWAY=${TCP_SHIFT_P3_CPU_RUNTIME_GATEWAY:-192.0.2.17}
FLOW_COUNT=${TCP_SHIFT_P3_CPU_FLOWS:-4}
OPS_PER_FLOW=${TCP_SHIFT_P3_CPU_OPS_PER_FLOW:-512}
PAYLOAD_BYTES=${TCP_SHIFT_P3_CPU_PAYLOAD_BYTES:-64}
IDLE_SECONDS=${TCP_SHIFT_P3_CPU_IDLE_SECONDS:-1}
TOTAL_OPS=$((FLOW_COUNT * OPS_PER_FLOW))
TOTAL_BYTES=$((TOTAL_OPS * PAYLOAD_BYTES))
START_FILE="$OUT/start-workload"
PID=
BACKEND_PID=
CLIENT_PID=

mkdir -p "$OUT"
rm -f "$OUT"/* 2>/dev/null || true
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"

if [ "$(id -u)" -ne 0 ]; then
    echo "P3 CPU baseline must run as root" >&2
    exit 1
fi
[ -x "$BINARY" ] || {
    echo "missing P3 runtime binary: $BINARY" >&2
    exit 1
}

cleanup()
{
    set +e
    : > "$START_FILE" 2>/dev/null || true
    for child in "$CLIENT_PID" "$PID" "$BACKEND_PID"; do
        if [ -n "${child:-}" ] && kill -0 "$child" 2>/dev/null; then
            kill -TERM "$child" >/dev/null 2>&1 || true
            wait "$child" >/dev/null 2>&1 || true
        fi
    done
    ip netns del "$CLIENT_NS" >/dev/null 2>&1 || true
    ip netns del "$RUNTIME_NS" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

wait_for_line()
{
    file=$1
    pattern=$2
    label=$3
    count=0
    while [ "$count" -lt 300 ]; do
        if grep -F "$pattern" "$file" >/dev/null 2>&1; then
            return 0
        fi
        count=$((count + 1))
        sleep 0.05
    done
    echo "timed out waiting for $label: $pattern" >&2
    return 1
}

cpu_ticks()
{
    awk '{print $14 + $15}' "/proc/$PID/stat"
}

backend_established_count()
{
    ip netns exec "$RUNTIME_NS" \
        ss -Htan state established "( sport = :$BACKEND_PORT )" 2>/dev/null \
        | wc -l | tr -d ' '
}

wait_for_backend_count()
{
    wanted=$1
    count=0
    while [ "$count" -lt 300 ]; do
        current=$(backend_established_count)
        if [ "$current" -eq "$wanted" ]; then
            return 0
        fi
        count=$((count + 1))
        sleep 0.05
    done
    echo "backend established count did not reach $wanted; got $(backend_established_count)" >&2
    return 1
}

ip netns add "$RUNTIME_NS"
ip netns add "$CLIENT_NS"
ip -n "$RUNTIME_NS" link set lo up
ip -n "$CLIENT_NS" link set lo up
ip link add "$RUNTIME_VETH" type veth peer name "$CLIENT_VETH"
ip link set "$RUNTIME_VETH" netns "$RUNTIME_NS"
ip link set "$CLIENT_VETH" netns "$CLIENT_NS"
ip -n "$RUNTIME_NS" addr add "$RUNTIME_LINK_IP" dev "$RUNTIME_VETH"
ip -n "$CLIENT_NS" addr add "$CLIENT_LINK_IP" dev "$CLIENT_VETH"
ip -n "$RUNTIME_NS" link set "$RUNTIME_VETH" up
ip -n "$CLIENT_NS" link set "$CLIENT_VETH" up
ip netns exec "$RUNTIME_NS" sysctl -q -w net.ipv4.ip_forward=1
ip -n "$CLIENT_NS" route add "$LWIP_IP/32" via "$RUNTIME_GATEWAY"

ip netns exec "$RUNTIME_NS" python3 - "$BACKEND_PORT" "$FLOW_COUNT" "$OPS_PER_FLOW" "$PAYLOAD_BYTES" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import socket
import sys
import threading

port = int(sys.argv[1])
flow_count = int(sys.argv[2])
ops_per_flow = int(sys.argv[3])
payload_bytes = int(sys.argv[4])
errors = []
threads = []
completed = 0
lock = threading.Lock()


def recv_exact(conn, length):
    chunks = bytearray()
    while len(chunks) < length:
        data = conn.recv(length - len(chunks))
        if not data:
            raise RuntimeError(f"early EOF: wanted={length} got={len(chunks)}")
        chunks.extend(data)
    return bytes(chunks)


def serve(conn, slot):
    global completed
    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.settimeout(30.0)
        for _ in range(ops_per_flow):
            data = recv_exact(conn, payload_bytes)
            conn.sendall(data)
        while conn.recv(8192):
            pass
        try:
            conn.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        with lock:
            completed += 1
    except Exception as exc:
        errors.append((slot, repr(exc)))
    finally:
        conn.close()


server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(flow_count)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
for slot in range(flow_count):
    conn, _ = server.accept()
    thread = threading.Thread(target=serve, args=(conn, slot), daemon=False)
    thread.start()
    threads.append(thread)
print(f"backend-accepted={flow_count}", flush=True)
server.close()
for thread in threads:
    thread.join()
if errors:
    raise SystemExit(f"backend errors: {errors}")
print(f"backend-completed={completed}", flush=True)
PY
BACKEND_PID=$!
wait_for_line "$OUT/backend.stdout" 'backend-ready ' 'P3 CPU backend readiness'

ip netns exec "$RUNTIME_NS" "$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!
wait_for_line "$OUT/runtime.stdout" "tcp-shift-p2: ready tun=$TUN_NAME" 'P3 CPU runtime readiness'

clock_ticks=$(getconf CLK_TCK)
idle_start_ticks=$(cpu_ticks)
sleep "$IDLE_SECONDS"
idle_end_ticks=$(cpu_ticks)
idle_ticks=$((idle_end_ticks - idle_start_ticks))

ip netns exec "$CLIENT_NS" python3 - "$LWIP_IP" "$PUBLIC_PORT" "$FLOW_COUNT" \
    "$OPS_PER_FLOW" "$PAYLOAD_BYTES" "$START_FILE" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY' &
import concurrent.futures
import os
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
flow_count = int(sys.argv[3])
ops_per_flow = int(sys.argv[4])
payload_bytes = int(sys.argv[5])
start_file = sys.argv[6]
payload = bytes(((i * 31 + 7) & 0xFF) for i in range(payload_bytes))
sockets = []
for _ in range(flow_count):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(30.0)
    sock.connect((host, port))
    sockets.append(sock)
print(f"client-connected={flow_count}", flush=True)
while not os.path.exists(start_file):
    time.sleep(0.01)


def recv_exact(sock, length):
    chunks = bytearray()
    while len(chunks) < length:
        data = sock.recv(length - len(chunks))
        if not data:
            raise RuntimeError(f"early EOF: wanted={length} got={len(chunks)}")
        chunks.extend(data)
    return bytes(chunks)


def run_flow(sock):
    for _ in range(ops_per_flow):
        sock.sendall(payload)
        echoed = recv_exact(sock, payload_bytes)
        if echoed != payload:
            raise RuntimeError("echo payload mismatch")
    sock.shutdown(socket.SHUT_WR)
    while sock.recv(8192):
        pass
    sock.close()
    return ops_per_flow

start = time.monotonic_ns()
with concurrent.futures.ThreadPoolExecutor(max_workers=flow_count) as executor:
    completed = list(executor.map(run_flow, sockets))
elapsed_ns = time.monotonic_ns() - start
print(
    f"client-completed={flow_count} operations={sum(completed)} "
    f"payload_bytes={payload_bytes} elapsed_ns={elapsed_ns}",
    flush=True,
)
PY
CLIENT_PID=$!

wait_for_line "$OUT/client.stdout" "client-connected=$FLOW_COUNT" 'P3 CPU clients connected'
wait_for_line "$OUT/backend.stdout" "backend-accepted=$FLOW_COUNT" 'P3 CPU backend accepted'
wait_for_backend_count "$FLOW_COUNT"
work_start_ticks=$(cpu_ticks)
: > "$START_FILE"
if ! wait "$CLIENT_PID"; then
    CLIENT_PID=
    cat "$OUT/client.stdout" >&2 || true
    cat "$OUT/client.stderr" >&2 || true
    exit 1
fi
CLIENT_PID=
work_end_ticks=$(cpu_ticks)
work_ticks=$((work_end_ticks - work_start_ticks))

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    exit 1
fi
BACKEND_PID=
wait_for_backend_count 0

kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    exit 1
fi
PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "client-completed=$FLOW_COUNT operations=$TOTAL_OPS payload_bytes=$PAYLOAD_BYTES" "$OUT/client.stdout" >/dev/null
grep -F "backend-completed=$FLOW_COUNT" "$OUT/backend.stdout" >/dev/null
grep -F "bridge_accepts=$FLOW_COUNT bridge_backend_connects=$FLOW_COUNT bridge_public_to_backend_bytes=$TOTAL_BYTES bridge_backend_to_public_bytes=$TOTAL_BYTES bridge_active_flows=0" "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_backend_failures=0 bridge_public_errors=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'shutdown_bridge_active_flows=0 shutdown_bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

python3 - "$OUT/client.stdout" "$OUT/summary.json" \
    "$clock_ticks" "$IDLE_SECONDS" "$idle_ticks" "$work_ticks" "$TOTAL_OPS" "$TOTAL_BYTES" <<'PY'
import json
import re
import sys

client_path, out, hz_text, idle_seconds_text, idle_ticks_text, work_ticks_text, ops_text, bytes_text = sys.argv[1:]
hz = int(hz_text)
idle_seconds = float(idle_seconds_text)
idle_ticks = int(idle_ticks_text)
work_ticks = int(work_ticks_text)
operations = int(ops_text)
bytes_one_direction = int(bytes_text)
with open(client_path, encoding="utf-8") as handle:
    client_text = handle.read()
match = re.search(r"elapsed_ns=(\d+)", client_text)
if not match:
    raise SystemExit("missing small-operation elapsed time")
elapsed_ns = int(match.group(1))
if work_ticks <= 0:
    raise SystemExit("small-operation workload consumed no measurable runtime CPU ticks")
summary = {
    "clock_ticks_per_second": hz,
    "idle_seconds": idle_seconds,
    "idle_cpu_ticks": idle_ticks,
    "idle_cpu_ms": idle_ticks * 1000.0 / hz,
    "flows": int(re.search(r"client-connected=(\d+)", client_text).group(1)),
    "operations": operations,
    "payload_bytes": bytes_one_direction // operations,
    "bytes_each_direction": bytes_one_direction,
    "work_cpu_ticks": work_ticks,
    "work_cpu_ms": work_ticks * 1000.0 / hz,
    "work_cpu_us_per_operation": work_ticks * 1_000_000.0 / hz / operations,
    "wall_elapsed_ms": elapsed_ns / 1_000_000.0,
    "wall_operations_per_second": operations * 1_000_000_000.0 / elapsed_ns,
}
with open(out, "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(summary, sort_keys=True))
PY

cat "$OUT/summary.json"
echo "P3 idle and small-operation CPU measurement passed"
