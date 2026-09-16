#!/bin/sh
set -eu

CASE=${TCP_SHIFT_LINUX_PACING_CASE:-low-bdp}
MODE=${TCP_SHIFT_LINUX_PACING_MODE:-netem}
RTT_MS=${TCP_SHIFT_LINUX_PACING_RTT_MS:-10}
RATE_MBIT=${TCP_SHIFT_LINUX_PACING_RATE_MBIT:-20}
PAYLOAD_BYTES=${TCP_SHIFT_LINUX_PACING_PAYLOAD_BYTES:-8388608}
OUT=${TCP_SHIFT_LINUX_PACING_OUT:-.build/linux-cubic-pacing/$CASE-$MODE}

NS_CLIENT="tspc$$"
NS_SERVER="tsps$$"
VETH_CLIENT=vethc
VETH_SERVER=veths
IFB_CLIENT=ifb0
CLIENT_IP=10.254.0.1
SERVER_IP=10.254.0.2
PORT=18841
SERVER_PID=

case "$MODE" in
    netem|fq) ;;
    *) echo "MODE must be netem or fq" >&2; exit 1 ;;
esac
case "$RTT_MS" in ''|*[!0-9]*) echo "RTT_MS must be an integer" >&2; exit 1;; esac
case "$RATE_MBIT" in ''|*[!0-9]*) echo "RATE_MBIT must be an integer" >&2; exit 1;; esac
case "$PAYLOAD_BYTES" in ''|*[!0-9]*) echo "PAYLOAD_BYTES must be an integer" >&2; exit 1;; esac
[ "$RTT_MS" -gt 0 ] && [ $((RTT_MS % 2)) -eq 0 ] || {
    echo "RTT_MS must be a positive even integer" >&2
    exit 1
}
[ "$RATE_MBIT" -gt 0 ] || { echo "RATE_MBIT must be positive" >&2; exit 1; }
[ "$PAYLOAD_BYTES" -gt 0 ] || { echo "PAYLOAD_BYTES must be positive" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "Linux pacing probe requires root" >&2; exit 1; }

command -v ip >/dev/null 2>&1 || { echo "ip is required" >&2; exit 1; }
command -v tc >/dev/null 2>&1 || { echo "tc is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 1; }

HALF_RTT_MS=$((RTT_MS / 2))
BDP_BYTES=$((RATE_MBIT * RTT_MS * 125))
QUEUE_PKTS=$(((BDP_BYTES + 1459) / 1460))
[ "$QUEUE_PKTS" -ge 16 ] || QUEUE_PKTS=16

mkdir -p "$OUT"

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

# ACK direction always carries half the propagation delay. For the data
# direction, netem mode reproduces the existing Linux parity reference. fq mode
# keeps sch_fq on the sender and moves delay/rate shaping to a receiver-side IFB
# so Linux can actually mark the socket SK_PACING_FQ and execute sk_pacing_rate.
ip netns exec "$NS_CLIENT" tc qdisc replace dev "$VETH_CLIENT" root netem \
    delay "${HALF_RTT_MS}ms" limit "$QUEUE_PKTS"

if [ "$MODE" = netem ]; then
    ip netns exec "$NS_SERVER" tc qdisc replace dev "$VETH_SERVER" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
else
    modprobe ifb >/dev/null 2>&1 || true
    ip link add "$IFB_CLIENT" type ifb
    ip link set "$IFB_CLIENT" netns "$NS_CLIENT"
    ip -n "$NS_CLIENT" link set "$IFB_CLIENT" up
    ip netns exec "$NS_SERVER" tc qdisc replace dev "$VETH_SERVER" root fq
    ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_CLIENT" handle ffff: ingress
    ip netns exec "$NS_CLIENT" tc filter add dev "$VETH_CLIENT" parent ffff: \
        protocol ip prio 1 u32 match u32 0 0 \
        action mirred egress redirect dev "$IFB_CLIENT"
    ip netns exec "$NS_CLIENT" tc qdisc replace dev "$IFB_CLIENT" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
fi

ip netns exec "$NS_SERVER" tc -s qdisc show dev "$VETH_SERVER" > "$OUT/server-qdisc-before.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$VETH_CLIENT" > "$OUT/client-qdisc-before.txt"
if [ "$MODE" = fq ]; then
    ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$IFB_CLIENT" > "$OUT/ifb-qdisc-before.txt"
fi

ip netns exec "$NS_SERVER" python3 - "$SERVER_IP" "$PORT" "$PAYLOAD_BYTES" "$OUT/tcp-info.tsv" \
    > "$OUT/server.txt" 2> "$OUT/server.stderr" <<'PY' &
import socket
import struct
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
samples_path = sys.argv[4]
TCP_CONGESTION = getattr(socket, "TCP_CONGESTION", 13)
TCP_INFO = getattr(socket, "TCP_INFO", 11)


def tcp_info(sock):
    info = sock.getsockopt(socket.IPPROTO_TCP, TCP_INFO, 232)
    if len(info) < 168:
        raise RuntimeError(f"short TCP_INFO: {len(info)}")
    return {
        "unacked": struct.unpack_from("=I", info, 24)[0],
        "rtt_us": struct.unpack_from("=I", info, 68)[0],
        "snd_ssthresh": struct.unpack_from("=I", info, 76)[0],
        "snd_cwnd": struct.unpack_from("=I", info, 80)[0],
        "total_retrans": struct.unpack_from("=I", info, 100)[0],
        "pacing_rate": struct.unpack_from("=Q", info, 104)[0],
        "max_pacing_rate": struct.unpack_from("=Q", info, 112)[0],
        "notsent_bytes": struct.unpack_from("=I", info, 144)[0],
        "min_rtt_us": struct.unpack_from("=I", info, 148)[0],
        "delivery_rate": struct.unpack_from("=Q", info, 160)[0],
    }

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, b"cubic")
server.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
server.bind((host, port))
server.listen(1)
print("linux-pacing-ready", flush=True)
conn, _ = server.accept()
conn.settimeout(60.0)
cc = conn.getsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, 16).rstrip(b"\0").decode()
chunk = b"x" * 65536
sent = 0
sample_index = 0
start_ns = time.monotonic_ns()
with open(samples_path, "w", encoding="utf-8") as samples:
    samples.write("sample\telapsed_ns\tbytes_written\tunacked\trtt_us\tmin_rtt_us\tsnd_cwnd\tsnd_ssthresh\tpacing_rate_Bps\tmax_pacing_rate_Bps\tdelivery_rate_Bps\tnotsent_bytes\ttotal_retrans\n")
    while sent < length:
        view = memoryview(chunk)[: min(len(chunk), length - sent)]
        written = conn.send(view)
        if written <= 0:
            raise RuntimeError("send made no progress")
        sent += written
        metrics = tcp_info(conn)
        sample_index += 1
        samples.write(
            f"{sample_index}\t{time.monotonic_ns() - start_ns}\t{sent}\t"
            f"{metrics['unacked']}\t{metrics['rtt_us']}\t{metrics['min_rtt_us']}\t"
            f"{metrics['snd_cwnd']}\t{metrics['snd_ssthresh']}\t"
            f"{metrics['pacing_rate']}\t{metrics['max_pacing_rate']}\t"
            f"{metrics['delivery_rate']}\t{metrics['notsent_bytes']}\t"
            f"{metrics['total_retrans']}\n"
        )
        samples.flush()
conn.shutdown(socket.SHUT_WR)
final = tcp_info(conn)
conn.close()
server.close()
print(
    f"linux-pacing-server cc={cc} bytes={sent} samples={sample_index} "
    f"final_pacing_rate_Bps={final['pacing_rate']} final_delivery_rate_Bps={final['delivery_rate']} "
    f"final_rtt_us={final['rtt_us']} final_cwnd={final['snd_cwnd']} "
    f"total_retrans={final['total_retrans']}",
    flush=True,
)
PY
SERVER_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'linux-pacing-ready' "$OUT/server.txt" >/dev/null 2>&1; do
    kill -0 "$SERVER_PID" 2>/dev/null || { cat "$OUT/server.stderr" >&2 || true; exit 1; }
    i=$((i + 1)); sleep 0.05
done
grep -F 'linux-pacing-ready' "$OUT/server.txt" >/dev/null || {
    echo "Linux pacing server ready timeout" >&2
    exit 1
}

ip netns exec "$NS_CLIENT" python3 - "$SERVER_IP" "$PORT" "$PAYLOAD_BYTES" > "$OUT/client.txt" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
expected = int(sys.argv[3])
received = 0
start = time.monotonic_ns()
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(60.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
    sock.connect((host, port))
    while True:
        data = sock.recv(65536)
        if not data:
            break
        received += len(data)
elapsed = time.monotonic_ns() - start
if received != expected:
    raise SystemExit(f"received={received} expected={expected}")
print(f"bytes={received} elapsed_ns={elapsed} goodput_mbps={received * 8.0 * 1000.0 / elapsed:.6f}")
PY

wait "$SERVER_PID" || { SERVER_PID=; cat "$OUT/server.stderr" >&2 || true; exit 1; }
SERVER_PID=

ip netns exec "$NS_SERVER" tc -s qdisc show dev "$VETH_SERVER" > "$OUT/server-qdisc-after.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$VETH_CLIENT" > "$OUT/client-qdisc-after.txt"
if [ "$MODE" = fq ]; then
    ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$IFB_CLIENT" > "$OUT/ifb-qdisc-after.txt"
fi

python3 - "$CASE" "$MODE" "$RTT_MS" "$RATE_MBIT" "$BDP_BYTES" "$QUEUE_PKTS" \
    "$OUT/client.txt" "$OUT/tcp-info.tsv" <<'PY' | tee "$OUT/summary.txt"
import statistics
import sys
from pathlib import Path

case, mode, rtt_ms, rate_mbit, bdp, queue_pkts, client_path, samples_path = sys.argv[1:]
client = Path(client_path).read_text(encoding="utf-8").strip()
goodput = float(client.split("goodput_mbps=")[1].split()[0])
rows = []
lines = Path(samples_path).read_text(encoding="utf-8").splitlines()
header = lines[0].split("\t")
for line in lines[1:]:
    values = line.split("\t")
    rows.append(dict(zip(header, values)))
if not rows:
    raise SystemExit("no TCP_INFO samples")
pacing = [int(row["pacing_rate_Bps"]) for row in rows if int(row["pacing_rate_Bps"]) > 0]
delivery = [int(row["delivery_rate_Bps"]) for row in rows if int(row["delivery_rate_Bps"]) > 0]
rtts = [int(row["rtt_us"]) for row in rows if int(row["rtt_us"]) > 0]
retrans = max(int(row["total_retrans"]) for row in rows)
print(
    f"linux_cubic_pacing_reference=ok case={case} mode={mode} base_rtt_ms={rtt_ms} "
    f"rate_mbit={rate_mbit} bdp_bytes={bdp} queue_pkts={queue_pkts} "
    f"goodput_mbps={goodput:.6f} samples={len(rows)} "
    f"pacing_rate_min_Bps={min(pacing)} pacing_rate_median_Bps={int(statistics.median(pacing))} "
    f"pacing_rate_max_Bps={max(pacing)} pacing_rate_final_Bps={pacing[-1]} "
    f"delivery_rate_median_Bps={int(statistics.median(delivery)) if delivery else 0} "
    f"rtt_median_us={int(statistics.median(rtts)) if rtts else 0} total_retrans={retrans}"
)
PY

uname -a > "$OUT/kernel.txt"
printf 'case=%s\nmode=%s\nbase_rtt_ms=%s\nrate_mbit=%s\npayload_bytes=%s\nbdp_bytes=%s\nqueue_pkts=%s\n' \
    "$CASE" "$MODE" "$RTT_MS" "$RATE_MBIT" "$PAYLOAD_BYTES" "$BDP_BYTES" "$QUEUE_PKTS" \
    > "$OUT/path.env"

echo "Linux CUBIC pacing reference completed: case=$CASE mode=$MODE"
