#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p1-nft-ci"
BINARY=${TCP_SHIFT_P1_BINARY:-"$BUILD/tcp-shift-p1"}
TUN_NAME=${TCP_SHIFT_P1_NFT_TUN_NAME:-tsp1nft0}
LWIP_IP=${TCP_SHIFT_P1_NFT_LWIP_IP:-10.232.0.2}
HOST_IP=${TCP_SHIFT_P1_NFT_HOST_IP:-10.232.0.1}
HOST_CIDR=${TCP_SHIFT_P1_NFT_HOST_CIDR:-10.232.0.1/30}
NETMASK=${TCP_SHIFT_P1_NFT_NETMASK:-255.255.255.252}
TCP_PORT=${TCP_SHIFT_P1_NFT_TCP_PORT:-18081}
NS_NAME=${TCP_SHIFT_P1_NFT_NS_NAME:-tsp1nftns}
WAN_HOST_IF=${TCP_SHIFT_P1_NFT_WAN_HOST_IF:-tsp1nfth0}
WAN_NS_IF=${TCP_SHIFT_P1_NFT_WAN_NS_IF:-tsp1nftn0}
WAN_HOST_IP=${TCP_SHIFT_P1_NFT_WAN_HOST_IP:-198.51.101.1}
WAN_HOST_CIDR=${TCP_SHIFT_P1_NFT_WAN_HOST_CIDR:-198.51.101.1/24}
WAN_CLIENT_IP=${TCP_SHIFT_P1_NFT_WAN_CLIENT_IP:-198.51.101.2}
WAN_CLIENT_CIDR=${TCP_SHIFT_P1_NFT_WAN_CLIENT_CIDR:-198.51.101.2/24}
PRODUCT_TABLE=tcp_shift_p1
UNRELATED_TABLE=tcp_shift_p1_unrelated_ci
PID=
OLD_FORWARD=
FORWARD_RULES=0

mkdir -p "$OUT"

[ "$(id -u)" -eq 0 ] || {
    echo "p1-nft-lifecycle.sh must run as root so tcp-shift can exec nft" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing P1 binary: $BINARY" >&2
    exit 1
}
[ -c /dev/net/tun ] || {
    echo "/dev/net/tun is unavailable" >&2
    exit 1
}
command -v nft >/dev/null 2>&1 || {
    echo "nft is unavailable" >&2
    exit 1
}

capture_state()
{
    ip -d addr show > "$OUT/ip-addr.txt" 2>&1 || true
    ip route show table all > "$OUT/ip-route.txt" 2>&1 || true
    nft list ruleset > "$OUT/nft-ruleset.txt" 2>&1 || true
    iptables-save > "$OUT/iptables-save.txt" 2>&1 || true
    conntrack -L -p tcp > "$OUT/conntrack-tcp.txt" 2>&1 || true
    if ip netns list | grep -F "$NS_NAME" >/dev/null 2>&1; then
        ip -n "$NS_NAME" -d addr show > "$OUT/netns-addr.txt" 2>&1 || true
        ip -n "$NS_NAME" route show table all > "$OUT/netns-route.txt" 2>&1 || true
    fi
}

remove_forward_rules()
{
    if [ "$FORWARD_RULES" -eq 1 ]; then
        iptables -w -D FORWARD -i "$WAN_HOST_IF" -o "$TUN_NAME" \
            -p tcp -d "$LWIP_IP" --dport "$TCP_PORT" -j ACCEPT \
            >/dev/null 2>&1 || true
        iptables -w -D FORWARD -i "$TUN_NAME" -o "$WAN_HOST_IF" \
            -p tcp -s "$LWIP_IP" --sport "$TCP_PORT" -j ACCEPT \
            >/dev/null 2>&1 || true
        FORWARD_RULES=0
    fi
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
    remove_forward_rules
    if nft list table ip "$PRODUCT_TABLE" >/dev/null 2>&1; then
        nft delete table ip "$PRODUCT_TABLE" >/dev/null 2>&1 || true
    fi
    if nft list table ip "$UNRELATED_TABLE" >/dev/null 2>&1; then
        nft delete table ip "$UNRELATED_TABLE" >/dev/null 2>&1 || true
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

assert_no_product_resources()
{
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        echo "product TUN leaked after failed/terminated runtime" >&2
        exit 1
    fi
    if nft list table ip "$PRODUCT_TABLE" >/dev/null 2>&1; then
        echo "product nft table leaked after failed/terminated runtime" >&2
        exit 1
    fi
}

run_expect_failure()
{
    stdout_file=$1
    stderr_file=$2

    : > "$stdout_file"
    : > "$stderr_file"
    set +e
    "$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
        "$TCP_PORT" "$WAN_HOST_IP" > "$stdout_file" 2> "$stderr_file"
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        cat "$stdout_file" >&2 || true
        cat "$stderr_file" >&2 || true
        echo "runtime unexpectedly succeeded in a forced-failure case" >&2
        exit 1
    fi
}

wait_runtime_ready()
{
    ready=0
    i=0

    while [ "$i" -lt 100 ]; do
        if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
           ip -4 addr show dev "$TUN_NAME" | grep -F "$HOST_CIDR" >/dev/null 2>&1 &&
           nft list table ip "$PRODUCT_TABLE" >/dev/null 2>&1 &&
           grep -F "public-ipv4=$WAN_HOST_IP nft-table=$PRODUCT_TABLE" \
               "$OUT/runtime.stdout" >/dev/null 2>&1; then
            ready=1
            break
        fi
        if ! kill -0 "$PID" 2>/dev/null; then
            cat "$OUT/runtime.stdout" >&2 || true
            cat "$OUT/runtime.stderr" >&2 || true
            echo "runtime exited before product-owned ingress became ready" >&2
            exit 1
        fi
        i=$((i + 1))
        sleep 0.05
    done
    [ "$ready" -eq 1 ] || {
        echo "timed out waiting for product-owned nft ingress" >&2
        exit 1
    }
}

OLD_FORWARD=$(sysctl -n net.ipv4.ip_forward)

# A disabled global forwarding sysctl is an operator prerequisite. tcp-shift
# must diagnose it and unwind TUN state; it must never silently enable it.
sysctl -q -w net.ipv4.ip_forward=0
run_expect_failure "$OUT/forwarding-disabled.stdout" "$OUT/forwarding-disabled.stderr"
grep -F 'IPv4 forwarding is disabled' "$OUT/forwarding-disabled.stderr" >/dev/null
[ "$(sysctl -n net.ipv4.ip_forward)" = "0" ] || {
    echo "runtime mutated global IPv4 forwarding state" >&2
    exit 1
}
assert_no_product_resources
printf 'forwarding_disabled_preflight=ok\n' | tee "$OUT/forwarding-preflight.txt"

sysctl -q -w net.ipv4.ip_forward=1

# Keep an unrelated nft resource alive for the whole qualification. Its exact
# serialized form must be identical after collision, live traffic, and cleanup.
nft -f - <<EOF
create table ip $UNRELATED_TABLE
add chain ip $UNRELATED_TABLE marker
EOF
nft list table ip "$UNRELATED_TABLE" > "$OUT/unrelated-before.txt"
UNRELATED_BEFORE=$(sha256sum "$OUT/unrelated-before.txt" | awk '{print $1}')

# Simulate a stale/other owner using the exact product resource name. Exclusive
# create plus the read-only nft check must reject the startup; tcp-shift must not
# adopt or delete this table.
nft -f - <<EOF
create table ip $PRODUCT_TABLE
add chain ip $PRODUCT_TABLE occupied
EOF
run_expect_failure "$OUT/collision.stdout" "$OUT/collision.stderr"
grep -F 'install nft ingress' "$OUT/collision.stderr" >/dev/null
nft list table ip "$PRODUCT_TABLE" > "$OUT/collision-table.txt"
grep -F 'chain occupied' "$OUT/collision-table.txt" >/dev/null
if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "TUN leaked after nft resource collision" >&2
    exit 1
fi
nft delete table ip "$PRODUCT_TABLE"
printf 'exclusive_collision_rejection=ok\n' | tee "$OUT/collision-summary.txt"

# CI-only external topology. The product owns DNAT; the harness owns only the
# namespace/veth and the exact forwarding exceptions required by the runner's
# Docker-managed FORWARD policy.
ip netns add "$NS_NAME"
ip link add "$WAN_HOST_IF" type veth peer name "$WAN_NS_IF"
ip link set "$WAN_NS_IF" netns "$NS_NAME"
ip addr add "$WAN_HOST_CIDR" dev "$WAN_HOST_IF"
ip link set "$WAN_HOST_IF" up
ip -n "$NS_NAME" link set lo up
ip -n "$NS_NAME" addr add "$WAN_CLIENT_CIDR" dev "$WAN_NS_IF"
ip -n "$NS_NAME" link set "$WAN_NS_IF" up
ip -n "$NS_NAME" route add default via "$WAN_HOST_IP"

: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$TCP_PORT" "$WAN_HOST_IP" > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!
wait_runtime_ready

nft list table ip "$PRODUCT_TABLE" > "$OUT/product-table-live.txt"
grep -F "ip daddr $WAN_HOST_IP tcp dport $TCP_PORT" \
    "$OUT/product-table-live.txt" >/dev/null
grep -F "dnat to $LWIP_IP:$TCP_PORT" "$OUT/product-table-live.txt" >/dev/null

iptables -w -I FORWARD 1 -i "$WAN_HOST_IF" -o "$TUN_NAME" \
    -p tcp -d "$LWIP_IP" --dport "$TCP_PORT" -j ACCEPT
iptables -w -I FORWARD 1 -i "$TUN_NAME" -o "$WAN_HOST_IF" \
    -p tcp -s "$LWIP_IP" --sport "$TCP_PORT" -j ACCEPT
FORWARD_RULES=1

if ! ip netns exec "$NS_NAME" python3 - "$WAN_HOST_IP" "$TCP_PORT" \
    > "$OUT/public-connect.txt" 2> "$OUT/public-connect.stderr" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.create_connection((host, port), timeout=2.0):
    pass
print(f"product DNAT connected {host}:{port}")
PY
then
    capture_state
    cat "$OUT/public-connect.stderr" >&2 || true
    cat "$OUT/product-table-live.txt" >&2 || true
    cat "$OUT/runtime.stderr" >&2 || true
    echo "product-owned DNAT connection failed" >&2
    exit 1
fi
cat "$OUT/public-connect.txt"

sleep 0.1
conntrack -L -p tcp > "$OUT/conntrack-after-public.txt" 2>&1 || true
grep -F "src=$WAN_CLIENT_IP dst=$WAN_HOST_IP" "$OUT/conntrack-after-public.txt" >/dev/null
grep -F "src=$LWIP_IP dst=$WAN_CLIENT_IP" "$OUT/conntrack-after-public.txt" >/dev/null

# SIGTERM is the normal lifecycle boundary. The product must remove the nft
# table before closing the nonpersistent TUN.
stop_runtime
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2
assert_no_product_resources

nft list table ip "$UNRELATED_TABLE" > "$OUT/unrelated-after.txt"
UNRELATED_AFTER=$(sha256sum "$OUT/unrelated-after.txt" | awk '{print $1}')
[ "$UNRELATED_BEFORE" = "$UNRELATED_AFTER" ] || {
    echo "unrelated nftables state changed" >&2
    diff -u "$OUT/unrelated-before.txt" "$OUT/unrelated-after.txt" >&2 || true
    exit 1
}

remove_forward_rules
capture_state
printf 'signal_cleanup=ok unrelated_ruleset_unchanged=ok\n' | \
    tee "$OUT/lifecycle-summary.txt"
echo "P1 product-owned nft ingress lifecycle passed"
