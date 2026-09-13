#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p1-ipv6-ci"
BINARY=${TCP_SHIFT_P1_IPV6_BINARY:-"$BUILD/tcp-shift-p1-ipv6"}
TUN_NAME=${TCP_SHIFT_P1_IPV6_TUN_NAME:-tsp1v60}
LWIP_IP=${TCP_SHIFT_P1_IPV6_LWIP_IP:-2001:db8:231::2}
HOST_CIDR=${TCP_SHIFT_P1_IPV6_HOST_CIDR:-2001:db8:231::1/126}
TCP_PORT=${TCP_SHIFT_P1_IPV6_TCP_PORT:-18082}
PID=

mkdir -p "$OUT"

[ "$(id -u)" -eq 0 ] || {
    echo "p1-ipv6-smoke.sh must run as root" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing IPv6 P1 binary: $BINARY" >&2
    exit 1
}
[ -c /dev/net/tun ] || {
    echo "/dev/net/tun is unavailable" >&2
    exit 1
}

capture_state()
{
    ip -6 -d addr show > "$OUT/ip6-addr.txt" 2>&1 || true
    ip -6 route show table all > "$OUT/ip6-route.txt" 2>&1 || true
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip -s link show "$TUN_NAME" > "$OUT/tun-link.txt" 2>&1 || true
    fi
}

cleanup()
{
    set +e
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID"
        wait "$PID" >/dev/null 2>&1 || true
    fi
    capture_state
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
"$BINARY" "$TUN_NAME" "$LWIP_IP" "$HOST_CIDR" "$TCP_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       ip -6 addr show dev "$TUN_NAME" | grep -F "$HOST_CIDR" >/dev/null 2>&1 &&
       grep -F "lwip-ipv6=$LWIP_IP" "$OUT/runtime.stdout" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "IPv6 runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    capture_state
    cat "$OUT/runtime.stderr" >&2 || true
    echo "timed out waiting for IPv6 TUN runtime" >&2
    exit 1
}

ping -6 -c 3 -W 1 "$LWIP_IP" | tee "$OUT/ping6.txt"
grep -F '3 packets transmitted, 3 received' "$OUT/ping6.txt" >/dev/null

# IPv6 header (40) + ICMPv6 header (8) + payload. 1452 reaches MTU 1500;
# 1453 with PMTU discovery forced must be rejected locally as 1501 bytes.
ping -6 -c 1 -W 1 -M do -s 1452 "$LWIP_IP" | tee "$OUT/mtu1500.txt"
set +e
ping -6 -c 1 -W 1 -M do -s 1453 "$LWIP_IP" > "$OUT/mtu1501.txt" 2>&1
MTU_RC=$?
set -e
[ "$MTU_RC" -ne 0 ] || {
    cat "$OUT/mtu1501.txt" >&2
    echo "1501-byte IPv6 packet unexpectedly succeeded across MTU 1500" >&2
    exit 1
}
cat "$OUT/mtu1501.txt"

python3 - "$LWIP_IP" "$TCP_PORT" > "$OUT/tcp6-connect.txt" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as sock:
    sock.settimeout(2.0)
    sock.connect((host, port, 0, 0))
print(f"IPv6 connected [{host}]:{port}")
PY
cat "$OUT/tcp6-connect.txt"

sleep 0.1
kill -TERM "$PID"
wait "$PID"
PID=
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -E 'tcp_accepts=[1-9][0-9]*' "$OUT/runtime.stderr" >/dev/null
grep -F 'tcp_errors=0' "$OUT/runtime.stderr" >/dev/null

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "IPv6 TUN leaked after runtime exit" >&2
    exit 1
fi

capture_state
echo "P1b direct IPv6 ICMP/TCP/MTU smoke passed"
