#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p3-backend-to-public-stability"
BINARY=${TCP_SHIFT_P3_BINARY:-"$BUILD/tcp-shift-p2"}
RUNTIME_NS=${TCP_SHIFT_P3_STABILITY_RUNTIME_NS:-tsp3stabr}
CLIENT_NS=${TCP_SHIFT_P3_STABILITY_CLIENT_NS:-tsp3stabc}
TUN_NAME=${TCP_SHIFT_P3_STABILITY_TUN_NAME:-tsp3stab0}
LWIP_IP=${TCP_SHIFT_P3_STABILITY_LWIP_IP:-10.244.0.2}
NETMASK=${TCP_SHIFT_P3_STABILITY_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P3_STABILITY_HOST_IP:-10.244.0.1}
PUBLIC_PORT=${TCP_SHIFT_P3_STABILITY_PUBLIC_PORT:-18103}
BACKEND_PORT=${TCP_SHIFT_P3_STABILITY_BACKEND_PORT:-19103}
RUNTIME_VETH=${TCP_SHIFT_P3_STABILITY_RUNTIME_VETH:-tsp3stabr0}
CLIENT_VETH=${TCP_SHIFT_P3_STABILITY_CLIENT_VETH:-tsp3stabc0}
RUNTIME_LINK_IP=${TCP_SHIFT_P3_STABILITY_RUNTIME_LINK_IP:-192.0.2.13/30}
CLIENT_LINK_IP=${TCP_SHIFT_P3_STABILITY_CLIENT_LINK_IP:-192.0.2.14/30}
RUNTIME_GATEWAY=${TCP_SHIFT_P3_STABILITY_RUNTIME_GATEWAY:-192.0.2.13}
FLOW_COUNT=${TCP_SHIFT_P3_STABILITY_FLOWS:-8}
PAYLOAD_BYTES=${TCP_SHIFT_P3_STABILITY_PAYLOAD_BYTES:-2097152}
CLIENT_RCVBUF=${TCP_SHIFT_P3_STABILITY_CLIENT_RCVBUF:-4096}
CYCLES=${TCP_SHIFT_P3_STABILITY_CYCLES:-4}
MAX_POST_WARM_GROWTH_KB=${TCP_SHIFT_P3_STABILITY_MAX_POST_WARM_GROWTH_KB:-1024}
TOTAL_FLOWS=$((FLOW_COUNT * CYCLES))
TOTAL_BYTES=$((FLOW_COUNT * PAYLOAD_BYTES * CYCLES))
PID=
BACKEND_PID=
CLIENT_PID=

mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/*.tsv "$OUT"/*.json "$OUT"/cycle-* 2>/dev/null || true
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/measurements.tsv"

[ "$(id -u)" -eq 0 ] || {
    echo "P3 stability harness must run as root" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing P3 runtime binary: $BINARY" >&2
    exit 1
}
case "$FLOW_COUNT" in ''|*[!0-9]*) echo "FLOW_COUNT must be an integer" >&2; exit 1;; esac
case "$PAYLOAD_BYTES" in ''|*[!0-9]*) echo "PAYLOAD_BYTES must be an integer" >&2; exit 1;; esac
case "$CYCLES" in ''|*[!0-9]*) echo "CYCLES must be an integer" >&2; exit 1;; esac
case "$MAX_POST_WARM_GROWTH_KB" in ''|*[!0-9]*) echo "MAX_POST_WARM_GROWTH_KB must be an integer" >&2; exit 1;; esac
[ "$FLOW_COUNT" -gt 0 ] && [ "$PAYLOAD_BYTES" -gt 0 ] && [ "$CYCLES" -ge 2 ] || {
    echo "stability harness requires positive flows/payload and at least two cycles" >&2
    exit 1
}

cleanup()
{
    set +e
    for child in "$CLIENT_PID" "$BACKEND_PID" "$PID"; do
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
    while [ "$count" -lt 400 ]; do
        if grep -F "$pattern" "$file" >/dev/null 2>&1; then
            return 0
        fi
        count=$((count + 1))
        sleep 0.05
    done
    echo "timed out waiting for $label: $pattern" >&2
    return 1
}

backend_open_count()
{
    ip netns exec "$RUNTIME_NS" \
        ss -Htan "( sport = :$BACKEND_PORT )" 2>/dev/null \
        | awk '$1 != "LISTEN" && $1 != "TIME-WAIT" {count++} END {print count + 0}'
}

backend_sendq_bytes()
{
    ip netns exec "$RUNTIME_NS" \
        ss -Htan "( sport = :$BACKEND_PORT )" 2>/dev/null \
        | awk '$1 != "LISTEN" && $1 != "TIME-WAIT" {sum += $3} END {print sum + 0}'
}

wait_for_backend_count()
{
    wanted=$1
    count=0
    while [ "$count" -lt 400 ]; do
        current=$(backend_open_count)
        if [ "$current" -eq "$wanted" ]; then
            return 0
        fi
        count=$((count + 1))
        sleep 0.05
    done
    echo "backend live socket count did not reach $wanted; got $(backend_open_count)" >&2
    ip netns exec "$RUNTIME_NS" ss -Htan "( sport = :$BACKEND_PORT )" >&2 || true
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
    pss=$(awk '/^Pss:/ {print $2; exit}' "$smaps")
    private_dirty=$(awk '/^Private_Dirty:/ {print $2; exit}' "$smaps")
    anonymous=$(awk '/^Anonymous:/ {print $2; exit}' "$smaps")
    sendq=$(backend_sendq_bytes)
    open_connections=$(backend_open_count)
    fd_count=$(find "/proc/$PID/fd" -mindepth 1 -maxdepth 1 -printf x 2>/dev/null | wc -c | tr -d ' ')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$vmrss" "$pss" "$private_dirty" "$anonymous" \
        "$fd_count" "$open_connections" "$sendq" >> "$OUT/measurements.tsv"
}

printf 'stage\tvmrss_kb\tpss_kb\tprivate_dirty_kb\tanonymous_kb\tfd_count\tbackend_open_connections\tbackend_sendq_bytes\n' \
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

ip netns exec "$RUNTIME_NS" "$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!
wait_for_line "$OUT/runtime.stdout" "tcp-shift-p2: ready tun=$TUN_NAME" 'P3 stability runtime readiness'
sample_process idle

cycle=1
while [ "$cycle" -le "$CYCLES" ]; do
    cycle_dir="$OUT/cycle-$cycle"
    start_file="$cycle_dir/start-backend-send"
    release_file="$cycle_dir/release-client-read"
    mkdir -p "$cycle_dir"
    : > "$cycle_dir/backend.stdout"
    : > "$cycle_dir/backend.stderr"
    : > "$cycle_dir/client.stdout"
    : > "$cycle_dir/client.stderr"

    ip netns exec "$RUNTIME_NS" python3 - "$BACKEND_PORT" "$FLOW_COUNT" \
        "$PAYLOAD_BYTES" "$start_file" \
        > "$cycle_dir/backend.stdout" 2> "$cycle_dir/backend.stderr" <<'PY' &
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
print(f"backend-ready port={port}", flush=True)
for _ in range(count):
    conn, _ = server.accept()
    conn.settimeout(30.0)
    connections.append(conn)
print(f"backend-accepted={len(connections)}", flush=True)
while not os.path.exists(start_file):
    time.sleep(0.02)

def send_one(conn):
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
    wait_for_line "$cycle_dir/backend.stdout" 'backend-ready ' "cycle $cycle backend readiness"

    ip netns exec "$CLIENT_NS" python3 - "$LWIP_IP" "$PUBLIC_PORT" "$FLOW_COUNT" \
        "$PAYLOAD_BYTES" "$CLIENT_RCVBUF" "$release_file" \
        > "$cycle_dir/client.stdout" 2> "$cycle_dir/client.stderr" <<'PY' &
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
for _ in range(count):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, requested_rcvbuf)
    sock.settimeout(30.0)
    sock.connect((host, port))
    sock.shutdown(socket.SHUT_WR)
    sockets.append(sock)
print(f"client-connected={len(sockets)}", flush=True)
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
        raise SystemExit(f"client payload mismatch expected={payload_bytes} got={received}")
    total += received
    sock.close()
print(f"client-drained={count} total_bytes={total}", flush=True)
PY
    CLIENT_PID=$!

    wait_for_line "$cycle_dir/client.stdout" "client-connected=$FLOW_COUNT" "cycle $cycle clients connected"
    wait_for_line "$cycle_dir/backend.stdout" "backend-accepted=$FLOW_COUNT" "cycle $cycle backend accepted"
    wait_for_backend_count "$FLOW_COUNT"
    : > "$start_file"
    wait_for_line "$cycle_dir/backend.stdout" "backend-send-started=$FLOW_COUNT" "cycle $cycle backend send start"
    sleep 0.5
    sample_process "active-$cycle"
    : > "$release_file"

    if ! wait "$CLIENT_PID"; then
        CLIENT_PID=
        cat "$cycle_dir/client.stdout" >&2 || true
        cat "$cycle_dir/client.stderr" >&2 || true
        exit 1
    fi
    CLIENT_PID=
    if ! wait "$BACKEND_PID"; then
        BACKEND_PID=
        cat "$cycle_dir/backend.stdout" >&2 || true
        cat "$cycle_dir/backend.stderr" >&2 || true
        exit 1
    fi
    BACKEND_PID=
    wait_for_backend_count 0
    sleep 0.2
    sample_process "drained-$cycle"

    grep -F "client-drained=$FLOW_COUNT total_bytes=$((FLOW_COUNT * PAYLOAD_BYTES))" "$cycle_dir/client.stdout" >/dev/null
    grep -F "backend-sent=$FLOW_COUNT total_bytes=$((FLOW_COUNT * PAYLOAD_BYTES))" "$cycle_dir/backend.stdout" >/dev/null
    grep -F "backend-drained=$FLOW_COUNT" "$cycle_dir/backend.stdout" >/dev/null
    cycle=$((cycle + 1))
done

kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    exit 1
fi
PID=

cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2
read_blocked=$(grep -Eo 'bridge_backend_read_blocked_events=[0-9]+' "$OUT/runtime.stderr" | tail -1 | cut -d= -f2)
[ -n "$read_blocked" ] && [ "$read_blocked" -gt 0 ] || {
    echo "stability run never reached lwIP send pressure" >&2
    exit 1
}
grep -F "bridge_accepts=$TOTAL_FLOWS bridge_backend_connects=$TOTAL_FLOWS bridge_public_to_backend_bytes=0 bridge_backend_to_public_bytes=$TOTAL_BYTES bridge_active_flows=0" "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_backend_failures=0 bridge_public_errors=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'shutdown_bridge_active_flows=0 shutdown_bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

python3 - "$OUT/measurements.tsv" "$OUT/summary.json" "$FLOW_COUNT" "$CYCLES" \
    "$MAX_POST_WARM_GROWTH_KB" "$read_blocked" <<'PY'
import csv
import json
import sys

path, out, flows_s, cycles_s, limit_s, read_blocked_s = sys.argv[1:]
flows = int(flows_s)
cycles = int(cycles_s)
limit = int(limit_s)
with open(path, newline="", encoding="utf-8") as handle:
    rows = {row["stage"]: row for row in csv.DictReader(handle, delimiter="\t")}
if "idle" not in rows:
    raise SystemExit("missing idle stability sample")
idle_pss = int(rows["idle"]["pss_kb"])
active = []
drained = []
for cycle in range(1, cycles + 1):
    a = rows.get(f"active-{cycle}")
    d = rows.get(f"drained-{cycle}")
    if a is None or d is None:
        raise SystemExit(f"missing stability sample for cycle {cycle}")
    active.append(int(a["pss_kb"]))
    drained.append(int(d["pss_kb"]))
first_delta = active[0] - idle_pss
if first_delta <= 0:
    raise SystemExit(f"first active cycle did not increase PSS: {first_delta} KiB")
post_warm_growth = max(drained[1:]) - drained[0]
post_warm_range = max(drained[1:]) - min(drained[1:]) if len(drained) > 2 else abs(drained[-1] - drained[0])
summary = {
    "flows_per_cycle": flows,
    "cycles": cycles,
    "idle_pss_kb": idle_pss,
    "active_pss_kb_per_cycle": active,
    "drained_pss_kb_per_cycle": drained,
    "first_active_pss_delta_kb": first_delta,
    "first_active_pss_delta_kb_per_flow": first_delta / flows,
    "post_warm_max_growth_kb": post_warm_growth,
    "post_warm_range_kb": post_warm_range,
    "max_post_warm_growth_kb": limit,
    "bridge_backend_read_blocked_events": int(read_blocked_s),
}
with open(out, "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(summary, sort_keys=True))
if post_warm_growth > limit:
    raise SystemExit(
        f"post-warm PSS grew by {post_warm_growth} KiB; limit is {limit} KiB"
    )
PY

cat "$OUT/measurements.tsv"
cat "$OUT/summary.json"
echo "P3 repeated sender-buffer stability qualification passed"