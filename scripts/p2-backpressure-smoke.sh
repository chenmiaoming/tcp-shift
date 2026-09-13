#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p2-backpressure-ci"
BINARY=${TCP_SHIFT_P2_BINARY:-"$BUILD/tcp-shift-p2"}
TUN_NAME=${TCP_SHIFT_P2_BP_TUN_NAME:-tsp2bp0}
LWIP_IP=${TCP_SHIFT_P2_BP_LWIP_IP:-10.234.0.2}
NETMASK=${TCP_SHIFT_P2_BP_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P2_BP_HOST_IP:-10.234.0.1}
PUBLIC_PORT=${TCP_SHIFT_P2_BP_PUBLIC_PORT:-18092}
BACKEND_PORT=${TCP_SHIFT_P2_BP_BACKEND_PORT:-19092}
PAYLOAD_BYTES=${TCP_SHIFT_P2_BP_PAYLOAD_BYTES:-1048576}
PID=
BACKEND_PID=

mkdir -p "$OUT"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"

[ -x "$BINARY" ] || {
    echo "missing P2 binary: $BINARY" >&2
    exit 1
}

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

cleanup()
{
    set +e
    stop_runtime
    stop_backend
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        sudo ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

# Constrain the backend receive window and deliberately do not read at first.
# Together with tcp-shift's bounded loopback SO_SNDBUF this forces the
# public->backend path to hit real EAGAIN instead of hiding pressure in a large
# Linux loopback send buffer.
python3 - "$BACKEND_PORT" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys
import time

port = int(sys.argv[1])
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
conn, peer = server.accept()
conn.settimeout(15.0)
print("backend-delay-read=0.5", flush=True)
time.sleep(0.5)
chunks = []
while True:
    chunk = conn.recv(16384)
    if not chunk:
        break
    chunks.append(chunk)
payload = b"".join(chunks)
print(
    f"backend-received={len(payload)} sha256={hashlib.sha256(payload).hexdigest()}",
    flush=True,
)
conn.sendall(payload)
conn.shutdown(socket.SHUT_WR)
conn.close()
server.close()
print(f"backend-echoed={len(payload)}", flush=True)
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
        echo "backpressure backend exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for backpressure backend" >&2
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
        cat "$OUT/runtime.stderr" >&2 || true
        echo "P2 backpressure runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for P2 backpressure runtime" >&2
    exit 1
}

# The client advertises a small receive buffer and pauses before reading the
# echo. This forces backend->public to fill lwIP's send budget and proves the
# bridge disables backend EPOLLIN until tcp_sent/tcp_poll makes progress.
python3 - "$LWIP_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import hashlib
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
payload = bytes(((index * 13 + 19) & 0xFF) for index in range(length))
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    sock.settimeout(15.0)
    sock.connect((host, port))
    sock.sendall(payload)
    sock.shutdown(socket.SHUT_WR)
    time.sleep(0.5)
    chunks = []
    while True:
        chunk = sock.recv(16384)
        if not chunk:
            break
        chunks.append(chunk)
echo = b"".join(chunks)
if echo != payload:
    raise SystemExit(
        f"backpressure echo mismatch: sent={len(payload)} received={len(echo)} "
        f"sent_sha={hashlib.sha256(payload).hexdigest()} "
        f"received_sha={hashlib.sha256(echo).hexdigest()}"
    )
print(
    f"backpressure-client-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok"
)
PY

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    echo "backpressure backend failed" >&2
    exit 1
fi
BACKEND_PID=

sleep 0.2
kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stdout" >&2 || true
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P2 backpressure runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "backpressure-client-bytes=$PAYLOAD_BYTES " "$OUT/client.stdout" >/dev/null
grep -F ' echo=ok' "$OUT/client.stdout" >/dev/null
grep -F "backend-received=$PAYLOAD_BYTES " "$OUT/backend.stdout" >/dev/null
grep -F "backend-echoed=$PAYLOAD_BYTES" "$OUT/backend.stdout" >/dev/null

grep -F "bridge_public_to_backend_bytes=$PAYLOAD_BYTES bridge_backend_to_public_bytes=$PAYLOAD_BYTES bridge_active_flows=0" \
    "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_backend_failures=0 bridge_public_errors=0' "$OUT/runtime.stderr" >/dev/null

metrics=$(grep -F 'tcp-shift-p2: rx_packets=' "$OUT/runtime.stderr" | tail -n 1)
peak_pending=$(printf '%s\n' "$metrics" | sed -n 's/.*bridge_peak_pending_public_bytes=\([0-9][0-9]*\).*/\1/p')
write_blocked=$(printf '%s\n' "$metrics" | sed -n 's/.*bridge_backend_write_blocked_events=\([0-9][0-9]*\).*/\1/p')
read_blocked=$(printf '%s\n' "$metrics" | sed -n 's/.*bridge_backend_read_blocked_events=\([0-9][0-9]*\).*/\1/p')
sndbuf=$(printf '%s\n' "$metrics" | sed -n 's/.*bridge_backend_socket_sndbuf_bytes=\([0-9][0-9]*\).*/\1/p')
rcvbuf=$(printf '%s\n' "$metrics" | sed -n 's/.*bridge_backend_socket_rcvbuf_bytes=\([0-9][0-9]*\).*/\1/p')

[ -n "$peak_pending" ] && [ "$peak_pending" -gt 0 ] && [ "$peak_pending" -le 32768 ] || {
    echo "public pending-pbuf bound violated: $peak_pending" >&2
    exit 1
}
[ -n "$write_blocked" ] && [ "$write_blocked" -gt 0 ] || {
    echo "public->backend path did not hit real socket backpressure" >&2
    exit 1
}
[ -n "$read_blocked" ] && [ "$read_blocked" -gt 0 ] || {
    echo "backend->public path did not hit lwIP send backpressure" >&2
    exit 1
}
[ -n "$sndbuf" ] && [ "$sndbuf" -gt 0 ] && [ "$sndbuf" -le 65536 ] || {
    echo "backend SO_SNDBUF bound violated: $sndbuf" >&2
    exit 1
}
[ -n "$rcvbuf" ] && [ "$rcvbuf" -gt 0 ] && [ "$rcvbuf" -le 65536 ] || {
    echo "backend SO_RCVBUF bound violated: $rcvbuf" >&2
    exit 1
}

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "P2 backpressure TUN leaked after process exit" >&2
    exit 1
fi

printf 'payload_bytes=%u peak_pending_public_bytes=%u backend_write_blocked_events=%u backend_read_blocked_events=%u backend_sndbuf=%u backend_rcvbuf=%u bounded_backpressure=ok\n' \
    "$PAYLOAD_BYTES" "$peak_pending" "$write_blocked" "$read_blocked" "$sndbuf" "$rcvbuf" \
    | tee "$OUT/summary.txt"
echo "P2 deterministic bidirectional backpressure smoke passed"
