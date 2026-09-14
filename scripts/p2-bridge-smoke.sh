#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p2-bridge-ci"
BINARY=${TCP_SHIFT_P2_BINARY:-"$BUILD/tcp-shift-p2"}
TUN_NAME=${TCP_SHIFT_P2_TUN_NAME:-tsp2ci0}
LWIP_IP=${TCP_SHIFT_P2_LWIP_IP:-10.233.0.2}
NETMASK=${TCP_SHIFT_P2_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P2_HOST_IP:-10.233.0.1}
PUBLIC_PORT=${TCP_SHIFT_P2_PUBLIC_PORT:-18090}
BACKEND_PORT=${TCP_SHIFT_P2_BACKEND_PORT:-19090}
PAYLOAD_BYTES=${TCP_SHIFT_P2_PAYLOAD_BYTES:-131072}
NETEM_DELAY_MS=${TCP_SHIFT_P2_NETEM_DELAY_MS:-0}
SOCKET_TIMEOUT_SECONDS=${TCP_SHIFT_P2_SOCKET_TIMEOUT_SECONDS:-8}
CAPTURE_RUNTIME_CPU=${TCP_SHIFT_P2_CAPTURE_RUNTIME_CPU:-0}
PID=
BACKEND_PID=
RUNTIME_CPU_START_TICKS=
RUNTIME_CPU_START_NS=
RUNTIME_CPU_CLK_TCK=

mkdir -p "$OUT"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
rm -f "$OUT/runtime-cpu.txt" "$OUT/netem-before.txt" "$OUT/netem-after.txt"

[ -x "$BINARY" ] || {
    echo "missing P2 binary: $BINARY" >&2
    exit 1
}
case "$NETEM_DELAY_MS" in
    ''|*[!0-9]*)
        echo "TCP_SHIFT_P2_NETEM_DELAY_MS must be a nonnegative integer" >&2
        exit 1
        ;;
esac
case "$SOCKET_TIMEOUT_SECONDS" in
    ''|*[!0-9]*)
        echo "TCP_SHIFT_P2_SOCKET_TIMEOUT_SECONDS must be a positive integer" >&2
        exit 1
        ;;
esac
[ "$SOCKET_TIMEOUT_SECONDS" -gt 0 ] || {
    echo "TCP_SHIFT_P2_SOCKET_TIMEOUT_SECONDS must be positive" >&2
    exit 1
}
case "$CAPTURE_RUNTIME_CPU" in
    0|1) ;;
    *)
        echo "TCP_SHIFT_P2_CAPTURE_RUNTIME_CPU must be 0 or 1" >&2
        exit 1
        ;;
esac

stop_runtime()
{
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID"
        wait "$PID"
    fi
    PID=
}

stop_backend()
{
    if [ -n "${BACKEND_PID:-}" ] && kill -0 "$BACKEND_PID" 2>/dev/null; then
        kill -TERM "$BACKEND_PID" >/dev/null 2>&1 || true
        wait "$BACKEND_PID" >/dev/null 2>&1 || true
    fi
    BACKEND_PID=
}

capture_state()
{
    ip -d addr show > "$OUT/ip-addr.txt" 2>&1 || true
    ip route show table all > "$OUT/ip-route.txt" 2>&1 || true
}

cleanup()
{
    set +e
    stop_runtime
    stop_backend
    capture_state
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        sudo ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

python3 - "$BACKEND_PORT" "$OUT/backend-payload.bin" "$SOCKET_TIMEOUT_SECONDS" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
path = sys.argv[2]
timeout = float(sys.argv[3])
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
conn, peer = server.accept()
conn.settimeout(timeout)
chunks = []
while True:
    chunk = conn.recv(16384)
    if not chunk:
        break
    chunks.append(chunk)
payload = b"".join(chunks)
with open(path, "wb") as output:
    output.write(payload)
conn.sendall(payload)
conn.shutdown(socket.SHUT_WR)
conn.close()
server.close()
print(
    f"backend-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok",
    flush=True,
)
PY
BACKEND_PID=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
        cat "$OUT/backend.stderr" >&2 || true
        echo "backend exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for backend" >&2
    exit 1
}

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p2: ready tun=$TUN_NAME" \
           "$OUT/runtime.stdout" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "P2 runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for P2 runtime" >&2
    exit 1
}

if [ "$NETEM_DELAY_MS" -gt 0 ]; then
    [ "$(id -u)" -eq 0 ] || {
        echo "P2 netem qualification requires root" >&2
        exit 1
    }
    command -v tc >/dev/null 2>&1 || {
        echo "tc is required for P2 netem qualification" >&2
        exit 1
    }
    tc qdisc replace dev "$TUN_NAME" root netem delay "${NETEM_DELAY_MS}ms"
    tc -s qdisc show dev "$TUN_NAME" > "$OUT/netem-before.txt"
fi

if [ "$CAPTURE_RUNTIME_CPU" -eq 1 ]; then
    RUNTIME_CPU_CLK_TCK=$(getconf CLK_TCK)
    RUNTIME_CPU_START_TICKS=$(awk '{print $14 + $15}' "/proc/$PID/stat")
    RUNTIME_CPU_START_NS=$(date +%s%N)
fi

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" "$SOCKET_TIMEOUT_SECONDS" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import hashlib
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
timeout = float(sys.argv[4])
payload = bytes(((index * 31 + 7) & 0xFF) for index in range(length))
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(timeout)
    sock.connect((host, port))
    sock.sendall(payload)
    sock.shutdown(socket.SHUT_WR)
    chunks = []
    while True:
        chunk = sock.recv(16384)
        if not chunk:
            break
        chunks.append(chunk)
echo = b"".join(chunks)
if echo != payload:
    raise SystemExit(
        f"echo mismatch: sent={len(payload)} received={len(echo)} "
        f"sent_sha={hashlib.sha256(payload).hexdigest()} "
        f"received_sha={hashlib.sha256(echo).hexdigest()}"
    )
print(
    f"bridge-client-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok"
)
PY

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    echo "backend failed" >&2
    exit 1
fi
BACKEND_PID=

if [ "$CAPTURE_RUNTIME_CPU" -eq 1 ]; then
    runtime_cpu_end_ticks=$(awk '{print $14 + $15}' "/proc/$PID/stat")
    runtime_cpu_end_ns=$(date +%s%N)
    runtime_cpu_elapsed_ticks=$((runtime_cpu_end_ticks - RUNTIME_CPU_START_TICKS))
    runtime_cpu_elapsed_ns=$((runtime_cpu_end_ns - RUNTIME_CPU_START_NS))
    printf 'clk_tck=%s start_ticks=%s end_ticks=%s elapsed_ticks=%s start_ns=%s end_ns=%s elapsed_ns=%s\n' \
        "$RUNTIME_CPU_CLK_TCK" "$RUNTIME_CPU_START_TICKS" \
        "$runtime_cpu_end_ticks" "$runtime_cpu_elapsed_ticks" \
        "$RUNTIME_CPU_START_NS" "$runtime_cpu_end_ns" \
        "$runtime_cpu_elapsed_ns" > "$OUT/runtime-cpu.txt"
fi

if [ "$NETEM_DELAY_MS" -gt 0 ]; then
    tc -s qdisc show dev "$TUN_NAME" > "$OUT/netem-after.txt"
fi

# Client EOF means the bridge propagated backend EOF to lwIP. Give the same
# single-owner loop a short chance to process the client's final ACK/close path;
# the metrics are captured before bridge_stop(), so active_flows=0 proves the
# flow state machine released itself rather than process shutdown hiding a leak.
sleep 0.2
kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stdout" >&2 || true
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P2 runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "bridge-client-bytes=$PAYLOAD_BYTES " "$OUT/client.stdout" >/dev/null
grep -F ' echo=ok' "$OUT/client.stdout" >/dev/null
grep -F "backend-bytes=$PAYLOAD_BYTES " "$OUT/backend.stdout" >/dev/null
grep -F ' echo=ok' "$OUT/backend.stdout" >/dev/null

grep -F "bridge_accepts=1 bridge_backend_connects=1 bridge_public_to_backend_bytes=$PAYLOAD_BYTES bridge_backend_to_public_bytes=$PAYLOAD_BYTES bridge_active_flows=0 bridge_peak_active_flows=1 bridge_backend_failures=0 bridge_public_errors=0" \
    "$OUT/runtime.stderr" >/dev/null

grep -F 'rx_errors=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'tx_queue_drops=0' "$OUT/runtime.stderr" >/dev/null

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "P2 TUN leaked after process exit" >&2
    exit 1
fi

capture_state
printf 'payload_bytes=%u bidirectional_integrity=ok active_flows=0 teardown=ok\n' \
    "$PAYLOAD_BYTES" | tee "$OUT/summary.txt"
echo "P2 IPv4 public-to-loopback bridge smoke passed"
