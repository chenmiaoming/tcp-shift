#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p3-repeated-drain"
BINARY=${TCP_SHIFT_P3_BINARY:-"$BUILD/tcp-shift-p2"}
RUNTIME_NS=${TCP_SHIFT_P3_REPEAT_RUNTIME_NS:-tsp3repr}
CLIENT_NS=${TCP_SHIFT_P3_REPEAT_CLIENT_NS:-tsp3repc}
TUN_NAME=${TCP_SHIFT_P3_REPEAT_TUN_NAME:-tsp3rep0}
LWIP_IP=${TCP_SHIFT_P3_REPEAT_LWIP_IP:-10.244.0.2}
NETMASK=${TCP_SHIFT_P3_REPEAT_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P3_REPEAT_HOST_IP:-10.244.0.1}
PUBLIC_PORT=${TCP_SHIFT_P3_REPEAT_PUBLIC_PORT:-18103}
BACKEND_PORT=${TCP_SHIFT_P3_REPEAT_BACKEND_PORT:-19103}
RUNTIME_VETH=${TCP_SHIFT_P3_REPEAT_RUNTIME_VETH:-tsp3repr0}
CLIENT_VETH=${TCP_SHIFT_P3_REPEAT_CLIENT_VETH:-tsp3repc0}
RUNTIME_LINK_IP=${TCP_SHIFT_P3_REPEAT_RUNTIME_LINK_IP:-192.0.2.13/30}
CLIENT_LINK_IP=${TCP_SHIFT_P3_REPEAT_CLIENT_LINK_IP:-192.0.2.14/30}
RUNTIME_GATEWAY=${TCP_SHIFT_P3_REPEAT_RUNTIME_GATEWAY:-192.0.2.13}
ROUNDS=${TCP_SHIFT_P3_REPEAT_ROUNDS:-3}
FLOW_COUNT=${TCP_SHIFT_P3_REPEAT_FLOWS:-128}
TOTAL_FLOWS=$((ROUNDS * FLOW_COUNT))
PID=
BACKEND_PID=
CLIENT_PID=

mkdir -p "$OUT"
rm -f "$OUT"/* 2>/dev/null || true
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/measurements.tsv"

if [ "$(id -u)" -ne 0 ]; then
    echo "P3 repeated-drain harness must run as root" >&2
    exit 1
fi
[ -x "$BINARY" ] || {
    echo "missing P3 runtime binary: $BINARY" >&2
    exit 1
}
case "$ROUNDS:$FLOW_COUNT" in
    *[!0-9:]*|0:*|*:0)
        echo "invalid P3 repeated-drain shape: rounds=$ROUNDS flows=$FLOW_COUNT" >&2
        exit 1
        ;;
esac

cleanup()
{
    set +e
    for release in "$OUT"/release-*; do
        [ -e "$release" ] && : > "$release" 2>/dev/null || true
    done
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

sample_process()
{
    label=$1
    flows=$2
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
    fd_count=$(find "/proc/$PID/fd" -mindepth 1 -maxdepth 1 -printf x 2>/dev/null | wc -c | tr -d ' ')
    established=$(backend_established_count)

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$flows" "$vmrss" "$rss" "$pss" "$private_dirty" \
        "$anonymous" "$fd_count" "$established" >> "$OUT/measurements.tsv"
}

printf 'stage\tflows\tvmrss_kb\trss_kb\tpss_kb\tprivate_dirty_kb\tanonymous_kb\tfd_count\tbackend_established\n' \
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

ip netns exec "$RUNTIME_NS" python3 - "$BACKEND_PORT" "$TOTAL_FLOWS" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import socket
import sys
import threading

port = int(sys.argv[1])
total_flows = int(sys.argv[2])
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
server.listen(256)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
for slot in range(1, total_flows + 1):
    conn, _ = server.accept()
    thread = threading.Thread(target=serve, args=(conn, slot), daemon=False)
    thread.start()
    threads.append(thread)
    if slot == total_flows or slot % 128 == 0:
        print(f"backend-accepted={slot}", flush=True)
server.close()
for thread in threads:
    thread.join()
if errors:
    raise SystemExit(f"backend errors: {errors}")
print(f"backend-drained={total_flows}", flush=True)
PY
BACKEND_PID=$!
wait_for_line "$OUT/backend.stdout" 'backend-ready ' 'P3 repeated backend readiness'

ip netns exec "$RUNTIME_NS" "$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!
wait_for_line "$OUT/runtime.stdout" "tcp-shift-p2: ready tun=$TUN_NAME" 'P3 repeated runtime readiness'
sample_process ready 0

round=1
while [ "$round" -le "$ROUNDS" ]; do
    release="$OUT/release-$round"
    client_out="$OUT/client-$round.stdout"
    client_err="$OUT/client-$round.stderr"
    rm -f "$release"

    ip netns exec "$CLIENT_NS" python3 - "$LWIP_IP" "$PUBLIC_PORT" "$FLOW_COUNT" "$release" "$round" \
        > "$client_out" 2> "$client_err" <<'PY' &
import os
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
count = int(sys.argv[3])
release = sys.argv[4]
round_no = int(sys.argv[5])
sockets = []
try:
    for _ in range(count):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((host, port))
        sockets.append(sock)
    print(f"client-round-ready={round_no} count={len(sockets)}", flush=True)
    while not os.path.exists(release):
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
        sock.close()
    print(f"client-round-drained={round_no} count={count}", flush=True)
finally:
    for sock in sockets:
        try:
            sock.close()
        except OSError:
            pass
PY
    CLIENT_PID=$!

    wait_for_line "$client_out" "client-round-ready=$round count=$FLOW_COUNT" "P3 repeated client round $round"
    cumulative=$((round * FLOW_COUNT))
    wait_for_line "$OUT/backend.stdout" "backend-accepted=$cumulative" "P3 repeated backend round $round"
    wait_for_backend_count "$FLOW_COUNT"
    sample_process "round-$round-idle" "$FLOW_COUNT"

    : > "$release"
    if ! wait "$CLIENT_PID"; then
        CLIENT_PID=
        cat "$client_out" >&2 || true
        cat "$client_err" >&2 || true
        exit 1
    fi
    CLIENT_PID=
    wait_for_backend_count 0
    sleep 0.2
    sample_process "round-$round-drained" 0
    round=$((round + 1))
done

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    exit 1
fi
BACKEND_PID=

kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    exit 1
fi
PID=

cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2
round=1
while [ "$round" -le "$ROUNDS" ]; do
    cat "$OUT/client-$round.stdout"
    round=$((round + 1))
done

grep -F "backend-drained=$TOTAL_FLOWS" "$OUT/backend.stdout" >/dev/null
grep -F "bridge_accepts=$TOTAL_FLOWS bridge_backend_connects=$TOTAL_FLOWS" "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_active_flows=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_backend_failures=0 bridge_public_errors=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'shutdown_bridge_active_flows=0 shutdown_bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

python3 - "$OUT/measurements.tsv" "$OUT/summary.json" "$ROUNDS" "$FLOW_COUNT" <<'PY'
import csv
import json
import sys

path, out, rounds_text, flow_text = sys.argv[1:]
rounds = int(rounds_text)
flow_count = int(flow_text)
with open(path, newline="", encoding="utf-8") as handle:
    rows = {row["stage"]: row for row in csv.DictReader(handle, delimiter="\t")}
ready = rows.get("ready")
if ready is None:
    raise SystemExit("missing repeated-drain ready sample")
ready_fd = int(ready["fd_count"])
ready_pss = int(ready["pss_kb"])
summary = {
    "rounds": rounds,
    "flows_per_round": flow_count,
    "ready_pss_kb": ready_pss,
    "ready_private_dirty_kb": int(ready["private_dirty_kb"]),
    "ready_fd_count": ready_fd,
    "rounds_detail": [],
}
drain_pss = []
for round_no in range(1, rounds + 1):
    idle = rows.get(f"round-{round_no}-idle")
    drained = rows.get(f"round-{round_no}-drained")
    if idle is None or drained is None:
        raise SystemExit(f"missing repeated-drain round {round_no} samples")
    if int(idle["backend_established"]) != flow_count:
        raise SystemExit(f"round {round_no} idle backend count mismatch")
    if int(drained["backend_established"]) != 0:
        raise SystemExit(f"round {round_no} did not drain backend sockets")
    if int(drained["fd_count"]) != ready_fd:
        raise SystemExit(
            f"round {round_no} fd floor changed: ready={ready_fd} drained={drained['fd_count']}"
        )
    drain_value = int(drained["pss_kb"])
    drain_pss.append(drain_value)
    summary["rounds_detail"].append(
        {
            "round": round_no,
            "idle_pss_kb": int(idle["pss_kb"]),
            "idle_pss_delta_from_ready_kb": int(idle["pss_kb"]) - ready_pss,
            "drained_pss_kb": drain_value,
            "drained_pss_delta_from_ready_kb": drain_value - ready_pss,
            "drained_private_dirty_kb": int(drained["private_dirty_kb"]),
        }
    )
summary["first_to_last_drain_pss_growth_kb"] = drain_pss[-1] - drain_pss[0]
summary["max_drain_pss_delta_from_ready_kb"] = max(drain_pss) - ready_pss
summary["min_drain_pss_delta_from_ready_kb"] = min(drain_pss) - ready_pss
with open(out, "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2, sort_keys=True)
    handle.write("\n")
print(json.dumps(summary, sort_keys=True))
PY

cat "$OUT/measurements.tsv"
cat "$OUT/summary.json"
echo "P3 repeated load/drain measurement passed"
