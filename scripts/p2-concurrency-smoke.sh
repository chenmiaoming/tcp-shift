#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p2-concurrency-ci"
BINARY=${TCP_SHIFT_P2_BINARY:-"$BUILD/tcp-shift-p2"}
TUN_NAME=${TCP_SHIFT_P2_CONC_TUN_NAME:-tsp2conc0}
LWIP_IP=${TCP_SHIFT_P2_CONC_LWIP_IP:-10.238.0.2}
NETMASK=${TCP_SHIFT_P2_CONC_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P2_CONC_HOST_IP:-10.238.0.1}
PUBLIC_PORT=${TCP_SHIFT_P2_CONC_PUBLIC_PORT:-18096}
BACKEND_PORT=${TCP_SHIFT_P2_CONC_BACKEND_PORT:-19096}
FLOW_COUNT=${TCP_SHIFT_P2_CONC_FLOW_COUNT:-8}
PAYLOAD_BYTES=${TCP_SHIFT_P2_CONC_PAYLOAD_BYTES:-32768}
TOTAL_BYTES=$((FLOW_COUNT * PAYLOAD_BYTES))
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
[ "$PAYLOAD_BYTES" -ge 4 ] || {
    echo "concurrency payload must be at least 4 bytes" >&2
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

python3 - "$BACKEND_PORT" "$FLOW_COUNT" "$PAYLOAD_BYTES" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import socket
import sys

port = int(sys.argv[1])
count = int(sys.argv[2])
length = int(sys.argv[3])

def make_payload(index):
    return index.to_bytes(4, "big") + bytes(
        (((offset + index) * 43 + 17) & 0xFF) for offset in range(length - 4)
    )

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(count)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
connections = []
for _ in range(count):
    conn, _ = server.accept()
    conn.settimeout(10.0)
    connections.append(conn)
print(f"backend-accepted={len(connections)}", flush=True)
total = 0
seen = set()
for accept_slot, conn in enumerate(connections):
    chunks = []
    while True:
        chunk = conn.recv(16384)
        if not chunk:
            break
        chunks.append(chunk)
    payload = b"".join(chunks)
    if len(payload) != length:
        raise SystemExit(
            f"backend accept slot {accept_slot} length mismatch: expected={length} got={len(payload)}"
        )
    flow_id = int.from_bytes(payload[:4], "big")
    if flow_id < 0 or flow_id >= count:
        raise SystemExit(f"backend accept slot {accept_slot} invalid flow id {flow_id}")
    if flow_id in seen:
        raise SystemExit(f"backend duplicate flow id {flow_id}")
    expected = make_payload(flow_id)
    if payload != expected:
        raise SystemExit(
            f"backend flow id {flow_id} content mismatch: expected={length} got={len(payload)}"
        )
    seen.add(flow_id)
    conn.sendall(payload)
    conn.shutdown(socket.SHUT_WR)
    conn.close()
    total += len(payload)
server.close()
if seen != set(range(count)):
    raise SystemExit(f"backend flow id set mismatch: seen={sorted(seen)}")
print(f"backend-concurrent-total={total} flows={count} unique_flow_ids={len(seen)} echo=ok", flush=True)
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
        echo "concurrency backend exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for concurrency backend" >&2
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
        echo "P2 concurrency runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for P2 concurrency runtime" >&2
    exit 1
}

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$FLOW_COUNT" "$PAYLOAD_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import concurrent.futures
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
count = int(sys.argv[3])
length = int(sys.argv[4])

def make_payload(index):
    return index.to_bytes(4, "big") + bytes(
        (((offset + index) * 43 + 17) & 0xFF) for offset in range(length - 4)
    )

def run(index):
    payload = make_payload(index)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(10.0)
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
        raise RuntimeError(f"flow {index} echo mismatch: expected={length} got={len(echo)}")
    return len(payload)

with concurrent.futures.ThreadPoolExecutor(max_workers=count) as executor:
    lengths = list(executor.map(run, range(count)))
print(f"concurrent-client-flows={count} total_bytes={sum(lengths)} echo=ok")
PY

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    echo "concurrency backend failed" >&2
    exit 1
fi
BACKEND_PID=

sleep 0.2
kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P2 concurrency runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "concurrent-client-flows=$FLOW_COUNT total_bytes=$TOTAL_BYTES echo=ok" "$OUT/client.stdout" >/dev/null
grep -F "backend-accepted=$FLOW_COUNT" "$OUT/backend.stdout" >/dev/null
grep -F "backend-concurrent-total=$TOTAL_BYTES flows=$FLOW_COUNT unique_flow_ids=$FLOW_COUNT echo=ok" "$OUT/backend.stdout" >/dev/null

grep -F "bridge_accepts=$FLOW_COUNT bridge_backend_connects=$FLOW_COUNT bridge_public_to_backend_bytes=$TOTAL_BYTES bridge_backend_to_public_bytes=$TOTAL_BYTES bridge_active_flows=0 bridge_peak_active_flows=$FLOW_COUNT bridge_backend_failures=0 bridge_public_errors=0" \
    "$OUT/runtime.stderr" >/dev/null
grep -F 'bridge_pending_public_bytes=0' "$OUT/runtime.stderr" >/dev/null

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "P2 concurrency TUN leaked after process exit" >&2
    exit 1
fi

printf 'flows=%u payload_per_flow=%u total_bytes=%u peak_active_flows=%u final_active_flows=0 concurrent_integrity=ok\n' \
    "$FLOW_COUNT" "$PAYLOAD_BYTES" "$TOTAL_BYTES" "$FLOW_COUNT" | tee "$OUT/summary.txt"
echo "P2 concurrent-flow smoke passed"
