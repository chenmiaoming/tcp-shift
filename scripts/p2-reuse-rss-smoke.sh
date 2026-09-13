#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p2-reuse-rss-ci"
BINARY=${TCP_SHIFT_P2_BINARY:-"$BUILD/tcp-shift-p2"}
TUN_NAME=${TCP_SHIFT_P2_REUSE_TUN_NAME:-tsp2reuse0}
LWIP_IP=${TCP_SHIFT_P2_REUSE_LWIP_IP:-10.240.0.2}
NETMASK=${TCP_SHIFT_P2_REUSE_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P2_REUSE_HOST_IP:-10.240.0.1}
PUBLIC_PORT=${TCP_SHIFT_P2_REUSE_PUBLIC_PORT:-18098}
BACKEND_PORT=${TCP_SHIFT_P2_REUSE_BACKEND_PORT:-19098}
TOTAL_FLOWS=${TCP_SHIFT_P2_REUSE_FLOWS:-64}
PAYLOAD_BYTES=${TCP_SHIFT_P2_REUSE_PAYLOAD_BYTES:-4096}
RSS_ALLOWANCE_KB=${TCP_SHIFT_P2_REUSE_RSS_ALLOWANCE_KB:-1024}
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
[ "$TOTAL_FLOWS" -ge 64 ] || {
    echo "reuse gate requires at least 64 flows" >&2
    exit 1
}

cleanup()
{
    set +e
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" >/dev/null 2>&1 || true
        wait "$PID" >/dev/null 2>&1 || true
    fi
    if [ -n "${BACKEND_PID:-}" ] && kill -0 "$BACKEND_PID" 2>/dev/null; then
        kill -TERM "$BACKEND_PID" >/dev/null 2>&1 || true
        wait "$BACKEND_PID" >/dev/null 2>&1 || true
    fi
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        sudo ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

rss_kb()
{
    awk '/^VmRSS:/ { print $2; exit }' "/proc/$PID/status"
}

run_clients()
{
    count=$1
    offset=$2
    python3 - "$LWIP_IP" "$PUBLIC_PORT" "$count" "$offset" "$PAYLOAD_BYTES" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
count = int(sys.argv[3])
offset = int(sys.argv[4])
length = int(sys.argv[5])
for index in range(offset, offset + count):
    payload = bytes((((byte + index) * 47 + 31) & 0xFF) for byte in range(length))
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(5.0)
        sock.connect((host, port))
        sock.sendall(payload)
        sock.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            chunks.append(chunk)
    if b"".join(chunks) != payload:
        raise SystemExit(f"reuse flow {index} echo mismatch")
print(f"reuse-client-offset={offset} flows={count} bytes_per_flow={length} ok")
PY
}

python3 - "$BACKEND_PORT" "$TOTAL_FLOWS" "$PAYLOAD_BYTES" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import socket
import sys

port = int(sys.argv[1])
count = int(sys.argv[2])
length = int(sys.argv[3])
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(16)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
total = 0
for index in range(count):
    conn, _ = server.accept()
    conn.settimeout(5.0)
    chunks = []
    while True:
        chunk = conn.recv(8192)
        if not chunk:
            break
        chunks.append(chunk)
    payload = b"".join(chunks)
    expected = bytes((((byte + index) * 47 + 31) & 0xFF) for byte in range(length))
    if payload != expected:
        raise SystemExit(f"backend reuse flow {index} mismatch")
    conn.sendall(payload)
    conn.shutdown(socket.SHUT_WR)
    conn.close()
    total += len(payload)
server.close()
print(f"backend-reuse-flows={count} total_bytes={total} echo=ok", flush=True)
PY
BACKEND_PID=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; then
        ready=1
        break
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    cat "$OUT/backend.stderr" >&2 || true
    echo "timed out waiting for reuse backend" >&2
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
        echo "P2 reuse runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for P2 reuse runtime" >&2
    exit 1
}

run_clients 8 0 > "$OUT/client-warmup.txt"
sleep 0.1
RSS_WARMUP=$(rss_kb)
[ -n "$RSS_WARMUP" ] || {
    echo "failed to sample warmup VmRSS" >&2
    exit 1
}

run_clients 24 8 > "$OUT/client-mid.txt"
sleep 0.1
RSS_MID=$(rss_kb)
[ -n "$RSS_MID" ] || {
    echo "failed to sample mid VmRSS" >&2
    exit 1
}

run_clients 32 32 > "$OUT/client-final.txt"
sleep 0.1
RSS_FINAL=$(rss_kb)
[ -n "$RSS_FINAL" ] || {
    echo "failed to sample final VmRSS" >&2
    exit 1
}

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    echo "reuse backend failed" >&2
    exit 1
fi
BACKEND_PID=

[ "$RSS_MID" -le $((RSS_WARMUP + RSS_ALLOWANCE_KB)) ] || {
    echo "P2 RSS ratcheted after 32 flows: warmup=${RSS_WARMUP}kB mid=${RSS_MID}kB" >&2
    exit 1
}
[ "$RSS_FINAL" -le $((RSS_WARMUP + RSS_ALLOWANCE_KB)) ] || {
    echo "P2 RSS ratcheted after 64 flows: warmup=${RSS_WARMUP}kB final=${RSS_FINAL}kB" >&2
    exit 1
}

kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P2 reuse runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/client-warmup.txt"
cat "$OUT/client-mid.txt"
cat "$OUT/client-final.txt"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

TOTAL_BYTES=$((TOTAL_FLOWS * PAYLOAD_BYTES))
grep -F "backend-reuse-flows=$TOTAL_FLOWS total_bytes=$TOTAL_BYTES echo=ok" "$OUT/backend.stdout" >/dev/null
grep -F "bridge_accepts=$TOTAL_FLOWS bridge_backend_connects=$TOTAL_FLOWS bridge_public_to_backend_bytes=$TOTAL_BYTES bridge_backend_to_public_bytes=$TOTAL_BYTES bridge_active_flows=0" \
    "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'shutdown_bridge_active_flows=0 shutdown_bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "P2 reuse TUN leaked after process exit" >&2
    exit 1
fi

printf 'flows=%u bytes_per_flow=%u rss_warmup_kb=%u rss_mid_kb=%u rss_final_kb=%u rss_allowance_kb=%u bridge_reuse_no_ratcheting=ok\n' \
    "$TOTAL_FLOWS" "$PAYLOAD_BYTES" "$RSS_WARMUP" "$RSS_MID" "$RSS_FINAL" "$RSS_ALLOWANCE_KB" \
    | tee "$OUT/summary.txt"
echo "P2 repeated flow reuse/RSS smoke passed"
