#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p2-reset-recovery-ci"
BINARY=${TCP_SHIFT_P2_BINARY:-"$BUILD/tcp-shift-p2"}
TUN_NAME=${TCP_SHIFT_P2_RESET_TUN_NAME:-tsp2rst0}
LWIP_IP=${TCP_SHIFT_P2_RESET_LWIP_IP:-10.237.0.2}
NETMASK=${TCP_SHIFT_P2_RESET_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P2_RESET_HOST_IP:-10.237.0.1}
PUBLIC_PORT=${TCP_SHIFT_P2_RESET_PUBLIC_PORT:-18095}
BACKEND_PORT=${TCP_SHIFT_P2_RESET_BACKEND_PORT:-19095}
PAYLOAD_BYTES=${TCP_SHIFT_P2_RESET_PAYLOAD_BYTES:-65536}
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

python3 - "$BACKEND_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import struct
import sys

port = int(sys.argv[1])
length = int(sys.argv[2])
expected = bytes(((index * 41 + 13) & 0xFF) for index in range(length))
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(4)
print(f"backend-ready 127.0.0.1:{port}", flush=True)

first, _ = server.accept()
first.settimeout(5.0)
print("backend-first-accepted", flush=True)
try:
    while first.recv(1024):
        pass
except (ConnectionError, OSError):
    pass
first.close()
print("first-public-reset=observed", flush=True)

second, _ = server.accept()
second.settimeout(5.0)
print("backend-second-accepted", flush=True)
second.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
second.close()
print("second-backend-reset=sent", flush=True)

third, _ = server.accept()
third.settimeout(5.0)
chunks = []
while True:
    chunk = third.recv(16384)
    if not chunk:
        break
    chunks.append(chunk)
payload = b"".join(chunks)
if payload != expected:
    raise SystemExit(f"third flow payload mismatch: expected={length} got={len(payload)}")
third.sendall(payload)
third.shutdown(socket.SHUT_WR)
third.close()
server.close()
print(
    f"reset-recovery-backend-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok",
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
        echo "reset backend exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for reset backend" >&2
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
        echo "P2 reset runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for P2 reset runtime" >&2
    exit 1
}

python3 - "$LWIP_IP" "$PUBLIC_PORT" > "$OUT/public-reset-client.txt" <<'PY'
import socket
import struct
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(5.0)
sock.connect((host, port))
time.sleep(0.2)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
sock.close()
print("public-reset=sent")
PY

i=0
while [ "$i" -lt 100 ]; do
    if grep -F 'first-public-reset=observed' "$OUT/backend.stdout" >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.02
done
grep -F 'first-public-reset=observed' "$OUT/backend.stdout" >/dev/null
kill -0 "$PID"

python3 - "$LWIP_IP" "$PUBLIC_PORT" > "$OUT/backend-reset-client.txt" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
failed = False
try:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(5.0)
        sock.connect((host, port))
        try:
            data = sock.recv(1)
            if data == b"":
                failed = True
        except (ConnectionError, OSError):
            failed = True
except (ConnectionError, OSError):
    failed = True
if not failed:
    raise SystemExit("backend RST did not terminate the public flow")
print("backend-reset-public-flow=closed")
PY

i=0
while [ "$i" -lt 100 ]; do
    if grep -F 'second-backend-reset=sent' "$OUT/backend.stdout" >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.02
done
grep -F 'second-backend-reset=sent' "$OUT/backend.stdout" >/dev/null
kill -0 "$PID"

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" > "$OUT/recovery-client.txt" <<'PY'
import hashlib
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
payload = bytes(((index * 41 + 13) & 0xFF) for index in range(length))
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(5.0)
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
    raise SystemExit(f"reset recovery mismatch: sent={length} got={len(echo)}")
print(
    f"reset-recovery-client-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok"
)
PY

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    echo "reset backend failed" >&2
    exit 1
fi
BACKEND_PID=

sleep 0.2
kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P2 reset runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/public-reset-client.txt"
cat "$OUT/backend-reset-client.txt"
cat "$OUT/recovery-client.txt"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F 'public-reset=sent' "$OUT/public-reset-client.txt" >/dev/null
grep -F 'backend-reset-public-flow=closed' "$OUT/backend-reset-client.txt" >/dev/null
grep -F "reset-recovery-client-bytes=$PAYLOAD_BYTES " "$OUT/recovery-client.txt" >/dev/null
grep -F "reset-recovery-backend-bytes=$PAYLOAD_BYTES " "$OUT/backend.stdout" >/dev/null

grep -F "bridge_accepts=3 bridge_backend_connects=3 bridge_public_to_backend_bytes=$PAYLOAD_BYTES bridge_backend_to_public_bytes=$PAYLOAD_BYTES bridge_active_flows=0 bridge_peak_active_flows=1 bridge_backend_failures=1 bridge_public_errors=1" \
    "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "P2 reset TUN leaked after process exit" >&2
    exit 1
fi

printf 'public_reset=ok backend_reset=ok recovery_flow=ok payload_bytes=%u active_flows=0\n' \
    "$PAYLOAD_BYTES" | tee "$OUT/summary.txt"
echo "P2 reset recovery smoke passed"
