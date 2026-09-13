#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p1-ci"
BINARY=${TCP_SHIFT_P1_BINARY:-"$BUILD/tcp-shift-p1"}
TUN_NAME=${TCP_SHIFT_P1_TUN_NAME:-tsp1ci0}
LWIP_IP=${TCP_SHIFT_P1_LWIP_IP:-10.231.0.2}
HOST_CIDR=${TCP_SHIFT_P1_HOST_CIDR:-10.231.0.1/30}
NETMASK=${TCP_SHIFT_P1_NETMASK:-255.255.255.252}
GATEWAY=${TCP_SHIFT_P1_GATEWAY:-10.231.0.1}
PID=

mkdir -p "$OUT"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"

capture_state()
{
    ip -d addr show > "$OUT/ip-addr.txt" 2>&1 || true
    ip route show table all > "$OUT/ip-route.txt" 2>&1 || true
    ip -s link show > "$OUT/ip-link.txt" 2>&1 || true
}

cleanup()
{
    set +e
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
    capture_state
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        sudo ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

[ -x "$BINARY" ] || {
    echo "missing P1 binary: $BINARY" >&2
    exit 1
}
[ -c /dev/net/tun ] || {
    echo "/dev/net/tun is unavailable" >&2
    exit 1
}

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$GATEWAY" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "P1 runtime exited before creating TUN" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for $TUN_NAME" >&2
    exit 1
}

sudo ip link set dev "$TUN_NAME" mtu 1500 up
sudo ip addr add "$HOST_CIDR" dev "$TUN_NAME"
capture_state

ping -n -c 3 -W 1 "$LWIP_IP" | tee "$OUT/ping.txt"

kill -TERM "$PID"
wait "$PID"
PID=

cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "tcp-shift-p1: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null
grep -Eq 'rx_packets=[1-9][0-9]*' "$OUT/runtime.stderr"
grep -Eq 'tx_packets=[1-9][0-9]*' "$OUT/runtime.stderr"

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "nonpersistent TUN survived runtime exit" >&2
    exit 1
fi

echo "P1 IPv4 TUN smoke passed"
