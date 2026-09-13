#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p3-memory-baseline"
BINARY=${TCP_SHIFT_P3_BINARY:-"$BUILD/tcp-shift-p2"}
RUNTIME_NS=${TCP_SHIFT_P3_RUNTIME_NS:-tsp3memr}
CLIENT_NS=${TCP_SHIFT_P3_CLIENT_NS:-tsp3memc}
TUN_NAME=${TCP_SHIFT_P3_TUN_NAME:-tsp3mem0}
LWIP_IP=${TCP_SHIFT_P3_LWIP_IP:-10.241.0.2}
NETMASK=${TCP_SHIFT_P3_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P3_HOST_IP:-10.241.0.1}
PUBLIC_PORT=${TCP_SHIFT_P3_PUBLIC_PORT:-18100}
BACKEND_PORT=${TCP_SHIFT_P3_BACKEND_PORT:-19100}
RUNTIME_VETH=${TCP_SHIFT_P3_RUNTIME_VETH:-tsp3r0}
CLIENT_VETH=${TCP_SHIFT_P3_CLIENT_VETH:-tsp3c0}
RUNTIME_LINK_IP=${TCP_SHIFT_P3_RUNTIME_LINK_IP:-192.0.2.1/30}
CLIENT_LINK_IP=${TCP_SHIFT_P3_CLIENT_LINK_IP:-192.0.2.2/30}
RUNTIME_GATEWAY=${TCP_SHIFT_P3_RUNTIME_GATEWAY:-192.0.2.1}
STAGES=${TCP_SHIFT_P3_STAGES:-"8 32 64 128"}
MAX_FLOWS=0
PID=
BACKEND_PID=
CLIENT_PIDS=
STOP_FILES=

mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/*.tsv "$OUT"/*.json 2>/dev/null || true
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/measurements.tsv"

if [ "$(id -u)" -ne 0 ]; then
    echo "P3 memory baseline must run as root for isolated network namespaces" >&2
    exit 1
fi
[ -x "$BINARY" ] || {
    echo "missing P3 runtime binary: $BINARY" >&2
    exit 1
}

prev=0
for stage in $STAGES; do
    case "$stage" in
        *[!0-9]*|'')
            echo "invalid P3 stage: $stage" >&2
            exit 1
            ;;
    esac
    [ "$stage" -gt "$prev" ] || {
        echo "P3 stages must be strictly increasing: $STAGES" >&2
        exit 1
    }
    prev=$stage
    MAX_FLOWS=$stage
done
[ "$MAX_FLOWS" -gt 0 ] || {
    echo "P3 requires at least one connection-count stage" >&2
    exit 1
}

cleanup()
{
    set +e
    for stop in $STOP_FILES; do
        : > "$stop" 2>/dev/null || true
    done
    for client_pid in $CLIENT_PIDS; do
        if kill -0 "$client_pid" 2>/dev/null; then
            kill -TERM "$client_pid" >/dev/null 2>&1 || true
            wait "$client_pid" >/dev/null 2>&1 || true
        fi
    done
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" >/dev/null 2>&1 || true
        wait "$PID" >/dev/null 2>&1 || true
    fi
    if [ -n "${BACKEND_PID:-}" ] && kill -0 "$BACKEND_PID" 2>/dev/null; then
        kill -TERM "$BACKEND_PID" >/dev/null 2>&1 || true
        wait "$BACKEND_PID" >/dev/null 2>&1 || true
    fi
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
    flows=$2
    status="/proc/$PID/status"
    smaps="/proc/$PID/smaps_rollup"

    [ -r "$status" ] && [ -r "$smaps" ] || {
        echo "runtime process disappeared before $label sample" >&2
        return 1
    }

    vmrss=$(awk '/^VmRSS:/ {print $2; exit}' "$status")
    rss=$(awk '/^Rss:/ {print $2; exit}' "$smaps")
    pss=$(awk '/^Pss:/ {print $2; exit}' "$smaps")
    private_clean=$(awk '/^Private_Clean:/ {print $2; exit}' "$smaps")
    private_dirty=$(awk '/^Private_Dirty:/ {print $2; exit}' "$smaps")
    anonymous=$(awk '/^Anonymous:/ {print $2; exit}' "$smaps")
    fd_count=$(find "/proc/$PID/fd" -mindepth 1 -maxdepth 1 -printf x 2>/dev/null | wc -c | tr -d ' ')
    cpu_ticks=$(awk '{print $14 + $15}' "/proc/$PID/stat")
    established=$(backend_established_count)
    sockstat=$(ip netns exec "$RUNTIME_NS" cat /proc/net/sockstat)
    tcp_inuse=$(printf '%s\n' "$sockstat" | awk '/^TCP:/ {for (i=1;i<=NF;i++) if ($i=="inuse") {print $(i+1); exit}}')
    tcp_tw=$(printf '%s\n' "$sockstat" | awk '/^TCP:/ {for (i=1;i<=NF;i++) if ($i=="tw") {print $(i+1); exit}}')
    tcp_alloc=$(printf '%s\n' "$sockstat" | awk '/^TCP:/ {for (i=1;i<=NF;i++) if ($i=="alloc") {print $(i+1); exit}}')
    tcp_mem_pages=$(printf '%s\n' "$sockstat" | awk '/^TCP:/ {for (i=1;i<=NF;i++) if ($i=="mem") {print $(i+1); exit}}')

    for value in "$vmrss" "$rss" "$pss" "$private_clean" "$private_dirty" "$anonymous" \
                 "$fd_count" "$cpu_ticks" "$established" "$tcp_inuse" "$tcp_tw" "$tcp_alloc" "$tcp_mem_pages"; do
        [ -n "$value" ] || {
            echo "missing measurement in stage $label" >&2
            return 1
        }
    done

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$flows" "$vmrss" "$rss" "$pss" "$private_clean" "$private_dirty" \
        "$anonymous" "$fd_count" "$cpu_ticks" "$established" "$tcp_inuse" "$tcp_tw" \
        "$tcp_alloc" "$tcp_mem_pages" >> "$OUT/measurements.tsv"
}

printf 'stage\tflows\tvmrss_kb\trss_kb\tpss_kb\tprivate_clean_kb\tprivate_dirty_kb\tanonymous_kb\tfd_count\tcpu_ticks\tbackend_established\ttcp_inuse\ttcp_tw\ttcp_alloc\ttcp_mem_pages\n' \
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

ip netns exec "$RUNTIME_NS" python3 - "$BACKEND_PORT" "$MAX_FLOWS" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import socket
import sys
import threading

port = int(sys.argv[1])
max_flows = int(sys.argv[2])
threads = []
errors = []


def serve(conn, slot):
    try:
        conn.settimeout(30.0)
        while True:
            data = conn.recv(8192)
            if not data:
                break
        try:
            conn.shutdown(socket.SHUT_WR)
        except OSError:
            pass
    except Exception as exc:
        errors.append((slot, repr(exc)))
    finally:
        conn.close()


server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(max_flows)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
for slot in range(1, max_flows + 1):
    conn, _ = server.accept()
    thread = threading.Thread(target=serve, args=(conn, slot), daemon=False)
    thread.start()
    threads.append(thread)
    print(f"backend-accepted={slot}", flush=True)
server.close()
for thread in threads:
    thread.join()
if errors:
    raise SystemExit(f"backend errors: {errors}")
print(f"backend-drained={max_flows}", flush=True)
PY
BACKEND_PID=$!
wait_for_line "$OUT/backend.stdout" 'backend-ready ' 'P3 backend readiness' || {
    cat "$OUT/backend.stderr" >&2 || true
    exit 1
}

ip netns exec "$RUNTIME_NS" "$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

wait_for_line "$OUT/runtime.stdout" "tcp-shift-p2: ready tun=$TUN_NAME" 'P3 runtime readiness' || {
    cat "$OUT/runtime.stderr" >&2 || true
    exit 1
}
[ -r "/proc/$PID/smaps_rollup" ] || {
    echo "runtime PID $PID does not expose smaps_rollup" >&2
    exit 1
}

sample_process ready 0

previous=0
batch_index=0
for stage in $STAGES; do
    delta=$((stage - previous))
    batch_index=$((batch_index + 1))
    stop_file="$OUT/client-stop-$batch_index"
    client_out="$OUT/client-$stage.stdout"
    client_err="$OUT/client-$stage.stderr"
    rm -f "$stop_file"

    ip netns exec "$CLIENT_NS" python3 - "$LWIP_IP" "$PUBLIC_PORT" "$delta" "$stop_file" "$stage" \
        > "$client_out" 2> "$client_err" <<'PY' &
import os
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
count = int(sys.argv[3])
stop_file = sys.argv[4]
stage = int(sys.argv[5])
sockets = []
try:
    for _ in range(count):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((host, port))
        sockets.append(sock)
    print(f"client-batch-ready stage={stage} count={count}", flush=True)
    while not os.path.exists(stop_file):
        time.sleep(0.02)
    for sock in sockets:
        try:
            sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
    for sock in sockets:
        while True:
            data = sock.recv(8192)
            if not data:
                break
    print(f"client-batch-drained stage={stage} count={count}", flush=True)
finally:
    for sock in sockets:
        sock.close()
PY
    client_pid=$!
    CLIENT_PIDS="$CLIENT_PIDS $client_pid"
    STOP_FILES="$STOP_FILES $stop_file"

    wait_for_line "$client_out" "client-batch-ready stage=$stage count=$delta" "P3 client stage $stage" || {
        cat "$client_err" >&2 || true
        exit 1
    }
    wait_for_line "$OUT/backend.stdout" "backend-accepted=$stage" "P3 backend stage $stage" || {
        cat "$OUT/backend.stderr" >&2 || true
        exit 1
    }
    wait_for_backend_count "$stage"
    sample_process "idle-$stage" "$stage"
    previous=$stage
done

for stop in $STOP_FILES; do
    : > "$stop"
done
for client_pid in $CLIENT_PIDS; do
    if ! wait "$client_pid"; then
        echo "P3 client batch $client_pid failed" >&2
        exit 1
    fi
done
CLIENT_PIDS=

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    echo "P3 backend failed during drain" >&2
    exit 1
fi
BACKEND_PID=
wait_for_backend_count 0
sleep 0.2
sample_process drained 0

kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P3 runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

for stage in $STAGES; do
    cat "$OUT/client-$stage.stdout"
done

TOTAL_FLOWS=$MAX_FLOWS
grep -F "backend-drained=$TOTAL_FLOWS" "$OUT/backend.stdout" >/dev/null
grep -F "bridge_accepts=$TOTAL_FLOWS bridge_backend_connects=$TOTAL_FLOWS" "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_active_flows=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_backend_failures=0 bridge_public_errors=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'shutdown_bridge_active_flows=0 shutdown_bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

python3 - "$OUT/measurements.tsv" "$OUT/summary.json" <<'PY'
import csv
import json
import sys

path = sys.argv[1]
out = sys.argv[2]
with open(path, newline="", encoding="utf-8") as handle:
    rows = list(csv.DictReader(handle, delimiter="\t"))
if len(rows) < 3:
    raise SystemExit("P3 memory baseline produced too few samples")
ready = rows[0]
drained = rows[-1]
idle = [row for row in rows if row["stage"].startswith("idle-")]
if not idle:
    raise SystemExit("P3 memory baseline produced no idle established samples")
for row in idle:
    if int(row["backend_established"]) != int(row["flows"]):
        raise SystemExit(f"backend established mismatch at {row['stage']}: {row}")
last = idle[-1]
flows = int(last["flows"])
summary = {
    "ready": {key: int(ready[key]) for key in ("vmrss_kb", "rss_kb", "pss_kb", "private_clean_kb", "private_dirty_kb", "anonymous_kb", "fd_count", "cpu_ticks")},
    "max_idle": {key: int(last[key]) for key in ("flows", "vmrss_kb", "rss_kb", "pss_kb", "private_clean_kb", "private_dirty_kb", "anonymous_kb", "fd_count", "cpu_ticks", "backend_established", "tcp_inuse", "tcp_tw", "tcp_alloc", "tcp_mem_pages")},
    "drained": {key: int(drained[key]) for key in ("vmrss_kb", "rss_kb", "pss_kb", "private_clean_kb", "private_dirty_kb", "anonymous_kb", "fd_count", "cpu_ticks", "tcp_inuse", "tcp_tw", "tcp_alloc", "tcp_mem_pages")},
}
for metric in ("vmrss_kb", "rss_kb", "pss_kb", "private_dirty_kb", "anonymous_kb"):
    delta = int(last[metric]) - int(ready[metric])
    summary[f"max_idle_{metric}_delta"] = delta
    summary[f"max_idle_{metric}_per_flow"] = delta / flows
summary["post_drain_pss_kb_delta"] = int(drained["pss_kb"]) - int(ready["pss_kb"])
summary["post_drain_private_dirty_kb_delta"] = int(drained["private_dirty_kb"]) - int(ready["private_dirty_kb"])
with open(out, "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(summary, sort_keys=True))
PY

cat "$OUT/measurements.tsv"
cat "$OUT/summary.json"
echo "P3 staged idle-connection memory baseline passed"
