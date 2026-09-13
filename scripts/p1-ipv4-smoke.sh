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
IDLE_SECONDS=${TCP_SHIFT_P1_IDLE_SECONDS:-2}
IDLE_WAIT_MAX=${TCP_SHIFT_P1_IDLE_WAIT_MAX:-32}
PID=
OLD_FORWARD=
FORWARD_RULES=0

mkdir -p "$OUT"

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

stop_runtime()
{
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID"
        wait "$PID"
    fi
    PID=
}

cleanup()
{
    set +e
    stop_runtime
    capture_state
    cleanup_network
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        sudo ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

wait_runtime_ready()
{
    stdout_file=$1
    stderr_file=$2
    ready=0
    i=0

    while [ "$i" -lt 100 ]; do
        if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
           ip -4 addr show dev "$TUN_NAME" | grep -F "$HOST_CIDR" >/dev/null 2>&1 &&
           grep -F "tcp-shift-p1: ready tun=$TUN_NAME" "$stdout_file" >/dev/null 2>&1; then
            ready=1
            break
        fi
        if ! kill -0 "$PID" 2>/dev/null; then
            cat "$stdout_file" >&2 || true
            cat "$stderr_file" >&2 || true
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
}

start_runtime()
{
    stdout_file=$1
    stderr_file=$2

    : > "$stdout_file"
    : > "$stderr_file"
    "$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" "$TCP_PORT" \
        > "$stdout_file" 2> "$stderr_file" &
    PID=$!
    wait_runtime_ready "$stdout_file" "$stderr_file"
}

extract_counter()
{
    name=$1
    file=$2
    sed -n "s/.*$name=\\([0-9][0-9]*\\).*/\\1/p" "$file" | tail -n 1
}

[ -x "$BINARY" ] || {
    echo "missing P1 binary: $BINARY" >&2
    exit 1
}
[ -c /dev/net/tun ] || {
    echo "/dev/net/tun is unavailable" >&2
    exit 1
}

# Idle qualification is a separate process so its counters are not polluted by
# the later ICMP/TCP traffic. lwIP's own timer deadlines are expected wakeups;
# the gate rejects a high-frequency polling loop and any idle EPOLLOUT activity.
start_runtime "$OUT/idle.stdout" "$OUT/idle.stderr"
sleep "$IDLE_SECONDS"
stop_runtime
cat "$OUT/idle.stdout"
cat "$OUT/idle.stderr" >&2

idle_wait_calls=$(extract_counter loop_wait_calls "$OUT/idle.stderr")
idle_timeout_wakeups=$(extract_counter loop_timeout_wakeups "$OUT/idle.stderr")
idle_ready_wakeups=$(extract_counter loop_ready_wakeups "$OUT/idle.stderr")
idle_writable_wakeups=$(extract_counter loop_tun_writable_wakeups "$OUT/idle.stderr")

[ -n "$idle_wait_calls" ] && [ -n "$idle_timeout_wakeups" ] &&
[ -n "$idle_ready_wakeups" ] && [ -n "$idle_writable_wakeups" ] || {
    echo "missing idle event-loop counters" >&2
    exit 1
}
[ "$idle_wait_calls" -gt 0 ] || {
    echo "idle loop recorded no waits" >&2
    exit 1
}
[ "$idle_wait_calls" -le "$IDLE_WAIT_MAX" ] || {
    echo "idle loop exceeded wait-call ceiling: $idle_wait_calls > $IDLE_WAIT_MAX" >&2
    exit 1
}
[ "$idle_writable_wakeups" -eq 0 ] || {
    echo "idle loop observed unexpected TUN writable wakeups: $idle_writable_wakeups" >&2
    exit 1
}

printf 'idle_seconds=%s wait_calls=%s timeout_wakeups=%s ready_wakeups=%s writable_wakeups=%s\n' \
    "$IDLE_SECONDS" "$idle_wait_calls" "$idle_timeout_wakeups" \
    "$idle_ready_wakeups" "$idle_writable_wakeups" | tee "$OUT/idle-summary.txt"

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "nonpersistent TUN survived idle runtime exit" >&2
    exit 1
fi

start_runtime "$OUT/runtime.stdout" "$OUT/runtime.stderr"
capture_state
ping -n -c 3 -W 1 "$LWIP_IP" | tee "$OUT/ping.txt"

# 1472 bytes of ICMP payload + 8-byte ICMP header + 20-byte IPv4 header is
# exactly the configured 1500-byte MTU and must traverse lwIP without fragment.
ping -n -c 1 -W 1 -M do -s 1472 "$LWIP_IP" | tee "$OUT/mtu-1500.txt"

# One byte beyond the configured MTU must be rejected locally with DF set. This
# proves the host-facing interface and lwIP netif agree on the same boundary.
if ping -n -c 1 -W 1 -M do -s 1473 "$LWIP_IP" \
    > "$OUT/mtu-over.txt" 2>&1; then
    cat "$OUT/mtu-over.txt" >&2
    echo "over-MTU DF ping unexpectedly succeeded" >&2
    exit 1
fi
cat "$OUT/mtu-over.txt"
grep -Ei 'message too long|mtu' "$OUT/mtu-over.txt" >/dev/null

# Send one deliberately bad ICMP checksum and one correct checksum through the
# same TUN route. lwIP must ignore the bad echo request, answer the valid one,
# and remain alive for the later TCP/DNAT qualification.
sudo python3 "$ROOT/scripts/p1-ipv4-checksum.py" "$HOST_IP" "$LWIP_IP" \
    | tee "$OUT/checksum.txt"
grep -F 'icmp_checksum_bad_reply=none icmp_checksum_good_reply=received' \
    "$OUT/checksum.txt" >/dev/null

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

stop_runtime
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "tcp-shift-p1: ready tun=$TUN_NAME host-ipv4=$HOST_IP" "$OUT/runtime.stdout" >/dev/null
grep -Eq 'rx_packets=[1-9][0-9]*' "$OUT/runtime.stderr"
grep -Eq 'rx_errors=0' "$OUT/runtime.stderr"
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

echo "P1 IPv4 idle/MTU/checksum/direct/DNAT smoke passed"
