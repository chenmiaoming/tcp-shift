#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p1-ci"
BINARY=${TCP_SHIFT_P1_BINARY:-"$BUILD/tcp-shift-p1"}
TUN_NAME=${TCP_SHIFT_P1_TUN_NAME:-tsp1ci0}
LWIP_IP=${TCP_SHIFT_P1_LWIP_IP:-10.231.0.2}
HOST_IP=${TCP_SHIFT_P1_HOST_IP:-10.231.0.1}
HOST_CIDR=${TCP_SHIFT_P1_HOST_CIDR:-10.231.0.1/30}
NETMASK=${TCP_SHIFT_P1_NETMASK:-255.255.255.252}
TCP_PORT=${TCP_SHIFT_P1_TCP_PORT:-18080}
NS_NAME=${TCP_SHIFT_P1_NS_NAME:-tsp1cins}
WAN_HOST_IF=${TCP_SHIFT_P1_WAN_HOST_IF:-tsp1wan0}
WAN_NS_IF=${TCP_SHIFT_P1_WAN_NS_IF:-tsp1wan1}
WAN_HOST_IP=${TCP_SHIFT_P1_WAN_HOST_IP:-198.51.100.1}
WAN_HOST_CIDR=${TCP_SHIFT_P1_WAN_HOST_CIDR:-198.51.100.1/24}
WAN_CLIENT_IP=${TCP_SHIFT_P1_WAN_CLIENT_IP:-198.51.100.2}
WAN_CLIENT_CIDR=${TCP_SHIFT_P1_WAN_CLIENT_CIDR:-198.51.100.2/24}
NFT_TABLE=${TCP_SHIFT_P1_NFT_TABLE:-tcp_shift_p1_ci}
PID=
OLD_FORWARD=
FORWARD_RULES=0

mkdir -p "$OUT"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"

capture_state()
{
    ip -d addr show > "$OUT/ip-addr.txt" 2>&1 || true
    ip route show table all > "$OUT/ip-route.txt" 2>&1 || true
    ip -s link show > "$OUT/ip-link.txt" 2>&1 || true
    sudo nft list ruleset > "$OUT/nft-ruleset.txt" 2>&1 || true
    sudo iptables-save > "$OUT/iptables-save.txt" 2>&1 || true
    sudo conntrack -L -p tcp > "$OUT/conntrack-tcp.txt" 2>&1 || true
    if sudo ip netns list | grep -F "$NS_NAME" >/dev/null 2>&1; then
        sudo ip -n "$NS_NAME" -d addr show > "$OUT/netns-addr.txt" 2>&1 || true
        sudo ip -n "$NS_NAME" route show table all > "$OUT/netns-route.txt" 2>&1 || true
    fi
}

remove_forward_rules()
{
    if [ "$FORWARD_RULES" -eq 1 ]; then
        sudo iptables -w -D FORWARD -i "$WAN_HOST_IF" -o "$TUN_NAME" \
            -p tcp -d "$LWIP_IP" --dport "$TCP_PORT" -j ACCEPT \
            >/dev/null 2>&1 || true
        sudo iptables -w -D FORWARD -i "$TUN_NAME" -o "$WAN_HOST_IF" \
            -p tcp -s "$LWIP_IP" --sport "$TCP_PORT" -j ACCEPT \
            >/dev/null 2>&1 || true
        FORWARD_RULES=0
    fi
}

cleanup_network()
{
    set +e
    remove_forward_rules
    if sudo nft list table ip "$NFT_TABLE" >/dev/null 2>&1; then
        sudo nft delete table ip "$NFT_TABLE" >/dev/null 2>&1 || true
    fi
    if sudo ip netns list | grep -F "$NS_NAME" >/dev/null 2>&1; then
        sudo ip netns del "$NS_NAME" >/dev/null 2>&1 || true
    fi
    if ip link show "$WAN_HOST_IF" >/dev/null 2>&1; then
        sudo ip link del "$WAN_HOST_IF" >/dev/null 2>&1 || true
    fi
    if [ -n "${OLD_FORWARD:-}" ]; then
        sudo sysctl -q -w net.ipv4.ip_forward="$OLD_FORWARD" >/dev/null 2>&1 || true
        OLD_FORWARD=
    fi
    set -e
}

cleanup()
{
    set +e
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
    capture_state
    cleanup_network
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

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" "$TCP_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       ip -4 addr show dev "$TUN_NAME" | grep -F "$HOST_CIDR" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p1: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "P1 runtime exited before configuring TUN" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || {
    echo "timed out waiting for product-owned TUN configuration" >&2
    exit 1
}

capture_state
ping -n -c 3 -W 1 "$LWIP_IP" | tee "$OUT/ping.txt"

python3 - "$LWIP_IP" "$TCP_PORT" > "$OUT/tcp-direct-connect.txt" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.create_connection((host, port), timeout=2.0):
    pass
print(f"direct connected {host}:{port}")
PY
cat "$OUT/tcp-direct-connect.txt"

OLD_FORWARD=$(sysctl -n net.ipv4.ip_forward)
sudo sysctl -q -w net.ipv4.ip_forward=1
sudo ip netns add "$NS_NAME"
sudo ip link add "$WAN_HOST_IF" type veth peer name "$WAN_NS_IF"
sudo ip link set "$WAN_NS_IF" netns "$NS_NAME"
sudo ip addr add "$WAN_HOST_CIDR" dev "$WAN_HOST_IF"
sudo ip link set "$WAN_HOST_IF" up
sudo ip -n "$NS_NAME" link set lo up
sudo ip -n "$NS_NAME" addr add "$WAN_CLIENT_CIDR" dev "$WAN_NS_IF"
sudo ip -n "$NS_NAME" link set "$WAN_NS_IF" up
sudo ip -n "$NS_NAME" route add default via "$WAN_HOST_IP"

sudo nft -f - <<EOF
table ip $NFT_TABLE {
    chain prerouting {
        type nat hook prerouting priority -100; policy accept;
        iifname "$WAN_HOST_IF" ip daddr $WAN_HOST_IP tcp dport $TCP_PORT dnat to $LWIP_IP:$TCP_PORT
    }
}
EOF

# GitHub-hosted runners carry Docker's iptables-nft FORWARD policy=DROP.
# Insert only the two exact directions needed by this isolated test and remove
# them before exit. This is a harness prerequisite, not product firewall code.
sudo iptables -w -I FORWARD 1 -i "$WAN_HOST_IF" -o "$TUN_NAME" \
    -p tcp -d "$LWIP_IP" --dport "$TCP_PORT" -j ACCEPT
sudo iptables -w -I FORWARD 1 -i "$TUN_NAME" -o "$WAN_HOST_IF" \
    -p tcp -s "$LWIP_IP" --sport "$TCP_PORT" -j ACCEPT
FORWARD_RULES=1

sudo nft list table ip "$NFT_TABLE" > "$OUT/nft-dnat.txt"
sudo iptables -w -S FORWARD > "$OUT/iptables-forward.txt"

if ! sudo ip netns exec "$NS_NAME" python3 - "$WAN_HOST_IP" "$TCP_PORT" \
    > "$OUT/tcp-dnat-connect.txt" 2> "$OUT/tcp-dnat-connect.stderr" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.create_connection((host, port), timeout=2.0):
    pass
print(f"DNAT connected {host}:{port}")
PY
then
    capture_state
    echo "DNAT connection failed; retained diagnostics follow" >&2
    cat "$OUT/tcp-dnat-connect.stderr" >&2 || true
    cat "$OUT/iptables-forward.txt" >&2 || true
    cat "$OUT/nft-dnat.txt" >&2 || true
    grep -E "198\\.51\\.100|10\\.231\\.0\\.2" "$OUT/conntrack-tcp.txt" >&2 || true
    cat "$OUT/runtime.stderr" >&2 || true
    exit 1
fi
cat "$OUT/tcp-dnat-connect.txt"

sleep 0.1
sudo conntrack -L -p tcp > "$OUT/conntrack-after-dnat.txt" 2>&1 || true
grep -F "src=$WAN_CLIENT_IP dst=$WAN_HOST_IP" "$OUT/conntrack-after-dnat.txt" >/dev/null
grep -F "src=$LWIP_IP dst=$WAN_CLIENT_IP" "$OUT/conntrack-after-dnat.txt" >/dev/null
capture_state

kill -TERM "$PID"
wait "$PID"
PID=

cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "tcp-shift-p1: ready tun=$TUN_NAME host-ipv4=$HOST_IP" "$OUT/runtime.stdout" >/dev/null
grep -Eq 'rx_packets=[1-9][0-9]*' "$OUT/runtime.stderr"
grep -Eq 'tx_packets=[1-9][0-9]*' "$OUT/runtime.stderr"
grep -Eq 'tcp_accepts=([2-9]|[1-9][0-9]+)' "$OUT/runtime.stderr"

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "nonpersistent TUN survived runtime exit" >&2
    exit 1
fi

cleanup_network

if sudo ip netns list | grep -F "$NS_NAME" >/dev/null 2>&1; then
    echo "test network namespace survived cleanup" >&2
    exit 1
fi
if ip link show "$WAN_HOST_IF" >/dev/null 2>&1; then
    echo "test veth survived cleanup" >&2
    exit 1
fi
if sudo nft list table ip "$NFT_TABLE" >/dev/null 2>&1; then
    echo "test nftables table survived cleanup" >&2
    exit 1
fi

echo "P1 IPv4 direct and DNAT TCP smoke passed"
