#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
FIREWALL_BACKEND=${TCP_SHIFT_SERVICE_FIREWALL_BACKEND:-nftables}
OUT="$BUILD/service-config-ci-$FIREWALL_BACKEND"
BINARY=${TCP_SHIFT_SERVICE_BINARY:-"$BUILD/tcp-shift"}
TUN_NAME=${TCP_SHIFT_SERVICE_TUN_NAME:-tssvcci0}
LWIP_IP=${TCP_SHIFT_SERVICE_LWIP_IP:-10.234.0.2}
HOST_CIDR=${TCP_SHIFT_SERVICE_HOST_CIDR:-10.234.0.1/30}
PUBLIC_PORT=${TCP_SHIFT_SERVICE_PUBLIC_PORT:-18092}
BACKEND_PORT=${TCP_SHIFT_SERVICE_BACKEND_PORT:-19092}
NS_NAME=${TCP_SHIFT_SERVICE_NS_NAME:-tssvcns}
WAN_HOST_IF=${TCP_SHIFT_SERVICE_WAN_HOST_IF:-tssvch0}
WAN_NS_IF=${TCP_SHIFT_SERVICE_WAN_NS_IF:-tssvcn0}
WAN_HOST_IP=${TCP_SHIFT_SERVICE_WAN_HOST_IP:-198.51.102.1}
WAN_HOST_CIDR=${TCP_SHIFT_SERVICE_WAN_HOST_CIDR:-198.51.102.1/24}
WAN_CLIENT_IP=${TCP_SHIFT_SERVICE_WAN_CLIENT_IP:-198.51.102.2}
WAN_CLIENT_CIDR=${TCP_SHIFT_SERVICE_WAN_CLIENT_CIDR:-198.51.102.2/24}
PRODUCT_TABLE=tcp_shift_p2
PAYLOAD_BYTES=${TCP_SHIFT_SERVICE_PAYLOAD_BYTES:-65536}
IPTABLES_LEGACY=`command -v iptables-legacy 2>/dev/null || true`
PID=
BACKEND_PID=
OLD_FORWARD=
FORWARD_RULES=0

mkdir -p "$OUT"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"

[ "$(id -u)" -eq 0 ] || {
    echo "service-config-smoke.sh must run as root" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing tcp-shift service binary: $BINARY" >&2
    exit 1
}
[ -x "$BUILD/tcp-shift-p2" ] || {
    echo "missing sibling tcp-shift-p2 runtime" >&2
    exit 1
}
case "$FIREWALL_BACKEND" in
    nftables)
        command -v nft >/dev/null 2>&1 || {
            echo "nft is unavailable for test introspection" >&2
            exit 1
        }
        ;;
    iptables)
        [ -n "$IPTABLES_LEGACY" ] || {
            echo "iptables-legacy is unavailable" >&2
            exit 1
        }
        ;;
    *)
        echo "service-config-smoke requires nftables or iptables backend" >&2
        exit 1
        ;;
esac

firewall_live()
{
    case "$FIREWALL_BACKEND" in
        nftables)
            nft list table ip "$PRODUCT_TABLE" >/dev/null 2>&1
            ;;
        iptables)
            "$IPTABLES_LEGACY" -w -t nat -S "$PRODUCT_TABLE" >/dev/null 2>&1
            ;;
    esac
}

dump_firewall()
{
    case "$FIREWALL_BACKEND" in
        nftables)
            nft list table ip "$PRODUCT_TABLE"
            ;;
        iptables)
            "$IPTABLES_LEGACY" -w -t nat -S PREROUTING
            "$IPTABLES_LEGACY" -w -t nat -S "$PRODUCT_TABLE"
            ;;
    esac
}

remove_firewall_emergency()
{
    case "$FIREWALL_BACKEND" in
        nftables)
            nft delete table ip "$PRODUCT_TABLE" >/dev/null 2>&1 || true
            ;;
        iptables)
            "$IPTABLES_LEGACY" -w -t nat -D PREROUTING \
                -d "$WAN_HOST_IP" -p tcp --dport "$PUBLIC_PORT" \
                -j "$PRODUCT_TABLE" >/dev/null 2>&1 || true
            "$IPTABLES_LEGACY" -w -t nat -F "$PRODUCT_TABLE" \
                >/dev/null 2>&1 || true
            "$IPTABLES_LEGACY" -w -t nat -X "$PRODUCT_TABLE" \
                >/dev/null 2>&1 || true
            ;;
    esac
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

remove_forward_rules()
{
    if [ "$FORWARD_RULES" -eq 1 ]; then
        iptables -w -D FORWARD -i "$WAN_HOST_IF" -o "$TUN_NAME" \
            -p tcp -d "$LWIP_IP" --dport "$PUBLIC_PORT" -j ACCEPT \
            >/dev/null 2>&1 || true
        iptables -w -D FORWARD -i "$TUN_NAME" -o "$WAN_HOST_IF" \
            -p tcp -s "$LWIP_IP" --sport "$PUBLIC_PORT" -j ACCEPT \
            >/dev/null 2>&1 || true
        FORWARD_RULES=0
    fi
}

cleanup()
{
    set +e
    stop_runtime
    stop_backend
    remove_forward_rules
    if firewall_live; then
        remove_firewall_emergency
    fi
    if ip netns list | grep -F "$NS_NAME" >/dev/null 2>&1; then
        ip netns del "$NS_NAME" >/dev/null 2>&1 || true
    fi
    if ip link show "$WAN_HOST_IF" >/dev/null 2>&1; then
        ip link del "$WAN_HOST_IF" >/dev/null 2>&1 || true
    fi
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
    if [ -n "${OLD_FORWARD:-}" ]; then
        sysctl -q -w net.ipv4.ip_forward="$OLD_FORWARD" >/dev/null 2>&1 || true
        OLD_FORWARD=
    fi
}
trap cleanup EXIT HUP INT TERM

cat > "$OUT/tcp-shift.toml" <<EOF
version = 1
cc = "reno"
firewall_backend = "$FIREWALL_BACKEND"
tun_name = "$TUN_NAME"
tun_host_address = "$HOST_CIDR"
tun_guest_address = "$LWIP_IP"

[[forward]]
listen = "$WAN_HOST_IP:$PUBLIC_PORT"
backend = "127.0.0.1:$BACKEND_PORT"
EOF

"$BINARY" --config "$OUT/tcp-shift.toml" --check > "$OUT/check.stdout"
grep -F "configuration ok version=1 family=ipv4 cc=reno firewall=$FIREWALL_BACKEND" "$OUT/check.stdout" >/dev/null
if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "--check unexpectedly created the TUN" >&2
    exit 1
fi
if firewall_live; then
    echo "--check unexpectedly created firewall state" >&2
    exit 1
fi

OLD_FORWARD=$(sysctl -n net.ipv4.ip_forward)
sysctl -q -w net.ipv4.ip_forward=1

ip netns add "$NS_NAME"
ip link add "$WAN_HOST_IF" type veth peer name "$WAN_NS_IF"
ip link set "$WAN_NS_IF" netns "$NS_NAME"
ip addr add "$WAN_HOST_CIDR" dev "$WAN_HOST_IF"
ip link set "$WAN_HOST_IF" up
ip -n "$NS_NAME" link set lo up
ip -n "$NS_NAME" addr add "$WAN_CLIENT_CIDR" dev "$WAN_NS_IF"
ip -n "$NS_NAME" link set "$WAN_NS_IF" up
ip -n "$NS_NAME" route add default via "$WAN_HOST_IP"

python3 - "$BACKEND_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
expected = int(sys.argv[2])
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
conn, _ = server.accept()
conn.settimeout(5.0)
chunks = []
while True:
    chunk = conn.recv(16384)
    if not chunk:
        break
    chunks.append(chunk)
payload = b"".join(chunks)
if len(payload) != expected:
    raise SystemExit(f"backend payload length {len(payload)} != {expected}")
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

i=0
while [ "$i" -lt 100 ]; do
    grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1 && break
    if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
        cat "$OUT/backend.stderr" >&2 || true
        echo "backend exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null

"$BINARY" --config "$OUT/tcp-shift.toml" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

i=0
ready=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       firewall_live &&
       grep -F "tcp-shift-p2: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null 2>&1 &&
       grep -F "firewall=$FIREWALL_BACKEND firewall-resource=$PRODUCT_TABLE" "$OUT/runtime.stdout" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "config-driven runtime exited before ready" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for config-driven runtime" >&2
    exit 1
}

dump_firewall > "$OUT/product-firewall-live.txt"
case "$FIREWALL_BACKEND" in
    nftables)
        grep -F "ip daddr $WAN_HOST_IP tcp dport $PUBLIC_PORT" \
            "$OUT/product-firewall-live.txt" >/dev/null
        grep -F "dnat to $LWIP_IP:$PUBLIC_PORT" \
            "$OUT/product-firewall-live.txt" >/dev/null
        ;;
    iptables)
        grep -F -- "-j $PRODUCT_TABLE" "$OUT/product-firewall-live.txt" >/dev/null
        grep -F -- "--dport $PUBLIC_PORT" "$OUT/product-firewall-live.txt" >/dev/null
        grep -F -- "-j DNAT --to-destination $LWIP_IP:$PUBLIC_PORT" \
            "$OUT/product-firewall-live.txt" >/dev/null
        ;;
esac

iptables -w -I FORWARD 1 -i "$WAN_HOST_IF" -o "$TUN_NAME" \
    -p tcp -d "$LWIP_IP" --dport "$PUBLIC_PORT" -j ACCEPT
iptables -w -I FORWARD 1 -i "$TUN_NAME" -o "$WAN_HOST_IF" \
    -p tcp -s "$LWIP_IP" --sport "$PUBLIC_PORT" -j ACCEPT
FORWARD_RULES=1

ip netns exec "$NS_NAME" python3 - "$WAN_HOST_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import hashlib
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
payload = bytes(((index * 29 + 11) & 0xff) for index in range(length))
with socket.create_connection((host, port), timeout=5.0) as sock:
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
        f"echo mismatch sent={len(payload)} received={len(echo)} "
        f"sent_sha={hashlib.sha256(payload).hexdigest()} "
        f"received_sha={hashlib.sha256(echo).hexdigest()}"
    )
print(
    f"config-client-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok"
)
PY

wait "$BACKEND_PID"
BACKEND_PID=
sleep 0.2
stop_runtime

cat "$OUT/check.stdout"
cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "config-client-bytes=$PAYLOAD_BYTES " "$OUT/client.stdout" >/dev/null
grep -F "backend-bytes=$PAYLOAD_BYTES " "$OUT/backend.stdout" >/dev/null
grep -F "public-ipv4=$WAN_HOST_IP" "$OUT/runtime.stdout" >/dev/null
grep -F "cc=reno firewall=$FIREWALL_BACKEND firewall-resource=$PRODUCT_TABLE" "$OUT/runtime.stdout" >/dev/null
grep -F "bridge_accepts=1 bridge_backend_connects=1" "$OUT/runtime.stderr" >/dev/null
grep -F "bridge_public_to_backend_bytes=$PAYLOAD_BYTES" "$OUT/runtime.stderr" >/dev/null
grep -F "bridge_backend_to_public_bytes=$PAYLOAD_BYTES" "$OUT/runtime.stderr" >/dev/null

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "config-driven TUN leaked after shutdown" >&2
    exit 1
fi
if firewall_live; then
    echo "config-driven firewall state leaked after shutdown" >&2
    dump_firewall >&2 || true
    exit 1
fi

printf 'config_check_no_mutation=ok firewall=%s public_dnat=ok payload_bytes=%u echo=ok cleanup=ok\n' \
    "$FIREWALL_BACKEND" "$PAYLOAD_BYTES" | tee "$OUT/summary.txt"
echo "Config-driven service ingress smoke passed"
