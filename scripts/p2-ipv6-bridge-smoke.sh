#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p2-ipv6-bridge-ci"
BINARY=${TCP_SHIFT_P2_IPV6_BINARY:-"$BUILD/tcp-shift-p2-ipv6"}
TUN_NAME=${TCP_SHIFT_P2_IPV6_TUN_NAME:-tsp2v6ci0}
LWIP_IP=${TCP_SHIFT_P2_IPV6_LWIP_IP:-fd00:198:22::2}
HOST_CIDR=${TCP_SHIFT_P2_IPV6_HOST_CIDR:-fd00:198:22::1/126}
PUBLIC_PORT=${TCP_SHIFT_P2_IPV6_PUBLIC_PORT:-18091}
BACKEND_PORT=${TCP_SHIFT_P2_IPV6_BACKEND_PORT:-19091}
PAYLOAD_BYTES=${TCP_SHIFT_P2_IPV6_PAYLOAD_BYTES:-131072}
PID=
BACKEND_PID=

mkdir -p "$OUT"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"

[ "$(id -u)" -eq 0 ] || {
    echo "p2-ipv6-bridge-smoke.sh must run as root" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing P2 IPv6 binary: $BINARY" >&2
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

capture_state()
{
    ip -6 -d addr show > "$OUT/ip6-addr.txt" 2>&1 || true
    ip -6 route show table all > "$OUT/ip6-route.txt" 2>&1 || true
}

cleanup()
{
    set +e
    stop_runtime
    stop_backend
    capture_state
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

python3 - "$BACKEND_PORT" "$OUT/backend-payload.bin" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
path = sys.argv[2]
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
conn, peer = server.accept()
conn.settimeout(8.0)
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

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$HOST_CIDR" \
    "$PUBLIC_PORT" "$BACKEND_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p2-ipv6: ready tun=$TUN_NAME" \
           "$OUT/runtime.stdout" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "P2 IPv6 runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for P2 IPv6 runtime" >&2
    exit 1
}

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import hashlib
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
payload = bytes(((index * 17 + 11) & 0xFF) for index in range(length))
with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as sock:
    sock.settimeout(8.0)
    sock.connect((host, port, 0, 0))
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
        f"IPv6 echo mismatch: sent={len(payload)} received={len(echo)} "
        f"sent_sha={hashlib.sha256(payload).hexdigest()} "
        f"received_sha={hashlib.sha256(echo).hexdigest()}"
    )
print(
    f"ipv6-bridge-client-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok"
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

sleep 0.2
kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stdout" >&2 || true
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P2 IPv6 runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "ipv6-bridge-client-bytes=$PAYLOAD_BYTES " "$OUT/client.stdout" >/dev/null
grep -F ' echo=ok' "$OUT/client.stdout" >/dev/null
grep -F "backend-bytes=$PAYLOAD_BYTES " "$OUT/backend.stdout" >/dev/null
grep -F ' echo=ok' "$OUT/backend.stdout" >/dev/null

grep -F "bridge_accepts=1 bridge_backend_connects=1 bridge_public_to_backend_bytes=$PAYLOAD_BYTES bridge_backend_to_public_bytes=$PAYLOAD_BYTES bridge_active_flows=0 bridge_peak_active_flows=1 bridge_backend_failures=0 bridge_public_errors=0" \
    "$OUT/runtime.stderr" >/dev/null

grep -F 'rx_errors=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'tx_queue_drops=0' "$OUT/runtime.stderr" >/dev/null

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "P2 IPv6 TUN leaked after process exit" >&2
    exit 1
fi

capture_state
printf 'public_family=ipv6 backend_family=ipv4 payload_bytes=%u bidirectional_integrity=ok active_flows=0 teardown=ok\n' \
    "$PAYLOAD_BYTES" | tee "$OUT/summary.txt"
echo "P2 IPv6 public-to-IPv4-loopback bridge smoke passed"
