#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/window-scaling-contract"
BINARY=${TCP_SHIFT_WINDOW_SCALE_BINARY:-"$BUILD/tcp-shift-p1"}
TUN_NAME=${TCP_SHIFT_WINDOW_SCALE_TUN:-"tsws$$"}
LWIP_IP=${TCP_SHIFT_WINDOW_SCALE_LWIP_IP:-10.246.0.2}
HOST_IP=${TCP_SHIFT_WINDOW_SCALE_HOST_IP:-10.246.0.1}
NETMASK=${TCP_SHIFT_WINDOW_SCALE_NETMASK:-255.255.255.252}
TCP_PORT=${TCP_SHIFT_WINDOW_SCALE_PORT:-18880}
PID=
TCPDUMP_PID=

mkdir -p "$OUT"

cleanup()
{
    set +e
    if [ -n "${TCPDUMP_PID:-}" ] && kill -0 "$TCPDUMP_PID" 2>/dev/null; then
        kill -TERM "$TCPDUMP_PID" >/dev/null 2>&1 || true
        wait "$TCPDUMP_PID" >/dev/null 2>&1 || true
    fi
    TCPDUMP_PID=
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" >/dev/null 2>&1 || true
        wait "$PID" >/dev/null 2>&1 || true
    fi
    PID=
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

[ "$(id -u)" -eq 0 ] || {
    echo "window-scaling contract requires root for TUN setup" >&2
    exit 1
}
[ -x "$BINARY" ] || { echo "missing P1 binary: $BINARY" >&2; exit 1; }
command -v tcpdump >/dev/null 2>&1 || { echo "tcpdump is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 1; }

: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" "$TCP_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p1: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "window-scaling runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$i" -lt 100 ] || { echo "window-scaling runtime ready timeout" >&2; exit 1; }

# Capture exactly the SYN and SYN-ACK. The Linux client should offer a nonzero
# receive scale; tcp-shift deliberately responds with wscale 0 in this first
# milestone so sender-side 32-bit window accounting is enabled without growing
# the local receive-window residency.
timeout 5 tcpdump -U -s 0 -i "$TUN_NAME" -c 2 -nn -vvv \
    "tcp and host $LWIP_IP and port $TCP_PORT" \
    > "$OUT/handshake.txt" 2>&1 &
TCPDUMP_PID=$!
sleep 0.15

python3 - "$LWIP_IP" "$TCP_PORT" > "$OUT/client.txt" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.create_connection((host, port), timeout=3.0):
    pass
print(f"window-scale client connected {host}:{port}")
PY

wait "$TCPDUMP_PID"
TCPDUMP_PID=
cat "$OUT/handshake.txt"

grep -E "IP .* > ${LWIP_IP}\\.${TCP_PORT}: Flags \\[S\\].*wscale [0-9]+" \
    "$OUT/handshake.txt" >/dev/null || {
    echo "client SYN did not advertise window scaling" >&2
    exit 1
}
grep -E "IP ${LWIP_IP}\\.${TCP_PORT} > .* Flags \\[S\\.\\].*wscale 0" \
    "$OUT/handshake.txt" >/dev/null || {
    echo "lwIP SYN-ACK did not advertise the qualified wscale 0 profile" >&2
    exit 1
}

kill -TERM "$PID"
wait "$PID"
PID=

grep -F 'rx_errors=0' "$OUT/runtime.stderr" >/dev/null || {
    cat "$OUT/runtime.stderr" >&2
    echo "runtime reported receive errors during window-scale negotiation" >&2
    exit 1
}

printf 'window_scaling_contract=ok peer_scale=present local_scale=0 patch_surface=unchanged\n' \
    | tee "$OUT/summary.txt"
