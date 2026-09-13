#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p2-active-shutdown-ci"
BINARY=${TCP_SHIFT_P2_BINARY:-"$BUILD/tcp-shift-p2"}
TUN_NAME=${TCP_SHIFT_P2_SHUTDOWN_TUN_NAME:-tsp2sd0}
LWIP_IP=${TCP_SHIFT_P2_SHUTDOWN_LWIP_IP:-10.239.0.2}
NETMASK=${TCP_SHIFT_P2_SHUTDOWN_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P2_SHUTDOWN_HOST_IP:-10.239.0.1}
PUBLIC_PORT=${TCP_SHIFT_P2_SHUTDOWN_PUBLIC_PORT:-18097}
BACKEND_PORT=${TCP_SHIFT_P2_SHUTDOWN_BACKEND_PORT:-19097}
PID=
BACKEND_PID=
CLIENT_PID=

mkdir -p "$OUT"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"

[ -x "$BINARY" ] || {
    echo "missing P2 binary: $BINARY" >&2
    exit 1
}

cleanup()
{
    set +e
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" >/dev/null 2>&1 || true
        wait "$PID" >/dev/null 2>&1 || true
    fi
    if [ -n "${CLIENT_PID:-}" ] && kill -0 "$CLIENT_PID" 2>/dev/null; then
        kill -TERM "$CLIENT_PID" >/dev/null 2>&1 || true
        wait "$CLIENT_PID" >/dev/null 2>&1 || true
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

python3 - "$BACKEND_PORT" > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import socket
import sys

port = int(sys.argv[1])
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
conn, peer = server.accept()
conn.settimeout(8.0)
print("backend-active-flow=accepted", flush=True)
try:
    data = conn.recv(1)
    if data == b"":
        print("backend-observed-runtime-close=ok", flush=True)
    else:
        print(f"backend-unexpected-data={len(data)}", flush=True)
except (ConnectionError, OSError):
    print("backend-observed-runtime-close=ok", flush=True)
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
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    cat "$OUT/backend.stderr" >&2 || true
    echo "timed out waiting for shutdown backend" >&2
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
        echo "P2 shutdown runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for P2 shutdown runtime" >&2
    exit 1
}

python3 - "$LWIP_IP" "$PUBLIC_PORT" > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY' &
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(8.0)
    sock.connect((host, port))
    print("client-active-flow=connected", flush=True)
    try:
        data = sock.recv(1)
        if data == b"":
            print("client-observed-runtime-close=ok", flush=True)
        else:
            print(f"client-unexpected-data={len(data)}", flush=True)
    except (ConnectionError, OSError):
        print("client-observed-runtime-close=ok", flush=True)
PY
CLIENT_PID=$!

active=0
i=0
while [ "$i" -lt 100 ]; do
    if grep -F 'backend-active-flow=accepted' "$OUT/backend.stdout" >/dev/null 2>&1 &&
       grep -F 'client-active-flow=connected' "$OUT/client.stdout" >/dev/null 2>&1; then
        active=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "runtime exited before active shutdown" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.02
done
[ "$active" -eq 1 ] || {
    echo "timed out establishing active bridge flow" >&2
    exit 1
}

kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P2 active-shutdown runtime failed" >&2
    exit 1
fi
PID=

wait "$CLIENT_PID"
CLIENT_PID=
wait "$BACKEND_PID"
BACKEND_PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F 'client-observed-runtime-close=ok' "$OUT/client.stdout" >/dev/null
grep -F 'backend-observed-runtime-close=ok' "$OUT/backend.stdout" >/dev/null
# The first line is sampled before bridge_stop and must prove an actually-live
# bridge object existed. The second line is emitted immediately after bridge_stop.
grep -F 'bridge_accepts=1 bridge_backend_connects=1 bridge_public_to_backend_bytes=0 bridge_backend_to_public_bytes=0 bridge_active_flows=1 bridge_peak_active_flows=1' \
    "$OUT/runtime.stderr" >/dev/null
grep -F 'shutdown_bridge_active_flows=0 shutdown_bridge_pending_public_bytes=0' \
    "$OUT/runtime.stderr" >/dev/null

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "P2 active-shutdown TUN leaked after process exit" >&2
    exit 1
fi

printf 'pre_stop_active_flows=1 post_stop_active_flows=0 client_close=ok backend_close=ok tun_cleanup=ok\n' \
    | tee "$OUT/summary.txt"
echo "P2 active-flow shutdown cleanup smoke passed"
