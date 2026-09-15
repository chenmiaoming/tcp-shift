#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
BINARY=${TCP_SHIFT_CUBIC_BENCH_BINARY:-"$BUILD/tcp-shift-p2"}
CASE=${TCP_SHIFT_CUBIC_BENCH_CASE:-wan}
RTT_MS=${TCP_SHIFT_CUBIC_BENCH_RTT_MS:-40}
RATE_MBIT=${TCP_SHIFT_CUBIC_BENCH_RATE_MBIT:-10}
LOSS_PCT=${TCP_SHIFT_CUBIC_BENCH_LOSS_PCT:-0}
PAYLOAD_BYTES=${TCP_SHIFT_CUBIC_BENCH_PAYLOAD_BYTES:-4194304}
OUT=${TCP_SHIFT_CUBIC_BENCH_OUT:-"$BUILD/cubic-linux-benchmark/$CASE"}

TUN_NAME="tscb$$"
IFB_NAME="tsifb$$"
LWIP_IP=10.252.0.2
HOST_IP=10.252.0.1
NETMASK=255.255.255.252
PUBLIC_PORT=18740
BACKEND_PORT=19740

NS_CLIENT="tscc$$"
NS_SERVER="tscs$$"
VETH_CLIENT=tsvethc
VETH_SERVER=tsveths
LINUX_CLIENT_IP=10.253.0.1
LINUX_SERVER_IP=10.253.0.2
LINUX_PORT=18741

RUNTIME_PID=
BACKEND_PID=
LINUX_SERVER_PID=

mkdir -p "$OUT/tcp-shift" "$OUT/linux"

[ "$(id -u)" -eq 0 ] || {
    echo "CUBIC benchmark requires root for TUN, IFB, namespaces and netem" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing tcp-shift benchmark binary: $BINARY" >&2
    exit 1
}
command -v tc >/dev/null 2>&1 || { echo "tc is required for CUBIC benchmark" >&2; exit 1; }
command -v ip >/dev/null 2>&1 || { echo "ip is required for CUBIC benchmark" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required for CUBIC benchmark" >&2; exit 1; }

case "$RTT_MS" in ''|*[!0-9]*) echo "RTT_MS must be an integer" >&2; exit 1;; esac
case "$RATE_MBIT" in ''|*[!0-9]*) echo "RATE_MBIT must be an integer" >&2; exit 1;; esac
case "$PAYLOAD_BYTES" in ''|*[!0-9]*) echo "PAYLOAD_BYTES must be an integer" >&2; exit 1;; esac
[ "$RTT_MS" -gt 0 ] && [ $((RTT_MS % 2)) -eq 0 ] || {
    echo "RTT_MS must be a positive even integer" >&2
    exit 1
}
[ "$RATE_MBIT" -gt 0 ] || { echo "RATE_MBIT must be positive" >&2; exit 1; }
[ "$PAYLOAD_BYTES" -gt 0 ] || { echo "PAYLOAD_BYTES must be positive" >&2; exit 1; }
case "$LOSS_PCT" in ''|*[!0-9.]*|*.*.*) echo "LOSS_PCT must be a nonnegative decimal" >&2; exit 1;; esac

HALF_RTT_MS=$((RTT_MS / 2))
BDP_BYTES=$((RATE_MBIT * RTT_MS * 125))
# Bound the bottleneck queue to roughly two path BDPs. The first benchmark
# iteration used netem's 10000-packet default-style deep queue; CUBIC filled it
# and converted a nominal 10-40 ms base RTT into 100-260 ms SRTT. Two BDPs is
# large enough to absorb normal bursts but small enough to make the configured
# RTT/bandwidth meaningful. Keep a 64-packet floor for very small-BDP cases.
QUEUE_PKTS=${TCP_SHIFT_CUBIC_BENCH_QUEUE_PKTS:-$(((BDP_BYTES * 2 + 1459) / 1460))}
[ "$QUEUE_PKTS" -ge 64 ] || QUEUE_PKTS=64

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
    stop_pid "${LINUX_SERVER_PID:-}"
    tc qdisc del dev "$TUN_NAME" root >/dev/null 2>&1 || true
    tc qdisc del dev "$TUN_NAME" ingress >/dev/null 2>&1 || true
    tc qdisc del dev "$IFB_NAME" root >/dev/null 2>&1 || true
    ip link del "$IFB_NAME" >/dev/null 2>&1 || true
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
    ip netns del "$NS_CLIENT" >/dev/null 2>&1 || true
    ip netns del "$NS_SERVER" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

add_data_netem()
{
    dev=$1
    if [ "$LOSS_PCT" = 0 ] || [ "$LOSS_PCT" = 0.0 ]; then
        tc qdisc replace dev "$dev" root netem \
            delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
    else
        # Ubuntu 24.04's iproute2 6.1 netem does not support the newer `seed`
        # option. Keep this loss case stochastic and retain qdisc counters in
        # the artifact instead of pretending it is bit-for-bit reproducible.
        tc qdisc replace dev "$dev" root netem \
            delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" \
            loss random "${LOSS_PCT}%" limit "$QUEUE_PKTS"
    fi
}

run_receiver()
{
    host=$1
    port=$2
    output=$3
    namespace=${4:-}

    if [ -n "$namespace" ]; then
        prefix="ip netns exec $namespace"
    else
        prefix=
    fi

    # shellcheck disable=SC2086
    $prefix python3 - "$host" "$port" "$PAYLOAD_BYTES" > "$output" <<'PY'
import hashlib
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
expected = int(sys.argv[3])
start = time.monotonic_ns()
received = 0
digest = hashlib.sha256()
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(60.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
    sock.connect((host, port))
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        received += len(chunk)
        digest.update(chunk)
elapsed = time.monotonic_ns() - start
if received != expected:
    raise SystemExit(f"received={received} expected={expected}")
goodput_mbps = received * 8.0 * 1000.0 / elapsed
print(
    f"bytes={received} elapsed_ns={elapsed} goodput_mbps={goodput_mbps:.6f} "
    f"sha256={digest.hexdigest()}"
)
PY
}

# tcp-shift CUBIC: backend sends; public lwIP TCP is the measured sender.
: > "$OUT/tcp-shift/backend.stdout"
: > "$OUT/tcp-shift/backend.stderr"
: > "$OUT/tcp-shift/runtime.stdout"
: > "$OUT/tcp-shift/runtime.stderr"

python3 - "$BACKEND_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/tcp-shift/backend.stdout" 2> "$OUT/tcp-shift/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
length = int(sys.argv[2])
payload = bytes(((i * 73 + 19) & 0xff) for i in range(length))
digest = hashlib.sha256(payload).hexdigest()
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready port={port}", flush=True)
conn, _ = server.accept()
conn.settimeout(60.0)
conn.sendall(payload)
conn.shutdown(socket.SHUT_WR)
while conn.recv(4096):
    pass
conn.close()
server.close()
print(f"backend-bytes={length} sha256={digest}", flush=True)
PY
BACKEND_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'backend-ready ' "$OUT/tcp-shift/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$BACKEND_PID" 2>/dev/null || { cat "$OUT/tcp-shift/backend.stderr" >&2 || true; exit 1; }
    i=$((i + 1)); sleep 0.05
done
grep -F 'backend-ready ' "$OUT/tcp-shift/backend.stdout" >/dev/null || { echo "backend ready timeout" >&2; exit 1; }

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" cubic \
    > "$OUT/tcp-shift/runtime.stdout" 2> "$OUT/tcp-shift/runtime.stderr" &
RUNTIME_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 && grep -F "tcp-shift-p2: ready tun=$TUN_NAME" "$OUT/tcp-shift/runtime.stdout" >/dev/null 2>&1; then break; fi
    kill -0 "$RUNTIME_PID" 2>/dev/null || { cat "$OUT/tcp-shift/runtime.stderr" >&2 || true; exit 1; }
    i=$((i + 1)); sleep 0.05
done
[ "$i" -lt 100 ] || { echo "runtime ready timeout" >&2; exit 1; }
grep -F 'cc=cubic' "$OUT/tcp-shift/runtime.stdout" >/dev/null

modprobe ifb >/dev/null 2>&1 || true
ip link add "$IFB_NAME" type ifb
ip link set "$IFB_NAME" up
tc qdisc add dev "$TUN_NAME" handle ffff: ingress
tc filter add dev "$TUN_NAME" parent ffff: protocol ip prio 1 u32 match u32 0 0 action mirred egress redirect dev "$IFB_NAME"
add_data_netem "$IFB_NAME"
tc qdisc replace dev "$TUN_NAME" root netem delay "${HALF_RTT_MS}ms" limit "$QUEUE_PKTS"
tc -s qdisc show dev "$TUN_NAME" > "$OUT/tcp-shift/tun-qdisc-before.txt"
tc -s qdisc show dev "$IFB_NAME" > "$OUT/tcp-shift/ifb-qdisc-before.txt"

run_receiver "$LWIP_IP" "$PUBLIC_PORT" "$OUT/tcp-shift/client.txt"
wait "$BACKEND_PID" || { BACKEND_PID=; cat "$OUT/tcp-shift/backend.stderr" >&2 || true; exit 1; }
BACKEND_PID=
tc -s qdisc show dev "$TUN_NAME" > "$OUT/tcp-shift/tun-qdisc-after.txt"
tc -s qdisc show dev "$IFB_NAME" > "$OUT/tcp-shift/ifb-qdisc-after.txt"
sleep 0.2
kill -TERM "$RUNTIME_PID"
wait "$RUNTIME_PID" || { RUNTIME_PID=; cat "$OUT/tcp-shift/runtime.stderr" >&2 || true; exit 1; }
RUNTIME_PID=

grep -F 'cc_controller_errors=0' "$OUT/tcp-shift/runtime.stderr" >/dev/null
TS_CWND=$(sed -n 's/.* cc_last_cwnd=\([0-9][0-9]*\).*/\1/p' "$OUT/tcp-shift/runtime.stderr" | tail -n 1)
TS_LOSS=$(sed -n 's/.* cc_loss_events=\([0-9][0-9]*\).*/\1/p' "$OUT/tcp-shift/runtime.stderr" | tail -n 1)
TS_TIMEOUT=$(sed -n 's/.* cc_timeout_events=\([0-9][0-9]*\).*/\1/p' "$OUT/tcp-shift/runtime.stderr" | tail -n 1)
[ -n "$TS_CWND" ] && [ -n "$TS_LOSS" ] && [ -n "$TS_TIMEOUT" ] || { echo "missing tcp-shift metrics" >&2; exit 1; }

tc qdisc del dev "$TUN_NAME" root >/dev/null 2>&1 || true
tc qdisc del dev "$TUN_NAME" ingress >/dev/null 2>&1 || true
tc qdisc del dev "$IFB_NAME" root >/dev/null 2>&1 || true
ip link del "$IFB_NAME" >/dev/null 2>&1 || true

# Linux tcp_cubic reference in isolated namespaces.
ip netns add "$NS_CLIENT"
ip netns add "$NS_SERVER"
ip link add "$VETH_CLIENT" type veth peer name "$VETH_SERVER"
ip link set "$VETH_CLIENT" netns "$NS_CLIENT"
ip link set "$VETH_SERVER" netns "$NS_SERVER"
ip -n "$NS_CLIENT" addr add "$LINUX_CLIENT_IP/30" dev "$VETH_CLIENT"
ip -n "$NS_SERVER" addr add "$LINUX_SERVER_IP/30" dev "$VETH_SERVER"
ip -n "$NS_CLIENT" link set lo up
ip -n "$NS_SERVER" link set lo up
ip -n "$NS_CLIENT" link set "$VETH_CLIENT" up
ip -n "$NS_SERVER" link set "$VETH_SERVER" up
if command -v ethtool >/dev/null 2>&1; then
    ip netns exec "$NS_CLIENT" ethtool -K "$VETH_CLIENT" tso off gso off gro off >/dev/null 2>&1 || true
    ip netns exec "$NS_SERVER" ethtool -K "$VETH_SERVER" tso off gso off gro off >/dev/null 2>&1 || true
fi
if [ "$LOSS_PCT" = 0 ] || [ "$LOSS_PCT" = 0.0 ]; then
    ip netns exec "$NS_SERVER" tc qdisc replace dev "$VETH_SERVER" root netem delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
else
    ip netns exec "$NS_SERVER" tc qdisc replace dev "$VETH_SERVER" root netem delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" loss random "${LOSS_PCT}%" limit "$QUEUE_PKTS"
fi
ip netns exec "$NS_CLIENT" tc qdisc replace dev "$VETH_CLIENT" root netem delay "${HALF_RTT_MS}ms" limit "$QUEUE_PKTS"
ip netns exec "$NS_SERVER" tc -s qdisc show dev "$VETH_SERVER" > "$OUT/linux/server-qdisc-before.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$VETH_CLIENT" > "$OUT/linux/client-qdisc-before.txt"

ip netns exec "$NS_SERVER" python3 - "$LINUX_SERVER_IP" "$LINUX_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/linux/server.txt" 2> "$OUT/linux/server.stderr" <<'PY' &
import hashlib
import socket
import struct
import sys

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
payload = bytes(((i * 73 + 19) & 0xff) for i in range(length))
digest = hashlib.sha256(payload).hexdigest()
TCP_CONGESTION = getattr(socket, "TCP_CONGESTION", 13)
TCP_INFO = getattr(socket, "TCP_INFO", 11)
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, b"cubic")
server.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
server.bind((host, port))
server.listen(1)
print(f"linux-ready cc={server.getsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, 16).rstrip(bytes([0])).decode()}", flush=True)
conn, _ = server.accept()
conn.settimeout(60.0)
cc = conn.getsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, 16).rstrip(bytes([0])).decode()
conn.sendall(payload)
conn.shutdown(socket.SHUT_WR)
while conn.recv(4096):
    pass
info = conn.getsockopt(socket.IPPROTO_TCP, TCP_INFO, 104)
snd_cwnd = struct.unpack_from("=I", info, 80)[0]
rtt_us = struct.unpack_from("=I", info, 68)[0]
total_retrans = struct.unpack_from("=I", info, 100)[0]
conn.close(); server.close()
print(f"linux-server cc={cc} bytes={length} sha256={digest} snd_cwnd_packets={snd_cwnd} rtt_us={rtt_us} total_retrans={total_retrans}", flush=True)
PY
LINUX_SERVER_PID=$!
i=0
while [ "$i" -lt 100 ] && ! grep -F 'linux-ready cc=cubic' "$OUT/linux/server.txt" >/dev/null 2>&1; do
    kill -0 "$LINUX_SERVER_PID" 2>/dev/null || { cat "$OUT/linux/server.stderr" >&2 || true; exit 1; }
    i=$((i + 1)); sleep 0.05
done
grep -F 'linux-ready cc=cubic' "$OUT/linux/server.txt" >/dev/null || { echo "Linux server ready timeout" >&2; exit 1; }
run_receiver "$LINUX_SERVER_IP" "$LINUX_PORT" "$OUT/linux/client.txt" "$NS_CLIENT"
wait "$LINUX_SERVER_PID" || { LINUX_SERVER_PID=; cat "$OUT/linux/server.stderr" >&2 || true; exit 1; }
LINUX_SERVER_PID=
ip netns exec "$NS_SERVER" tc -s qdisc show dev "$VETH_SERVER" > "$OUT/linux/server-qdisc-after.txt"
ip netns exec "$NS_CLIENT" tc -s qdisc show dev "$VETH_CLIENT" > "$OUT/linux/client-qdisc-after.txt"

TS_GOODPUT=$(sed -n 's/.* goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/tcp-shift/client.txt")
LINUX_GOODPUT=$(sed -n 's/.* goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/linux/client.txt")
LINUX_CWND=$(sed -n 's/.* snd_cwnd_packets=\([0-9][0-9]*\).*/\1/p' "$OUT/linux/server.txt" | tail -n 1)
LINUX_RTT_US=$(sed -n 's/.* rtt_us=\([0-9][0-9]*\).*/\1/p' "$OUT/linux/server.txt" | tail -n 1)
LINUX_RETRANS=$(sed -n 's/.* total_retrans=\([0-9][0-9]*\).*/\1/p' "$OUT/linux/server.txt" | tail -n 1)
[ -n "$TS_GOODPUT" ] && [ -n "$LINUX_GOODPUT" ] && [ -n "$LINUX_CWND" ] || { echo "missing benchmark metrics" >&2; exit 1; }

python3 - "$CASE" "$RTT_MS" "$RATE_MBIT" "$LOSS_PCT" "$PAYLOAD_BYTES" "$BDP_BYTES" "$QUEUE_PKTS" \
    "$TS_GOODPUT" "$LINUX_GOODPUT" "$TS_CWND" "$TS_LOSS" "$TS_TIMEOUT" "$LINUX_CWND" "$LINUX_RTT_US" "$LINUX_RETRANS" <<'PY' | tee "$OUT/summary.txt"
import sys
(case, rtt_ms, rate_mbit, loss_pct, payload, bdp, queue_pkts, ts_g, linux_g,
 ts_cwnd, ts_loss, ts_timeout, linux_cwnd, linux_rtt_us, linux_retrans) = sys.argv[1:]
ts = float(ts_g); linux = float(linux_g); ratio = ts / linux if linux else 0.0
print(
    f"cubic_linux_benchmark=ok case={case} base_rtt_ms={rtt_ms} rate_mbit={rate_mbit} "
    f"loss_pct={loss_pct} payload_bytes={payload} bdp_bytes={bdp} queue_pkts={queue_pkts} "
    f"tcp_shift_goodput_mbps={ts:.6f} linux_goodput_mbps={linux:.6f} ratio={ratio:.6f} "
    f"tcp_shift_cwnd_bytes={ts_cwnd} tcp_shift_loss_events={ts_loss} tcp_shift_timeout_events={ts_timeout} "
    f"linux_cwnd_packets={linux_cwnd} linux_rtt_us={linux_rtt_us} linux_total_retrans={linux_retrans}"
)
PY

uname -a > "$OUT/kernel.txt"
sysctl net.ipv4.tcp_available_congestion_control > "$OUT/linux-congestion-controls.txt"
if [ -d /sys/module/tcp_cubic/parameters ]; then
    for file in /sys/module/tcp_cubic/parameters/*; do
        [ -f "$file" ] || continue
        printf '%s=' "$(basename "$file")"; cat "$file"
    done > "$OUT/linux-cubic-parameters.txt"
fi
printf 'base_rtt_ms=%s\nrate_mbit=%s\nloss_pct=%s\nbdp_bytes=%s\nqueue_pkts=%s\n' \
    "$RTT_MS" "$RATE_MBIT" "$LOSS_PCT" "$BDP_BYTES" "$QUEUE_PKTS" > "$OUT/path.env"
echo "CUBIC tcp-shift/Linux benchmark case $CASE completed"
