#!/bin/sh
set -eu

FLOWS=${TCP_SHIFT_LINUX_BBR_MULTI_FLOWS:-4}
RTT_MS=${TCP_SHIFT_LINUX_BBR_MULTI_RTT_MS:-40}
RATE_MBIT=${TCP_SHIFT_LINUX_BBR_MULTI_RATE_MBIT:-10}
PAYLOAD_BYTES=${TCP_SHIFT_LINUX_BBR_MULTI_PAYLOAD_BYTES:-2097152}
OUT=${TCP_SHIFT_LINUX_BBR_MULTI_OUT:-.build/linux-bbr-multiflow}

NS_CLIENT="tsbmc$$"
NS_SERVER="tsbms$$"
VETH_CLIENT=vethc
VETH_SERVER=veths
IFB_CLIENT=ifb0
CLIENT_IP=10.248.0.1
SERVER_IP=10.248.0.2
PORT=18842
SERVER_PID=

mkdir -p "$OUT"
: > "$OUT/server.txt"
: > "$OUT/server.stderr"
: > "$OUT/client.txt"
: > "$OUT/client.stderr"

[ "$(id -u)" -eq 0 ] || {
    echo "Linux BBR multi-flow reference requires root" >&2
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

modprobe tcp_bbr >/dev/null 2>&1 || true
available_cc=$(sysctl -n net.ipv4.tcp_available_congestion_control 2>/dev/null || true)
printf '%s\n' "$available_cc" | grep -w bbr >/dev/null 2>&1 || {
    printf 'available congestion controls: %s\n' "$available_cc" >&2
    echo "Linux tcp_bbr is unavailable on this kernel" >&2
    exit 1
}

HALF_RTT_MS=$((RTT_MS / 2))
BDP_BYTES=$((RATE_MBIT * RTT_MS * 125))
BDP_PKTS=$(((BDP_BYTES + 1459) / 1460))
QUEUE_PKTS=${TCP_SHIFT_LINUX_BBR_MULTI_QUEUE_PKTS:-$((BDP_PKTS * 8))}
[ "$QUEUE_PKTS" -ge 64 ] || QUEUE_PKTS=64
WIRE_BYTES_PER_FLOW=$((PAYLOAD_BYTES + 12))
TOTAL_WIRE_BYTES=$((WIRE_BYTES_PER_FLOW * FLOWS))

cleanup()
{
    set +e
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    ip netns del "$NS_CLIENT" >/dev/null 2>&1 || true
    ip netns del "$NS_SERVER" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

ip netns add "$NS_CLIENT"
ip netns add "$NS_SERVER"
ip link add "$VETH_CLIENT" type veth peer name "$VETH_SERVER"
ip link set "$VETH_CLIENT" netns "$NS_CLIENT"
ip link set "$VETH_SERVER" netns "$NS_SERVER"
ip -n "$NS_CLIENT" addr add "$CLIENT_IP/30" dev "$VETH_CLIENT"
ip -n "$NS_SERVER" addr add "$SERVER_IP/30" dev "$VETH_SERVER"
ip -n "$NS_CLIENT" link set lo up
ip -n "$NS_SERVER" link set lo up
ip -n "$NS_CLIENT" link set "$VETH_CLIENT" up
ip -n "$NS_SERVER" link set "$VETH_SERVER" up

if command -v ethtool >/dev/null 2>&1; then
    ip netns exec "$NS_CLIENT" ethtool -K "$VETH_CLIENT" tso off gso off gro off >/dev/null 2>&1 || true
    ip netns exec "$NS_SERVER" ethtool -K "$VETH_SERVER" tso off gso off gro off >/dev/null 2>&1 || true
fi

modprobe ifb >/dev/null 2>&1 || true
ip link add "$IFB_CLIENT" type ifb
ip link set "$IFB_CLIENT" netns "$NS_CLIENT"
ip -n "$NS_CLIENT" link set "$IFB_CLIENT" up

# ACK direction: half propagation delay. Data direction: keep sch_fq on the
# BBR sender, then redirect receiver ingress to IFB for half-delay + bottleneck
# rate shaping. This preserves Linux BBR's paced sender semantics.
ip netns exec "$NS_CLIENT" tc qdisc replace dev "$VETH_CLIENT" root netem \
    delay "${HALF_RTT_MS}ms" limit "$QUEUE_PKTS"
ip netns exec "$NS_SERVER" tc qdisc replace dev "$VETH_SERVER" root fq
ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_CLIENT" handle ffff: ingress
ip netns exec "$NS_CLIENT" tc filter add dev "$VETH_CLIENT" parent ffff: \
    protocol ip prio 1 u32 match u32 0 0 \
    action mirred egress redirect dev "$IFB_CLIENT"
ip netns exec "$NS_CLIENT" tc qdisc replace dev "$IFB_CLIENT" root netem \
    delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"

ip netns exec "$NS_SERVER" tc -s qdisc show dev "$VETH_SERVER" > "$OUT/server-qdisc-before.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$VETH_CLIENT" > "$OUT/client-qdisc-before.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$IFB_CLIENT" > "$OUT/ifb-qdisc-before.txt"

ip netns exec "$NS_SERVER" python3 - "$SERVER_IP" "$PORT" "$FLOWS" "$PAYLOAD_BYTES" \
    > "$OUT/server.txt" 2> "$OUT/server.stderr" <<'PY' &
import hashlib
import socket
import struct
import sys
import threading

host = sys.argv[1]
port = int(sys.argv[2])
flows = int(sys.argv[3])
payload_bytes = int(sys.argv[4])
TCP_CONGESTION = getattr(socket, "TCP_CONGESTION", 13)
TCP_INFO = getattr(socket, "TCP_INFO", 11)

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, b"bbr")
server.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
server.bind((host, port))
server.listen(flows)
print(f"linux-bbr-multiflow-ready flows={flows}", flush=True)

connections = []
for _ in range(flows):
    conn, _ = server.accept()
    conn.settimeout(90.0)
    connections.append(conn)

barrier = threading.Barrier(flows)
lock = threading.Lock()
results = []


def tcp_info(sock):
    info = sock.getsockopt(socket.IPPROTO_TCP, TCP_INFO, 232)
    return {
        "snd_cwnd": struct.unpack_from("=I", info, 80)[0],
        "total_retrans": struct.unpack_from("=I", info, 100)[0],
        "pacing_rate": struct.unpack_from("=Q", info, 104)[0],
        "delivery_rate": struct.unpack_from("=Q", info, 160)[0],
    }


def worker(index, conn):
    cc = conn.getsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, 16).rstrip(b"\0").decode()
    if cc != "bbr":
        raise RuntimeError(f"flow={index} unexpected cc={cc}")
    payload = bytes((((offset * 73) + 19 + index * 17) & 0xFF)
                    for offset in range(payload_bytes))
    digest = hashlib.sha256(payload).hexdigest()
    header = struct.pack("!IQ", index, payload_bytes)
    barrier.wait()
    conn.sendall(header)
    conn.sendall(payload)
    conn.shutdown(socket.SHUT_WR)
    while conn.recv(4096):
        pass
    final = tcp_info(conn)
    conn.close()
    with lock:
        results.append((index, digest, final))


threads = [
    threading.Thread(target=worker, args=(index, conn))
    for index, conn in enumerate(connections)
]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
server.close()

if len(results) != flows:
    raise SystemExit(f"server completed={len(results)} expected={flows}")
for index, digest, final in sorted(results):
    print(
        f"linux-server-flow={index} sha256={digest} "
        f"snd_cwnd_packets={final['snd_cwnd']} "
        f"pacing_rate_Bps={final['pacing_rate']} "
        f"delivery_rate_Bps={final['delivery_rate']} "
        f"total_retrans={final['total_retrans']}",
        flush=True,
    )
PY
SERVER_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F "linux-bbr-multiflow-ready flows=$FLOWS" "$OUT/server.txt" >/dev/null 2>&1; do
    kill -0 "$SERVER_PID" 2>/dev/null || {
        cat "$OUT/server.stderr" >&2 || true
        echo "Linux BBR multi-flow server exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
grep -F "linux-bbr-multiflow-ready flows=$FLOWS" "$OUT/server.txt" >/dev/null || {
    echo "timed out waiting for Linux BBR multi-flow server" >&2
    exit 1
}

ip netns exec "$NS_CLIENT" python3 - "$SERVER_IP" "$PORT" "$FLOWS" "$PAYLOAD_BYTES" \
    > "$OUT/client.txt" 2> "$OUT/client.stderr" <<'PY'
import hashlib
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
        if sock.recv(1):
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
        results.append((flow_id, elapsed_ns, goodput_mbps, digest))


threads = [threading.Thread(target=worker, args=(index,)) for index in range(flows)]
start_all_ns = time.monotonic_ns()
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
elapsed_all_ns = time.monotonic_ns() - start_all_ns

if len(results) != flows:
    raise SystemExit(f"client completed={len(results)} expected={flows}")
flow_ids = sorted(result[0] for result in results)
if flow_ids != list(range(flows)):
    raise SystemExit(f"unexpected flow ids: {flow_ids}")

throughputs = [result[2] for result in results]
sum_x = sum(throughputs)
sum_x2 = sum(value * value for value in throughputs)
jain = (sum_x * sum_x) / (flows * sum_x2) if sum_x2 else 0.0
aggregate_mbps = flows * (expected_payload + 12) * 8.0 * 1000.0 / elapsed_all_ns

for flow_id, elapsed_ns, goodput_mbps, digest in sorted(results):
    print(
        f"linux-client-flow={flow_id} elapsed_ns={elapsed_ns} "
        f"goodput_mbps={goodput_mbps:.6f} sha256={digest}"
    )
print(
    f"linux-client-complete flows={flows} "
    f"total_wire_bytes={flows * (expected_payload + 12)} "
    f"elapsed_all_ns={elapsed_all_ns} aggregate_goodput_mbps={aggregate_mbps:.6f} "
    f"min_flow_goodput_mbps={min(throughputs):.6f} "
    f"max_flow_goodput_mbps={max(throughputs):.6f} jain_fairness={jain:.6f}"
)
PY

if ! wait "$SERVER_PID"; then
    SERVER_PID=
    cat "$OUT/server.stderr" >&2 || true
    cat "$OUT/client.stderr" >&2 || true
    echo "Linux BBR multi-flow server failed" >&2
    exit 1
fi
SERVER_PID=

ip netns exec "$NS_SERVER" tc -s qdisc show dev "$VETH_SERVER" > "$OUT/server-qdisc-after.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$VETH_CLIENT" > "$OUT/client-qdisc-after.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$IFB_CLIENT" > "$OUT/ifb-qdisc-after.txt"

data_drops=$(sed -n 's/.*(dropped \([0-9][0-9]*\),.*/\1/p' "$OUT/ifb-qdisc-after.txt" | head -n 1)
ack_drops=$(sed -n 's/.*(dropped \([0-9][0-9]*\),.*/\1/p' "$OUT/client-qdisc-after.txt" | head -n 1)
[ -n "$data_drops" ] && [ "$data_drops" -eq 0 ] &&
[ -n "$ack_drops" ] && [ "$ack_drops" -eq 0 ] || {
    cat "$OUT/ifb-qdisc-after.txt" >&2 || true
    cat "$OUT/client-qdisc-after.txt" >&2 || true
    echo "Linux BBR multi-flow qdisc dropped packets: data=${data_drops:-missing} ack=${ack_drops:-missing}" >&2
    exit 1
}

aggregate=$(sed -n 's/.* aggregate_goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.txt" | tail -n 1)
min_flow=$(sed -n 's/.* min_flow_goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.txt" | tail -n 1)
max_flow=$(sed -n 's/.* max_flow_goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.txt" | tail -n 1)
jain=$(sed -n 's/.* jain_fairness=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.txt" | tail -n 1)
[ -n "$aggregate" ] && [ -n "$min_flow" ] && [ -n "$max_flow" ] && [ -n "$jain" ] || {
    echo "missing Linux BBR multi-flow client metrics" >&2
    exit 1
}

total_retrans=$(sed -n 's/.* total_retrans=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | awk '{sum += $1} END {print sum + 0}')
median_cwnd=$(sed -n 's/.* snd_cwnd_packets=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | sort -n | awk '
    {a[NR]=$1}
    END {
        if (NR == 0) { print 0; exit }
        if (NR % 2) print a[(NR+1)/2]
        else print int((a[NR/2] + a[NR/2+1]) / 2)
    }')
median_pacing=$(sed -n 's/.* pacing_rate_Bps=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | sort -n | awk '
    {a[NR]=$1}
    END {
        if (NR == 0) { print 0; exit }
        if (NR % 2) print a[(NR+1)/2]
        else print int((a[NR/2] + a[NR/2+1]) / 2)
    }')
median_delivery=$(sed -n 's/.* delivery_rate_Bps=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | sort -n | awk '
    {a[NR]=$1}
    END {
        if (NR == 0) { print 0; exit }
        if (NR % 2) print a[(NR+1)/2]
        else print int((a[NR/2] + a[NR/2+1]) / 2)
    }')

printf 'linux_bbr_multiflow_reference=ok flows=%s base_rtt_ms=%s rate_mbit=%s bdp_bytes=%s queue_pkts=%s payload_bytes_per_flow=%s total_wire_bytes=%s aggregate_goodput_mbps=%s min_flow_goodput_mbps=%s max_flow_goodput_mbps=%s jain_fairness=%s median_final_cwnd_packets=%s median_final_pacing_rate_Bps=%s median_final_delivery_rate_Bps=%s qdisc_drops=%s/%s total_retrans=%s payload_integrity=ok\n' \
    "$FLOWS" "$RTT_MS" "$RATE_MBIT" "$BDP_BYTES" "$QUEUE_PKTS" \
    "$PAYLOAD_BYTES" "$TOTAL_WIRE_BYTES" "$aggregate" "$min_flow" "$max_flow" "$jain" \
    "$median_cwnd" "$median_pacing" "$median_delivery" "$data_drops" "$ack_drops" \
    "$total_retrans" | tee "$OUT/summary.txt"

echo "Linux BBR multi-flow reference completed"
