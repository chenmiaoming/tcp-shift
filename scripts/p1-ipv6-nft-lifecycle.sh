#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p1-ipv6-nft-ci"
BINARY=${TCP_SHIFT_P1_IPV6_BINARY:-"$BUILD/tcp-shift-p1-ipv6"}
TUN_NAME=${TCP_SHIFT_P1_IPV6_NFT_TUN_NAME:-tsp1v6nft0}
LWIP_IP=${TCP_SHIFT_P1_IPV6_NFT_LWIP_IP:-fd00:198:18::2}
HOST_CIDR=${TCP_SHIFT_P1_IPV6_NFT_HOST_CIDR:-fd00:198:18::1/126}
TCP_PORT=${TCP_SHIFT_P1_IPV6_NFT_TCP_PORT:-18083}
NS_NAME=${TCP_SHIFT_P1_IPV6_NFT_NS_NAME:-tsp1v6nftns}
WAN_HOST_IF=${TCP_SHIFT_P1_IPV6_NFT_WAN_HOST_IF:-tsp1v6h0}
WAN_NS_IF=${TCP_SHIFT_P1_IPV6_NFT_WAN_NS_IF:-tsp1v6n0}
WAN_HOST_IP=${TCP_SHIFT_P1_IPV6_NFT_WAN_HOST_IP:-2001:db8:101::1}
WAN_HOST_CIDR=${TCP_SHIFT_P1_IPV6_NFT_WAN_HOST_CIDR:-2001:db8:101::1/64}
WAN_CLIENT_IP=${TCP_SHIFT_P1_IPV6_NFT_WAN_CLIENT_IP:-2001:db8:101::2}
WAN_CLIENT_CIDR=${TCP_SHIFT_P1_IPV6_NFT_WAN_CLIENT_CIDR:-2001:db8:101::2/64}
PRODUCT_TABLE=tcp_shift_p1
UNRELATED_TABLE=tcp_shift_p1_unrelated_v6_ci
PID=
OLD_FORWARD=
FORWARD_RULES=0

mkdir -p "$OUT"

[ "$(id -u)" -eq 0 ] || {
    echo "p1-ipv6-nft-lifecycle.sh must run as root" >&2
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
command -v nft >/dev/null 2>&1 || {
    echo "nft is unavailable" >&2
    exit 1
}
command -v ip6tables >/dev/null 2>&1 || {
    echo "ip6tables is unavailable" >&2
    exit 1
}

capture_state()
{
    ip -6 -d addr show > "$OUT/ip6-addr.txt" 2>&1 || true
    ip -6 route show table all > "$OUT/ip6-route.txt" 2>&1 || true
    nft list ruleset > "$OUT/nft-ruleset.txt" 2>&1 || true
    ip6tables-save > "$OUT/ip6tables-save.txt" 2>&1 || true
    conntrack -L -f ipv6 -p tcp > "$OUT/conntrack-ipv6-tcp.txt" 2>&1 || true
    if ip netns list | grep -F "$NS_NAME" >/dev/null 2>&1; then
        ip -n "$NS_NAME" -6 -d addr show > "$OUT/netns-ip6-addr.txt" 2>&1 || true
        ip -n "$NS_NAME" -6 route show table all > "$OUT/netns-ip6-route.txt" 2>&1 || true
    fi
}

remove_forward_rules()
{
    if [ "$FORWARD_RULES" -eq 1 ]; then
        ip6tables -w -D FORWARD -i "$WAN_HOST_IF" -o "$TUN_NAME" \
            -p tcp -d "$LWIP_IP" --dport "$TCP_PORT" -j ACCEPT \
            >/dev/null 2>&1 || true
        ip6tables -w -D FORWARD -i "$TUN_NAME" -o "$WAN_HOST_IF" \
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
    if nft list table ip6 "$PRODUCT_TABLE" >/dev/null 2>&1; then
        nft delete table ip6 "$PRODUCT_TABLE" >/dev/null 2>&1 || true
    fi
    if nft list table ip6 "$UNRELATED_TABLE" >/dev/null 2>&1; then
        nft delete table ip6 "$UNRELATED_TABLE" >/dev/null 2>&1 || true
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
        sysctl -q -w net.ipv6.conf.all.forwarding="$OLD_FORWARD" >/dev/null 2>&1 || true
        OLD_FORWARD=
    fi
}
trap cleanup EXIT HUP INT TERM

assert_no_product_resources()
{
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        echo "IPv6 product TUN leaked after failed/terminated runtime" >&2
        exit 1
    fi
    if nft list table ip6 "$PRODUCT_TABLE" >/dev/null 2>&1; then
        echo "IPv6 product nft table leaked after failed/terminated runtime" >&2
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
    "$BINARY" "$TUN_NAME" "$LWIP_IP" "$HOST_CIDR" "$TCP_PORT" \
        "$WAN_HOST_IP" > "$stdout_file" 2> "$stderr_file"
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        cat "$stdout_file" >&2 || true
        cat "$stderr_file" >&2 || true
        echo "IPv6 runtime unexpectedly succeeded in a forced-failure case" >&2
        exit 1
    fi
}

wait_runtime_ready()
{
    ready=0
    i=0

    while [ "$i" -lt 100 ]; do
        if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
           ip -6 addr show dev "$TUN_NAME" | grep -F "$HOST_CIDR" >/dev/null 2>&1 &&
           nft list table ip6 "$PRODUCT_TABLE" >/dev/null 2>&1 &&
           grep -F "public-ipv6=$WAN_HOST_IP nft-table=$PRODUCT_TABLE" \
               "$OUT/runtime.stdout" >/dev/null 2>&1; then
            ready=1
            break
        fi
        if ! kill -0 "$PID" 2>/dev/null; then
            cat "$OUT/runtime.stdout" >&2 || true
            cat "$OUT/runtime.stderr" >&2 || true
            echo "IPv6 runtime exited before product-owned ingress became ready" >&2
            exit 1
        fi
        i=$((i + 1))
        sleep 0.05
    done
    [ "$ready" -eq 1 ] || {
        echo "timed out waiting for product-owned IPv6 nft ingress" >&2
        exit 1
    }
}

OLD_FORWARD=$(sysctl -n net.ipv6.conf.all.forwarding)

# IPv6 forwarding is an operator-owned prerequisite. tcp-shift diagnoses but
# never rewrites this global host policy.
sysctl -q -w net.ipv6.conf.all.forwarding=0
run_expect_failure "$OUT/forwarding-disabled.stdout" "$OUT/forwarding-disabled.stderr"
grep -F 'IPv6 forwarding is disabled' "$OUT/forwarding-disabled.stderr" >/dev/null
[ "$(sysctl -n net.ipv6.conf.all.forwarding)" = "0" ] || {
    echo "runtime mutated global IPv6 forwarding state" >&2
    exit 1
}
assert_no_product_resources
printf 'ipv6_forwarding_disabled_preflight=ok\n' | tee "$OUT/forwarding-preflight.txt"

sysctl -q -w net.ipv6.conf.all.forwarding=1

nft -f - <<EOF
create table ip6 $UNRELATED_TABLE
add chain ip6 $UNRELATED_TABLE marker
EOF
nft list table ip6 "$UNRELATED_TABLE" > "$OUT/unrelated-before.txt"
UNRELATED_BEFORE=$(sha256sum "$OUT/unrelated-before.txt" | awk '{print $1}')

# Existing ownership is never adopted. A stale/foreign table with the product
# name must make startup fail while leaving that table untouched.
nft -f - <<EOF
create table ip6 $PRODUCT_TABLE
add chain ip6 $PRODUCT_TABLE occupied
EOF
run_expect_failure "$OUT/collision.stdout" "$OUT/collision.stderr"
grep -F 'install IPv6 nft ingress' "$OUT/collision.stderr" >/dev/null
nft list table ip6 "$PRODUCT_TABLE" > "$OUT/collision-table.txt"
grep -F 'chain occupied' "$OUT/collision-table.txt" >/dev/null
if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    echo "IPv6 TUN leaked after nft resource collision" >&2
    exit 1
fi
nft delete table ip6 "$PRODUCT_TABLE"
printf 'ipv6_exclusive_collision_rejection=ok\n' | tee "$OUT/collision-summary.txt"

# CI-only external topology. Product owns only its ip6 DNAT table; the harness
# owns the namespace/veth and exact FORWARD exceptions needed by the runner.
ip netns add "$NS_NAME"
ip link add "$WAN_HOST_IF" type veth peer name "$WAN_NS_IF"
ip link set "$WAN_NS_IF" netns "$NS_NAME"
ip link set "$WAN_HOST_IF" up
ip -6 addr add "$WAN_HOST_CIDR" dev "$WAN_HOST_IF" nodad
ip -n "$NS_NAME" link set lo up
ip -n "$NS_NAME" link set "$WAN_NS_IF" up
ip -n "$NS_NAME" -6 addr add "$WAN_CLIENT_CIDR" dev "$WAN_NS_IF" nodad

: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
"$BINARY" "$TUN_NAME" "$LWIP_IP" "$HOST_CIDR" "$TCP_PORT" \
    "$WAN_HOST_IP" > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!
wait_runtime_ready

nft list table ip6 "$PRODUCT_TABLE" > "$OUT/product-table-live.txt"
grep -F "ip6 daddr $WAN_HOST_IP" "$OUT/product-table-live.txt" >/dev/null
grep -F 'meta l4proto tcp' "$OUT/product-table-live.txt" >/dev/null
grep -F "tcp dport $TCP_PORT" "$OUT/product-table-live.txt" >/dev/null
grep -F "$LWIP_IP" "$OUT/product-table-live.txt" >/dev/null

ip6tables -w -I FORWARD 1 -i "$WAN_HOST_IF" -o "$TUN_NAME" \
    -p tcp -d "$LWIP_IP" --dport "$TCP_PORT" -j ACCEPT
ip6tables -w -I FORWARD 1 -i "$TUN_NAME" -o "$WAN_HOST_IF" \
    -p tcp -s "$LWIP_IP" --sport "$TCP_PORT" -j ACCEPT
FORWARD_RULES=1

if ! ip netns exec "$NS_NAME" python3 - "$WAN_HOST_IP" "$TCP_PORT" \
    > "$OUT/public-connect.txt" 2> "$OUT/public-connect.stderr" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as sock:
    sock.settimeout(2.0)
    sock.connect((host, port, 0, 0))
print(f"product IPv6 DNAT connected [{host}]:{port}")
PY
then
    capture_state
    cat "$OUT/public-connect.stderr" >&2 || true
    cat "$OUT/product-table-live.txt" >&2 || true
    cat "$OUT/runtime.stderr" >&2 || true
    echo "product-owned IPv6 DNAT connection failed" >&2
    exit 1
fi
cat "$OUT/public-connect.txt"

sleep 0.1
conntrack -L -f ipv6 -p tcp > "$OUT/conntrack-after-public.txt" 2>&1 || true
grep -F "src=$WAN_CLIENT_IP dst=$WAN_HOST_IP" "$OUT/conntrack-after-public.txt" >/dev/null
grep -F "src=$LWIP_IP dst=$WAN_CLIENT_IP" "$OUT/conntrack-after-public.txt" >/dev/null

stop_runtime
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2
assert_no_product_resources

nft list table ip6 "$UNRELATED_TABLE" > "$OUT/unrelated-after.txt"
UNRELATED_AFTER=$(sha256sum "$OUT/unrelated-after.txt" | awk '{print $1}')
[ "$UNRELATED_BEFORE" = "$UNRELATED_AFTER" ] || {
    echo "unrelated IPv6 nftables state changed" >&2
    diff -u "$OUT/unrelated-before.txt" "$OUT/unrelated-after.txt" >&2 || true
    exit 1
}

remove_forward_rules
capture_state
printf 'ipv6_signal_cleanup=ok unrelated_ruleset_unchanged=ok\n' | \
    tee "$OUT/lifecycle-summary.txt"
echo "P1b product-owned IPv6 nft ingress lifecycle passed"
