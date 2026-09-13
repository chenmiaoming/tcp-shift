#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p3-backend-to-public-residency"
BINARY=${TCP_SHIFT_P3_BINARY:-"$BUILD/tcp-shift-p2"}
RUNTIME_NS=${TCP_SHIFT_P3_B2P_RUNTIME_NS:-tsp3b2pr}
CLIENT_NS=${TCP_SHIFT_P3_B2P_CLIENT_NS:-tsp3b2pc}
TUN_NAME=${TCP_SHIFT_P3_B2P_TUN_NAME:-tsp3b2p0}
LWIP_IP=${TCP_SHIFT_P3_B2P_LWIP_IP:-10.243.0.2}
NETMASK=${TCP_SHIFT_P3_B2P_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P3_B2P_HOST_IP:-10.243.0.1}
PUBLIC_PORT=${TCP_SHIFT_P3_B2P_PUBLIC_PORT:-18102}
BACKEND_PORT=${TCP_SHIFT_P3_B2P_BACKEND_PORT:-19102}
RUNTIME_VETH=${TCP_SHIFT_P3_B2P_RUNTIME_VETH:-tsp3b2pr0}
CLIENT_VETH=${TCP_SHIFT_P3_B2P_CLIENT_VETH:-tsp3b2pc0}
RUNTIME_LINK_IP=${TCP_SHIFT_P3_B2P_RUNTIME_LINK_IP:-192.0.2.9/30}
CLIENT_LINK_IP=${TCP_SHIFT_P3_B2P_CLIENT_LINK_IP:-192.0.2.10/30}
RUNTIME_GATEWAY=${TCP_SHIFT_P3_B2P_RUNTIME_GATEWAY:-192.0.2.9}
FLOW_COUNT=${TCP_SHIFT_P3_B2P_FLOWS:-8}
PAYLOAD_BYTES=${TCP_SHIFT_P3_B2P_PAYLOAD_BYTES:-1048576}
CLIENT_RCVBUF=${TCP_SHIFT_P3_B2P_CLIENT_RCVBUF:-4096}
TOTAL_BYTES=$((FLOW_COUNT * PAYLOAD_BYTES))
START_FILE="$OUT/start-backend-send"
RELEASE_FILE="$OUT/release-client-read"
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
    echo "P3 backend-to-public residency harness must run as root" >&2
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

backend_sendq_bytes()
{
    ip netns exec "$RUNTIME_NS" \
        ss -Htn state established "( sport = :$BACKEND_PORT )" 2>/dev/null \
        | awk '{sum += $3} END {print sum + 0}'
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
    sendq=$(backend_sendq_bytes)
    established=$(backend_established_count)
    fd_count=$(find "/proc/$PID/fd" -mindepth 1 -maxdepth 1 -printf x 2>/dev/null | wc -c | tr -d ' ')

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$FLOW_COUNT" "$vmrss" "$rss" "$pss" "$private_dirty" \
        "$anonymous" "$fd_count" "$established" "$sendq" >> "$OUT/measurements.tsv"
}

printf 'stage\tflows\tvmrss_kb\trss_kb\tpss_kb\tprivate_dirty_kb\tanonymous_kb\tfd_count\tbackend_established\tbackend_sendq_bytes\n' \
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

ip netns exec "$RUNTIME_NS" python3 - "$BACKEND_PORT" "$FLOW_COUNT" \
    "$PAYLOAD_BYTES" "$START_FILE" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import concurrent.futures
import os
import socket
import sys
import time

port = int(sys.argv[1])
count = int(sys.argv[2])
payload_bytes = int(sys.argv[3])
start_file = sys.argv[4]
payload = bytes(((i * 71 + 19) & 0xFF) for i in range(payload_bytes))
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
while not os.path.exists(start_file):
    time.sleep(0.02)

def send_one(conn):
    # The public client half-closes first, so the backend receives EOF while its
    # transmit direction remains independently usable.
    while True:
        data = conn.recv(8192)
        if not data:
            break
    conn.sendall(payload)
    conn.shutdown(socket.SHUT_WR)
    return payload_bytes

with concurrent.futures.ThreadPoolExecutor(max_workers=count) as executor:
    futures = [executor.submit(send_one, conn) for conn in connections]
    print(f"backend-send-started={len(futures)}", flush=True)
    sent = [future.result() for future in futures]
print(f"backend-sent={len(sent)} total_bytes={sum(sent)}", flush=True)
for conn in connections:
    conn.close()
server.close()
print(f"backend-drained={count}", flush=True)
PY
BACKEND_PID=$!
wait_for_line "$OUT/backend.stdout" 'backend-ready ' 'P3 backend-to-public backend readiness'

ip netns exec "$RUNTIME_NS" "$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!
wait_for_line "$OUT/runtime.stdout" "tcp-shift-p2: ready tun=$TUN_NAME" 'P3 backend-to-public runtime readiness'

ip netns exec "$CLIENT_NS" python3 - "$LWIP_IP" "$PUBLIC_PORT" "$FLOW_COUNT" \
    "$PAYLOAD_BYTES" "$CLIENT_RCVBUF" "$RELEASE_FILE" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY' &
import os
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
count = int(sys.argv[3])
payload_bytes = int(sys.argv[4])
requested_rcvbuf = int(sys.argv[5])
release_file = sys.argv[6]
sockets = []
actual_buffers = []
for _ in range(count):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, requested_rcvbuf)
    sock.settimeout(30.0)
    sock.connect((host, port))
    sock.shutdown(socket.SHUT_WR)
    sockets.append(sock)
    actual_buffers.append(sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF))
print(
    f"client-connected={len(sockets)} requested_rcvbuf={requested_rcvbuf} "
    f"actual_rcvbuf_min={min(actual_buffers)} actual_rcvbuf_max={max(actual_buffers)}",
    flush=True,
)
while not os.path.exists(release_file):
    time.sleep(0.02)
total = 0
for sock in sockets:
    received = 0
    while True:
        data = sock.recv(65536)
        if not data:
            break
        received += len(data)
    if received != payload_bytes:
        raise SystemExit(f"client payload mismatch: expected={payload_bytes} got={received}")
    total += received
    sock.close()
print(f"client-drained={count} total_bytes={total}", flush=True)
PY
CLIENT_PID=$!

wait_for_line "$OUT/client.stdout" "client-connected=$FLOW_COUNT" 'P3 backend-to-public clients connected'
wait_for_line "$OUT/backend.stdout" "backend-accepted=$FLOW_COUNT" 'P3 backend-to-public backend accepted'
wait_for_backend_count "$FLOW_COUNT"
sample_process idle

: > "$START_FILE"
wait_for_line "$OUT/backend.stdout" "backend-send-started=$FLOW_COUNT" 'P3 backend senders started'

# The public clients deliberately do not read. Their small receive buffers
# shrink the advertised public receive window, forcing lwIP's send queues to
# remain populated and the bridge to suppress backend reads under send pressure.
sleep 0.5
sample_process active-backend-to-public

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

read_blocked=$(grep -Eo 'bridge_backend_read_blocked_events=[0-9]+' "$OUT/runtime.stderr" | tail -1 | cut -d= -f2)
[ -n "$read_blocked" ] && [ "$read_blocked" -gt 0 ] || {
    echo "backend-to-public active run never reached lwIP send pressure" >&2
    exit 1
}

grep -F "client-drained=$FLOW_COUNT total_bytes=$TOTAL_BYTES" "$OUT/client.stdout" >/dev/null
grep -F "backend-sent=$FLOW_COUNT total_bytes=$TOTAL_BYTES" "$OUT/backend.stdout" >/dev/null
grep -F "backend-drained=$FLOW_COUNT" "$OUT/backend.stdout" >/dev/null
grep -F "bridge_accepts=$FLOW_COUNT bridge_backend_connects=$FLOW_COUNT bridge_public_to_backend_bytes=0 bridge_backend_to_public_bytes=$TOTAL_BYTES bridge_active_flows=0" "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_backend_failures=0 bridge_public_errors=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'shutdown_bridge_active_flows=0 shutdown_bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

python3 - "$OUT/measurements.tsv" "$OUT/summary.json" "$read_blocked" "$OUT/client.stdout" <<'PY'
import csv
import json
import re
import sys

path, out, read_blocked, client_path = sys.argv[1:]
with open(path, newline="", encoding="utf-8") as handle:
    rows = {row["stage"]: row for row in csv.DictReader(handle, delimiter="\t")}
for name in ("idle", "active-backend-to-public", "drained"):
    if name not in rows:
        raise SystemExit(f"missing P3 backend-to-public sample: {name}")
idle = rows["idle"]
active = rows["active-backend-to-public"]
drained = rows["drained"]
flows = int(active["flows"])
active_pss_delta = int(active["pss_kb"]) - int(idle["pss_kb"])
if active_pss_delta <= 0:
    raise SystemExit(f"active backend-to-public sample did not increase PSS: {active_pss_delta} KiB")
with open(client_path, encoding="utf-8") as handle:
    client_text = handle.read()
match = re.search(r"actual_rcvbuf_min=(\d+) actual_rcvbuf_max=(\d+)", client_text)
if not match:
    raise SystemExit("missing public client receive-buffer observation")
summary = {
    "flows": flows,
    "idle_pss_kb": int(idle["pss_kb"]),
    "active_pss_kb": int(active["pss_kb"]),
    "active_pss_delta_kb": active_pss_delta,
    "active_pss_delta_kb_per_flow": active_pss_delta / flows,
    "idle_private_dirty_kb": int(idle["private_dirty_kb"]),
    "active_private_dirty_kb": int(active["private_dirty_kb"]),
    "active_private_dirty_delta_kb": int(active["private_dirty_kb"]) - int(idle["private_dirty_kb"]),
    "active_backend_sendq_bytes": int(active["backend_sendq_bytes"]),
    "bridge_backend_read_blocked_events": int(read_blocked),
    "public_client_actual_rcvbuf_min": int(match.group(1)),
    "public_client_actual_rcvbuf_max": int(match.group(2)),
    "drained_pss_kb": int(drained["pss_kb"]),
}
with open(out, "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(summary, sort_keys=True))
PY

cat "$OUT/measurements.tsv"
cat "$OUT/summary.json"
echo "P3 backend-to-public active residency measurement passed"
