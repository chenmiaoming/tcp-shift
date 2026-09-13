#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p2-backend-recovery-ci"
BINARY=${TCP_SHIFT_P2_BINARY:-"$BUILD/tcp-shift-p2"}
TUN_NAME=${TCP_SHIFT_P2_RECOVERY_TUN_NAME:-tsp2rec0}
LWIP_IP=${TCP_SHIFT_P2_RECOVERY_LWIP_IP:-10.235.0.2}
NETMASK=${TCP_SHIFT_P2_RECOVERY_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P2_RECOVERY_HOST_IP:-10.235.0.1}
PUBLIC_PORT=${TCP_SHIFT_P2_RECOVERY_PUBLIC_PORT:-18093}
BACKEND_PORT=${TCP_SHIFT_P2_RECOVERY_BACKEND_PORT:-19093}
PAYLOAD_BYTES=${TCP_SHIFT_P2_RECOVERY_PAYLOAD_BYTES:-65536}
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

# No backend is listening yet. The public TCP handshake may complete before the
# nonblocking loopback connect reports ECONNREFUSED; either way the first public
# flow must be torn down instead of taking down the process/listener.
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
        echo "P2 recovery runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for P2 recovery runtime" >&2
    exit 1
}

python3 - "$LWIP_IP" "$PUBLIC_PORT" > "$OUT/refused-client.txt" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
failed = False
try:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(2.0)
        sock.connect((host, port))
        try:
            sock.sendall(b"backend-must-not-exist")
            data = sock.recv(1)
            if data == b"":
                failed = True
        except (ConnectionError, OSError):
            failed = True
except (ConnectionError, OSError):
    failed = True
if not failed:
    raise SystemExit("first public flow did not fail after backend refusal")
print("backend-refusal-public-flow=closed")
PY

sleep 0.2
kill -0 "$PID"

python3 - "$BACKEND_PORT" > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
conn, peer = server.accept()
conn.settimeout(5.0)
chunks = []
while True:
    chunk = conn.recv(16384)
    if not chunk:
        break
    chunks.append(chunk)
payload = b"".join(chunks)
conn.sendall(payload)
conn.shutdown(socket.SHUT_WR)
conn.close()
server.close()
print(
    f"recovery-backend-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok",
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
        echo "recovery backend exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for recovery backend" >&2
    exit 1
}

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" > "$OUT/recovery-client.txt" <<'PY'
import hashlib
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
payload = bytes(((index * 29 + 23) & 0xFF) for index in range(length))
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
    raise SystemExit(
        f"recovery echo mismatch: sent={len(payload)} received={len(echo)}"
    )
print(
    f"recovery-client-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok"
)
PY

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stderr" >&2 || true
    echo "recovery backend failed" >&2
    exit 1
fi
BACKEND_PID=

sleep 0.2
kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P2 recovery runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/refused-client.txt"
cat "$OUT/recovery-client.txt"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F 'backend-refusal-public-flow=closed' "$OUT/refused-client.txt" >/dev/null
grep -F "recovery-client-bytes=$PAYLOAD_BYTES " "$OUT/recovery-client.txt" >/dev/null
grep -F ' echo=ok' "$OUT/recovery-client.txt" >/dev/null
grep -F "recovery-backend-bytes=$PAYLOAD_BYTES " "$OUT/backend.stdout" >/dev/null

grep -F "bridge_accepts=2 bridge_backend_connects=1 bridge_public_to_backend_bytes=$PAYLOAD_BYTES bridge_backend_to_public_bytes=$PAYLOAD_BYTES bridge_active_flows=0 bridge_peak_active_flows=1 bridge_backend_failures=1 bridge_public_errors=0" \
    "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "P2 recovery TUN leaked after process exit" >&2
    exit 1
fi

printf 'refused_flows=1 recovered_flows=1 payload_bytes=%u final_active_flows=0 listener_reuse=ok\n' \
    "$PAYLOAD_BYTES" | tee "$OUT/summary.txt"
echo "P2 backend-refusal recovery smoke passed"
