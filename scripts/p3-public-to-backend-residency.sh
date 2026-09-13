#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p3-public-to-backend-residency"
BINARY=${TCP_SHIFT_P3_BINARY:-"$BUILD/tcp-shift-p2"}
RUNTIME_NS=${TCP_SHIFT_P3_P2B_RUNTIME_NS:-tsp3p2br}
CLIENT_NS=${TCP_SHIFT_P3_P2B_CLIENT_NS:-tsp3p2bc}
TUN_NAME=${TCP_SHIFT_P3_P2B_TUN_NAME:-tsp3p2b0}
LWIP_IP=${TCP_SHIFT_P3_P2B_LWIP_IP:-10.242.0.2}
NETMASK=${TCP_SHIFT_P3_P2B_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P3_P2B_HOST_IP:-10.242.0.1}
PUBLIC_PORT=${TCP_SHIFT_P3_P2B_PUBLIC_PORT:-18101}
BACKEND_PORT=${TCP_SHIFT_P3_P2B_BACKEND_PORT:-19101}
RUNTIME_VETH=${TCP_SHIFT_P3_P2B_RUNTIME_VETH:-tsp3p2br0}
CLIENT_VETH=${TCP_SHIFT_P3_P2B_CLIENT_VETH:-tsp3p2bc0}
RUNTIME_LINK_IP=${TCP_SHIFT_P3_P2B_RUNTIME_LINK_IP:-192.0.2.5/30}
CLIENT_LINK_IP=${TCP_SHIFT_P3_P2B_CLIENT_LINK_IP:-192.0.2.6/30}
RUNTIME_GATEWAY=${TCP_SHIFT_P3_P2B_RUNTIME_GATEWAY:-192.0.2.5}
FLOW_COUNT=${TCP_SHIFT_P3_P2B_FLOWS:-8}
PAYLOAD_BYTES=${TCP_SHIFT_P3_P2B_PAYLOAD_BYTES:-1048576}
TOTAL_BYTES=$((FLOW_COUNT * PAYLOAD_BYTES))
START_FILE="$OUT/start-send"
RELEASE_FILE="$OUT/release-backend"
PID=
BACKEND_PID=
CLIENT_PID=

mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/*.tsv "$OUT"/*.json "$START_FILE" "$RELEASE_FILE" 2>/dev/null || true
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"
: > "$OUT/measurements.tsv"

if [ "$(id -u)" -ne 0 ]; then
    echo "P3 active residency harness must run as root" >&2
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
    : > "$RELEASE_FILE" 2>/dev/null || true
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
    while [ "$count" -lt 200 ]; do
        if grep -F "$pattern" "$file" >/dev/null 2>&1; then
            return 0
        fi
        count=$((count + 1))
        sleep 0.05
    done
    echo "timed out waiting for $label: $pattern" >&2
    return 1
}

backend_established_count()
{
    ip netns exec "$RUNTIME_NS" \
        ss -Htan state established "( sport = :$BACKEND_PORT )" 2>/dev/null \
        | wc -l | tr -d ' '
}

backend_recvq_bytes()
{
    ip netns exec "$RUNTIME_NS" \
        ss -Htn state established "( sport = :$BACKEND_PORT )" 2>/dev/null \
        | awk '{sum += $2} END {print sum + 0}'
}

wait_for_backend_count()
{
    wanted=$1
    count=0
    while [ "$count" -lt 200 ]; do
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

sample_process()
{
    label=$1
    status="/proc/$PID/status"
    smaps="/proc/$PID/smaps_rollup"

    [ -r "$status" ] && [ -r "$smaps" ] || {
        echo "runtime disappeared before $label sample" >&2
        exit 1
    }

    vmrss=$(awk '/^VmRSS:/ {print $2; exit}' "$status")
    rss=$(awk '/^Rss:/ {print $2; exit}' "$smaps")
    pss=$(awk '/^Pss:/ {print $2; exit}' "$smaps")
    private_dirty=$(awk '/^Private_Dirty:/ {print $2; exit}' "$smaps")
    anonymous=$(awk '/^Anonymous:/ {print $2; exit}' "$smaps")
    recvq=$(backend_recvq_bytes)
    established=$(backend_established_count)
    fd_count=$(find "/proc/$PID/fd" -mindepth 1 -maxdepth 1 -printf x 2>/dev/null | wc -c | tr -d ' ')

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$FLOW_COUNT" "$vmrss" "$rss" "$pss" "$private_dirty" \
        "$anonymous" "$fd_count" "$established" "$recvq" >> "$OUT/measurements.tsv"
}

printf 'stage\tflows\tvmrss_kb\trss_kb\tpss_kb\tprivate_dirty_kb\tanonymous_kb\tfd_count\tbackend_established\tbackend_recvq_bytes\n' \
    >> "$OUT/measurements.tsv"

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

ip netns exec "$RUNTIME_NS" python3 - "$BACKEND_PORT" "$FLOW_COUNT" "$RELEASE_FILE" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import os
import socket
import sys
import time

port = int(sys.argv[1])
count = int(sys.argv[2])
release_file = sys.argv[3]
connections = []
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(count)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
for _ in range(count):
    conn, _ = server.accept()
    conn.settimeout(30.0)
    connections.append(conn)
print(f"backend-accepted={len(connections)}", flush=True)
while not os.path.exists(release_file):
    time.sleep(0.02)
total = 0
for conn in connections:
    while True:
        data = conn.recv(65536)
        if not data:
            break
        total += len(data)
    try:
        conn.shutdown(socket.SHUT_WR)
    except OSError:
        pass
    conn.close()
server.close()
print(f"backend-drained={len(connections)} total_bytes={total}", flush=True)
PY
BACKEND_PID=$!
wait_for_line "$OUT/backend.stdout" 'backend-ready ' 'P3 blocked backend readiness'

ip netns exec "$RUNTIME_NS" "$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!
wait_for_line "$OUT/runtime.stdout" "tcp-shift-p2: ready tun=$TUN_NAME" 'P3 active runtime readiness'

ip netns exec "$CLIENT_NS" python3 - "$LWIP_IP" "$PUBLIC_PORT" "$FLOW_COUNT" \
    "$PAYLOAD_BYTES" "$START_FILE" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY' &
import concurrent.futures
import os
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
count = int(sys.argv[3])
payload_bytes = int(sys.argv[4])
start_file = sys.argv[5]
payload = bytes(((i * 67 + 13) & 0xFF) for i in range(payload_bytes))
sockets = []
for _ in range(count):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((host, port))
    sockets.append(sock)
print(f"client-connected={len(sockets)}", flush=True)
while not os.path.exists(start_file):
    time.sleep(0.02)

def send_one(sock):
    sock.sendall(payload)
    sock.shutdown(socket.SHUT_WR)
    return payload_bytes

with concurrent.futures.ThreadPoolExecutor(max_workers=count) as executor:
    futures = [executor.submit(send_one, sock) for sock in sockets]
    print(f"client-send-started={len(futures)}", flush=True)
    sent = [future.result() for future in futures]
print(f"client-sent={len(sent)} total_bytes={sum(sent)}", flush=True)
for sock in sockets:
    while True:
        data = sock.recv(8192)
        if not data:
            break
    sock.close()
print(f"client-drained={count}", flush=True)
PY
CLIENT_PID=$!

wait_for_line "$OUT/client.stdout" "client-connected=$FLOW_COUNT" 'P3 active clients connected'
wait_for_line "$OUT/backend.stdout" "backend-accepted=$FLOW_COUNT" 'P3 active backend accepted'
wait_for_backend_count "$FLOW_COUNT"
sample_process idle

: > "$START_FILE"
wait_for_line "$OUT/client.stdout" "client-send-started=$FLOW_COUNT" 'P3 active senders started'

# Sample while sendall() calls are intentionally unable to finish because the
# backend has not consumed data. This is the state whose residency matters.
sleep 0.5
sample_process active-public-to-backend
active_recvq=$(backend_recvq_bytes)
[ "$active_recvq" -gt 0 ] || {
    echo "blocked backend has no queued bytes at active sample" >&2
    exit 1
}

: > "$RELEASE_FILE"
if ! wait "$CLIENT_PID"; then
    CLIENT_PID=
    cat "$OUT/client.stdout" >&2 || true
    cat "$OUT/client.stderr" >&2 || true
    exit 1
fi
CLIENT_PID=
if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    exit 1
fi
BACKEND_PID=
wait_for_backend_count 0
sleep 0.2
sample_process drained

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

peak_pending=$(grep -Eo 'bridge_peak_pending_public_bytes=[0-9]+' "$OUT/runtime.stderr" | tail -1 | cut -d= -f2)
write_blocked=$(grep -Eo 'bridge_backend_write_blocked_events=[0-9]+' "$OUT/runtime.stderr" | tail -1 | cut -d= -f2)
[ -n "$peak_pending" ] && [ "$peak_pending" -gt 0 ] || {
    echo "public-to-backend active run never retained a pending lwIP pbuf" >&2
    exit 1
}
[ -n "$write_blocked" ] && [ "$write_blocked" -gt 0 ] || {
    echo "public-to-backend active run never reached backend EAGAIN" >&2
    exit 1
}

grep -F "client-sent=$FLOW_COUNT total_bytes=$TOTAL_BYTES" "$OUT/client.stdout" >/dev/null
grep -F "client-drained=$FLOW_COUNT" "$OUT/client.stdout" >/dev/null
grep -F "backend-drained=$FLOW_COUNT total_bytes=$TOTAL_BYTES" "$OUT/backend.stdout" >/dev/null
grep -F "bridge_accepts=$FLOW_COUNT bridge_backend_connects=$FLOW_COUNT bridge_public_to_backend_bytes=$TOTAL_BYTES bridge_backend_to_public_bytes=0 bridge_active_flows=0" "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_backend_failures=0 bridge_public_errors=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'shutdown_bridge_active_flows=0 shutdown_bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

python3 - "$OUT/measurements.tsv" "$OUT/summary.json" "$peak_pending" "$write_blocked" <<'PY'
import csv
import json
import sys

path, out, peak_pending, write_blocked = sys.argv[1:]
with open(path, newline="", encoding="utf-8") as handle:
    rows = {row["stage"]: row for row in csv.DictReader(handle, delimiter="\t")}
for name in ("idle", "active-public-to-backend", "drained"):
    if name not in rows:
        raise SystemExit(f"missing P3 active sample: {name}")
idle = rows["idle"]
active = rows["active-public-to-backend"]
drained = rows["drained"]
flows = int(active["flows"])
summary = {
    "flows": flows,
    "idle_pss_kb": int(idle["pss_kb"]),
    "active_pss_kb": int(active["pss_kb"]),
    "active_pss_delta_kb": int(active["pss_kb"]) - int(idle["pss_kb"]),
    "active_pss_delta_kb_per_flow": (int(active["pss_kb"]) - int(idle["pss_kb"])) / flows,
    "idle_private_dirty_kb": int(idle["private_dirty_kb"]),
    "active_private_dirty_kb": int(active["private_dirty_kb"]),
    "active_private_dirty_delta_kb": int(active["private_dirty_kb"]) - int(idle["private_dirty_kb"]),
    "active_backend_recvq_bytes": int(active["backend_recvq_bytes"]),
    "bridge_peak_pending_public_bytes": int(peak_pending),
    "bridge_backend_write_blocked_events": int(write_blocked),
    "drained_pss_kb": int(drained["pss_kb"]),
}
with open(out, "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(summary, sort_keys=True))
PY

cat "$OUT/measurements.tsv"
cat "$OUT/summary.json"
echo "P3 public-to-backend active residency measurement passed"
