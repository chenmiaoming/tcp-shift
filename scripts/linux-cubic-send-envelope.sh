#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CASE=${TCP_SHIFT_LINUX_ENVELOPE_CASE:-low-bdp}
RTT_MS=${TCP_SHIFT_LINUX_ENVELOPE_RTT_MS:-10}
RATE_MBIT=${TCP_SHIFT_LINUX_ENVELOPE_RATE_MBIT:-20}
PAYLOAD_BYTES=${TCP_SHIFT_LINUX_ENVELOPE_PAYLOAD_BYTES:-8388608}
QUEUE_PKTS=${TCP_SHIFT_LINUX_ENVELOPE_QUEUE_PKTS:-35}
OUT=${TCP_SHIFT_LINUX_ENVELOPE_OUT:-"$ROOT/.build/linux-cubic-envelope/$CASE"}

NS_CLIENT="tsec$$"
NS_SERVER="tses$$"
VETH_CLIENT=vethc
VETH_SERVER=veths
IFB_CLIENT=ifb0
CLIENT_IP=10.255.0.1
SERVER_IP=10.255.0.2
PORT=18842
SERVER_PID=
TCPDUMP_PID=

case "$RTT_MS" in ''|*[!0-9]*) echo "RTT_MS must be an integer" >&2; exit 1;; esac
case "$RATE_MBIT" in ''|*[!0-9]*) echo "RATE_MBIT must be an integer" >&2; exit 1;; esac
case "$PAYLOAD_BYTES" in ''|*[!0-9]*) echo "PAYLOAD_BYTES must be an integer" >&2; exit 1;; esac
case "$QUEUE_PKTS" in ''|*[!0-9]*) echo "QUEUE_PKTS must be an integer" >&2; exit 1;; esac
[ "$RTT_MS" -gt 0 ] && [ $((RTT_MS % 2)) -eq 0 ] || {
    echo "RTT_MS must be a positive even integer" >&2
    exit 1
}
[ "$RATE_MBIT" -gt 0 ] || { echo "RATE_MBIT must be positive" >&2; exit 1; }
[ "$PAYLOAD_BYTES" -gt 0 ] || { echo "PAYLOAD_BYTES must be positive" >&2; exit 1; }
[ "$QUEUE_PKTS" -gt 0 ] || { echo "QUEUE_PKTS must be positive" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "Linux envelope probe requires root" >&2; exit 1; }

for tool in ip tc python3 tcpdump; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$tool is required" >&2; exit 1; }
done

HALF_RTT_MS=$((RTT_MS / 2))
BDP_BYTES=$((RATE_MBIT * RTT_MS * 125))
mkdir -p "$OUT"

stop_pid()
{
    pid=$1
    signal=${2:-TERM}
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -"$signal" "$pid" >/dev/null 2>&1 || true
        wait "$pid" >/dev/null 2>&1 || true
    fi
}

cleanup()
{
    set +e
    stop_pid "${TCPDUMP_PID:-}" INT
    stop_pid "${SERVER_PID:-}" TERM
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

# Execute Linux TCP pacing at sender-side sch_fq. The receiver-side veth ingress
# is captured before ingress redirect to IFB, so packet timestamps reflect fq's
# actual release envelope rather than the downstream netem rate shaper.
ip netns exec "$NS_SERVER" tc qdisc replace dev "$VETH_SERVER" root fq
ip netns exec "$NS_CLIENT" tc qdisc replace dev "$VETH_CLIENT" root netem \
    delay "${HALF_RTT_MS}ms" limit "$QUEUE_PKTS"
modprobe ifb >/dev/null 2>&1 || true
ip link add "$IFB_CLIENT" type ifb
ip link set "$IFB_CLIENT" netns "$NS_CLIENT"
ip -n "$NS_CLIENT" link set "$IFB_CLIENT" up
ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_CLIENT" handle ffff: ingress
ip netns exec "$NS_CLIENT" tc filter add dev "$VETH_CLIENT" parent ffff: \
    protocol ip prio 1 u32 match u32 0 0 \
    action mirred egress redirect dev "$IFB_CLIENT"
ip netns exec "$NS_CLIENT" tc qdisc replace dev "$IFB_CLIENT" root netem \
    delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"

ip netns exec "$NS_SERVER" tc -s qdisc show dev "$VETH_SERVER" > "$OUT/server-qdisc-before.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$IFB_CLIENT" > "$OUT/ifb-qdisc-before.txt"

: > "$OUT/tcpdump.txt"
: > "$OUT/tcpdump.stderr"
ip netns exec "$NS_CLIENT" tcpdump --time-stamp-precision=nano -tt -n -l -i any \
    "tcp and src host $SERVER_IP and src port $PORT" \
    > "$OUT/tcpdump.txt" 2> "$OUT/tcpdump.stderr" &
TCPDUMP_PID=$!
sleep 0.1
kill -0 "$TCPDUMP_PID" 2>/dev/null || { cat "$OUT/tcpdump.stderr" >&2 || true; exit 1; }

ip netns exec "$NS_SERVER" python3 - "$SERVER_IP" "$PORT" "$PAYLOAD_BYTES" \
    > "$OUT/server.txt" 2> "$OUT/server.stderr" <<'PY' &
import socket
import struct
import sys

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
TCP_CONGESTION = getattr(socket, "TCP_CONGESTION", 13)
TCP_INFO = getattr(socket, "TCP_INFO", 11)


def tcp_info(sock):
    info = sock.getsockopt(socket.IPPROTO_TCP, TCP_INFO, 232)
    return {
        "rtt_us": struct.unpack_from("=I", info, 68)[0],
        "snd_ssthresh": struct.unpack_from("=I", info, 76)[0],
        "snd_cwnd": struct.unpack_from("=I", info, 80)[0],
        "total_retrans": struct.unpack_from("=I", info, 100)[0],
        "pacing_rate": struct.unpack_from("=Q", info, 104)[0],
        "delivery_rate": struct.unpack_from("=Q", info, 160)[0],
    }

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, b"cubic")
server.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
server.bind((host, port))
server.listen(1)
print("linux-envelope-ready", flush=True)
conn, _ = server.accept()
conn.settimeout(60.0)
chunk = b"x" * 65536
sent = 0
while sent < length:
    view = memoryview(chunk)[: min(len(chunk), length - sent)]
    written = conn.send(view)
    if written <= 0:
        raise RuntimeError("send made no progress")
    sent += written
conn.shutdown(socket.SHUT_WR)
while conn.recv(4096):
    pass
final = tcp_info(conn)
conn.close()
server.close()
print(
    f"linux-envelope-server bytes={sent} final_pacing_rate_Bps={final['pacing_rate']} "
    f"final_delivery_rate_Bps={final['delivery_rate']} final_rtt_us={final['rtt_us']} "
    f"final_cwnd={final['snd_cwnd']} final_ssthresh={final['snd_ssthresh']} "
    f"total_retrans={final['total_retrans']}",
    flush=True,
)
PY
SERVER_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'linux-envelope-ready' "$OUT/server.txt" >/dev/null 2>&1; do
    kill -0 "$SERVER_PID" 2>/dev/null || { cat "$OUT/server.stderr" >&2 || true; exit 1; }
    i=$((i + 1)); sleep 0.05
done
grep -F 'linux-envelope-ready' "$OUT/server.txt" >/dev/null || {
    echo "Linux envelope server ready timeout" >&2
    exit 1
}

ip netns exec "$NS_CLIENT" python3 - "$SERVER_IP" "$PORT" "$PAYLOAD_BYTES" \
    > "$OUT/client.txt" <<'PY'
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
stop_pid "$TCPDUMP_PID" INT
TCPDUMP_PID=

ip netns exec "$NS_SERVER" tc -s qdisc show dev "$VETH_SERVER" > "$OUT/server-qdisc-after.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$IFB_CLIENT" > "$OUT/ifb-qdisc-after.txt"

GOODPUT=$(sed -n 's/.*goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.txt")
FINAL_PACING=$(sed -n 's/.* final_pacing_rate_Bps=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | tail -n 1)
FINAL_DELIVERY=$(sed -n 's/.* final_delivery_rate_Bps=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | tail -n 1)
FINAL_RTT=$(sed -n 's/.* final_rtt_us=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | tail -n 1)
FINAL_CWND=$(sed -n 's/.* final_cwnd=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | tail -n 1)
FINAL_SSTHRESH=$(sed -n 's/.* final_ssthresh=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | tail -n 1)
FINAL_RETRANS=$(sed -n 's/.* total_retrans=\([0-9][0-9]*\).*/\1/p' "$OUT/server.txt" | tail -n 1)
[ -n "$GOODPUT" ] && [ -n "$FINAL_PACING" ] && [ -n "$FINAL_RETRANS" ] || {
    echo "missing Linux envelope metrics" >&2
    exit 1
}

printf 'linux_cubic_envelope_reference=ok case=%s base_rtt_ms=%s rate_mbit=%s bdp_bytes=%s queue_pkts=%s goodput_mbps=%s final_pacing_rate_Bps=%s final_delivery_rate_Bps=%s final_rtt_us=%s final_cwnd=%s final_ssthresh=%s total_retrans=%s\n' \
    "$CASE" "$RTT_MS" "$RATE_MBIT" "$BDP_BYTES" "$QUEUE_PKTS" "$GOODPUT" \
    "$FINAL_PACING" "$FINAL_DELIVERY" "$FINAL_RTT" "$FINAL_CWND" "$FINAL_SSTHRESH" "$FINAL_RETRANS" \
    | tee "$OUT/summary.txt"

python3 "$ROOT/scripts/cc-send-envelope-analyze.py" \
    --trace "$OUT/tcpdump.txt" \
    --interface "$VETH_CLIENT" \
    --rtt-ms "$RTT_MS" \
    --rate-mbit "$RATE_MBIT" \
    --mode linux-fq \
    --benchmark-summary "$OUT/summary.txt" \
    | tee "$OUT/envelope.txt"

uname -a > "$OUT/kernel.txt"
printf 'case=%s\nbase_rtt_ms=%s\nrate_mbit=%s\npayload_bytes=%s\nbdp_bytes=%s\nqueue_pkts=%s\ntrace_interface=%s\n' \
    "$CASE" "$RTT_MS" "$RATE_MBIT" "$PAYLOAD_BYTES" "$BDP_BYTES" "$QUEUE_PKTS" "$VETH_CLIENT" \
    > "$OUT/path.env"

echo "Linux CUBIC send-envelope reference completed: case=$CASE"
