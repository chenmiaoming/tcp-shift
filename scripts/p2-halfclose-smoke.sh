#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p2-halfclose-ci"
BINARY=${TCP_SHIFT_P2_BINARY:-"$BUILD/tcp-shift-p2"}
TUN_NAME=${TCP_SHIFT_P2_HALFCLOSE_TUN_NAME:-tsp2hc0}
LWIP_IP=${TCP_SHIFT_P2_HALFCLOSE_LWIP_IP:-10.236.0.2}
NETMASK=${TCP_SHIFT_P2_HALFCLOSE_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P2_HALFCLOSE_HOST_IP:-10.236.0.1}
PUBLIC_PORT=${TCP_SHIFT_P2_HALFCLOSE_PUBLIC_PORT:-18094}
BACKEND_PORT=${TCP_SHIFT_P2_HALFCLOSE_BACKEND_PORT:-19094}
REQUEST_BYTES=${TCP_SHIFT_P2_HALFCLOSE_REQUEST_BYTES:-65536}
GREETING_BYTES=${TCP_SHIFT_P2_HALFCLOSE_GREETING_BYTES:-32768}
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

python3 - "$BACKEND_PORT" "$GREETING_BYTES" "$REQUEST_BYTES" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
greeting_len = int(sys.argv[2])
request_len = int(sys.argv[3])
greeting = bytes(((index * 7 + 3) & 0xFF) for index in range(greeting_len))
expected_request = bytes(((index * 37 + 5) & 0xFF) for index in range(request_len))
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
conn, peer = server.accept()
conn.settimeout(8.0)
conn.sendall(greeting)
conn.shutdown(socket.SHUT_WR)
print(
    f"backend-first-fin greeting_bytes={len(greeting)} sha256={hashlib.sha256(greeting).hexdigest()}",
    flush=True,
)
chunks = []
while True:
    chunk = conn.recv(16384)
    if not chunk:
        break
    chunks.append(chunk)
request = b"".join(chunks)
if request != expected_request:
    raise SystemExit(
        f"backend request mismatch: expected={len(expected_request)} got={len(request)}"
    )
print(
    f"backend-request-after-fin={len(request)} sha256={hashlib.sha256(request).hexdigest()} ok",
    flush=True,
)
conn.close()
server.close()
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
        echo "half-close backend exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for half-close backend" >&2
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
        echo "P2 half-close runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for P2 half-close runtime" >&2
    exit 1
}

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$GREETING_BYTES" "$REQUEST_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import hashlib
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
greeting_len = int(sys.argv[3])
request_len = int(sys.argv[4])
expected_greeting = bytes(((index * 7 + 3) & 0xFF) for index in range(greeting_len))
request = bytes(((index * 37 + 5) & 0xFF) for index in range(request_len))
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(8.0)
    sock.connect((host, port))
    chunks = []
    while True:
        chunk = sock.recv(16384)
        if not chunk:
            break
        chunks.append(chunk)
    greeting = b"".join(chunks)
    if greeting != expected_greeting:
        raise SystemExit(
            f"greeting mismatch: expected={len(expected_greeting)} got={len(greeting)}"
        )
    # Keep our write side open after observing the peer FIN. If tcp-shift leaves
    # level-triggered RDHUP armed during this interval, loop_wait_calls explodes.
    time.sleep(0.5)
    sock.sendall(request)
    sock.shutdown(socket.SHUT_WR)
print(
    f"client-observed-backend-fin greeting_bytes={len(greeting)} "
    f"request_after_fin={len(request)} greeting_sha256={hashlib.sha256(greeting).hexdigest()} ok"
)
PY

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    echo "half-close backend failed" >&2
    exit 1
fi
BACKEND_PID=

sleep 0.2
kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P2 half-close runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "client-observed-backend-fin greeting_bytes=$GREETING_BYTES request_after_fin=$REQUEST_BYTES " \
    "$OUT/client.stdout" >/dev/null
grep -F "backend-request-after-fin=$REQUEST_BYTES " "$OUT/backend.stdout" >/dev/null

grep -F "bridge_accepts=1 bridge_backend_connects=1 bridge_public_to_backend_bytes=$REQUEST_BYTES bridge_backend_to_public_bytes=$GREETING_BYTES bridge_active_flows=0 bridge_peak_active_flows=1 bridge_backend_failures=0 bridge_public_errors=0" \
    "$OUT/runtime.stderr" >/dev/null

metrics=$(grep -F 'tcp-shift-p2: rx_packets=' "$OUT/runtime.stderr" | tail -n 1)
wait_calls=$(printf '%s\n' "$metrics" | sed -n 's/.*loop_wait_calls=\([0-9][0-9]*\).*/\1/p')
[ -n "$wait_calls" ] && [ "$wait_calls" -le 64 ] || {
    echo "backend-first half-close caused readiness spin: loop_wait_calls=$wait_calls" >&2
    exit 1
}

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "P2 half-close TUN leaked after process exit" >&2
    exit 1
fi

printf 'backend_first_fin=ok request_after_fin=%u greeting_bytes=%u loop_wait_calls=%u active_flows=0 teardown=ok\n' \
    "$REQUEST_BYTES" "$GREETING_BYTES" "$wait_calls" | tee "$OUT/summary.txt"
echo "P2 backend-first half-close smoke passed"
