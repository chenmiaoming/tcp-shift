#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p1-ipv6-pmtu-ci"
BINARY=${TCP_SHIFT_P1_IPV6_BINARY:-"$BUILD/tcp-shift-p1-ipv6"}
TUN_NAME=${TCP_SHIFT_P1_IPV6_PMTU_TUN_NAME:-tsp1pmtu0}
LWIP_IP=${TCP_SHIFT_P1_IPV6_PMTU_LWIP_IP:-fd00:198:19::2}
HOST_CIDR=${TCP_SHIFT_P1_IPV6_PMTU_HOST_CIDR:-fd00:198:19::1/126}
TCP_PORT=${TCP_SHIFT_P1_IPV6_PMTU_TCP_PORT:-18084}
NS_NAME=${TCP_SHIFT_P1_IPV6_PMTU_NS_NAME:-tsp1pmtuns}
WAN_HOST_IF=${TCP_SHIFT_P1_IPV6_PMTU_HOST_IF:-tsp1pmtuh0}
WAN_NS_IF=${TCP_SHIFT_P1_IPV6_PMTU_NS_IF:-tsp1pmtun0}
WAN_HOST_IP=${TCP_SHIFT_P1_IPV6_PMTU_HOST_IP:-2001:db8:202::1}
WAN_HOST_CIDR=${TCP_SHIFT_P1_IPV6_PMTU_HOST_CIDR:-2001:db8:202::1/64}
WAN_CLIENT_IP=${TCP_SHIFT_P1_IPV6_PMTU_CLIENT_IP:-2001:db8:202::2}
WAN_CLIENT_CIDR=${TCP_SHIFT_P1_IPV6_PMTU_CLIENT_CIDR:-2001:db8:202::2/64}
PATH_MTU=${TCP_SHIFT_P1_IPV6_PMTU_PATH_MTU:-1280}
EXPECTED_MSS=$((PATH_MTU - 40 - 20))
PID=
CAPTURE_PID=
OLD_FORWARD=
FORWARD_RULES=0

mkdir -p "$OUT"

[ "$(id -u)" -eq 0 ] || {
    echo "p1-ipv6-pmtu.sh must run as root" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing IPv6 P1 binary: $BINARY" >&2
    exit 1
}
command -v tcpdump >/dev/null 2>&1 || {
    echo "tcpdump is unavailable" >&2
    exit 1
}
command -v ip6tables >/dev/null 2>&1 || {
    echo "ip6tables is unavailable" >&2
    exit 1
}

stop_capture()
{
    if [ -n "${CAPTURE_PID:-}" ] && kill -0 "$CAPTURE_PID" 2>/dev/null; then
        kill "$CAPTURE_PID" >/dev/null 2>&1 || true
        wait "$CAPTURE_PID" >/dev/null 2>&1 || true
    fi
    CAPTURE_PID=
}

stop_runtime()
{
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID"
        wait "$PID"
    fi
    PID=
}

remove_forward_rules()
{
    if [ "$FORWARD_RULES" -eq 1 ]; then
        ip6tables -w -D FORWARD -i "$WAN_HOST_IF" -o "$TUN_NAME" \
            -d "$LWIP_IP" -j ACCEPT >/dev/null 2>&1 || true
        ip6tables -w -D FORWARD -i "$TUN_NAME" -o "$WAN_HOST_IF" \
            -s "$LWIP_IP" -j ACCEPT >/dev/null 2>&1 || true
        FORWARD_RULES=0
    fi
}

capture_state()
{
    ip -6 -d addr show > "$OUT/ip6-addr.txt" 2>&1 || true
    ip -6 route show table all > "$OUT/ip6-route.txt" 2>&1 || true
    ip6tables-save > "$OUT/ip6tables-save.txt" 2>&1 || true
    if ip netns list | grep -F "$NS_NAME" >/dev/null 2>&1; then
        ip -n "$NS_NAME" -6 -d addr show > "$OUT/netns-ip6-addr.txt" 2>&1 || true
        ip -n "$NS_NAME" -6 route show table all > "$OUT/netns-ip6-route.txt" 2>&1 || true
    fi
}

cleanup()
{
    set +e
    stop_capture
    stop_runtime
    capture_state
    remove_forward_rules
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

wait_runtime_ready()
{
    i=0
    while [ "$i" -lt 100 ]; do
        if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
           grep -F "tcp-shift-p1-ipv6: ready tun=$TUN_NAME" \
               "$OUT/runtime.stdout" >/dev/null 2>&1; then
            return 0
        fi
        if ! kill -0 "$PID" 2>/dev/null; then
            cat "$OUT/runtime.stdout" >&2 || true
            cat "$OUT/runtime.stderr" >&2 || true
            echo "IPv6 PMTU runtime exited before ready" >&2
            exit 1
        fi
        i=$((i + 1))
        sleep 0.05
    done
    echo "timed out waiting for IPv6 PMTU runtime" >&2
    exit 1
}

connect_once()
{
    output=$1
    ip netns exec "$NS_NAME" python3 - "$LWIP_IP" "$TCP_PORT" \
        > "$output" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as sock:
    sock.settimeout(2.0)
    sock.connect((host, port, 0, 0))
print(f"connected [{host}]:{port}")
PY
}

capture_synack()
{
    output=$1
    connect_output=$2

    # libpcap rejects raw tcp[13] byte offsets for IPv6. The runtime is idle
    # and this controlled connection is the only flow, so capture the first TCP
    # packet emitted by the listener. The caller validates its SYN-only MSS
    # option, which identifies the SYN-ACK without another timing-sensitive read.
    timeout 4 tcpdump -i "$TUN_NAME" -c 1 -nn -vv -l \
        "ip6 and tcp and src host $LWIP_IP and src port $TCP_PORT" \
        > "$output" 2>&1 &
    CAPTURE_PID=$!
    sleep 0.1
    connect_once "$connect_output"
    if ! wait "$CAPTURE_PID"; then
        CAPTURE_PID=
        cat "$output" >&2 || true
        echo "failed to capture lwIP IPv6 SYN-ACK" >&2
        exit 1
    fi
    CAPTURE_PID=
}

OLD_FORWARD=$(sysctl -n net.ipv6.conf.all.forwarding)
sysctl -q -w net.ipv6.conf.all.forwarding=1

ip netns add "$NS_NAME"
ip link add "$WAN_HOST_IF" type veth peer name "$WAN_NS_IF"
ip link set "$WAN_NS_IF" netns "$NS_NAME"
ip link set "$WAN_HOST_IF" mtu 1500 up
ip -6 addr add "$WAN_HOST_CIDR" dev "$WAN_HOST_IF" nodad
ip -n "$NS_NAME" link set lo up
ip -n "$NS_NAME" link set "$WAN_NS_IF" mtu 1500 up
ip -n "$NS_NAME" -6 addr add "$WAN_CLIENT_CIDR" dev "$WAN_NS_IF" nodad
ip -n "$NS_NAME" -6 route add "$LWIP_IP"/128 via "$WAN_HOST_IP" dev "$WAN_NS_IF"

: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
"$BINARY" "$TUN_NAME" "$LWIP_IP" "$HOST_CIDR" "$TCP_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!
wait_runtime_ready

ip6tables -w -I FORWARD 1 -i "$WAN_HOST_IF" -o "$TUN_NAME" \
    -d "$LWIP_IP" -j ACCEPT
ip6tables -w -I FORWARD 1 -i "$TUN_NAME" -o "$WAN_HOST_IF" \
    -s "$LWIP_IP" -j ACCEPT
FORWARD_RULES=1

# Before any PTB feedback, the TUN MTU is 1500, so the IPv6 SYN-ACK should
# advertise the normal 1440-byte TCP MSS. tcpdump prints SYN MSS options in the
# stable form "mss N]" when this is the last option, so use a literal match and
# avoid shell/ERE character-class ambiguity in this qualification gate.
capture_synack "$OUT/synack-before.txt" "$OUT/connect-before.txt"
grep -F 'mss 1440]' "$OUT/synack-before.txt" >/dev/null || {
    cat "$OUT/synack-before.txt" >&2
    echo "baseline IPv6 MSS was not 1440" >&2
    exit 1
}

# Keep the client link itself at MTU 1500 so a 1500-byte request can enter the
# router. Apply the smaller MTU only to the host's egress route back to this
# client. The request therefore reaches lwIP unchanged, while the 1500-byte
# reply hits the 1280-byte route and Linux must send ICMPv6 Packet Too Big back
# to the lwIP source through the TUN.
ip -6 route replace "$WAN_CLIENT_IP"/128 dev "$WAN_HOST_IF" mtu "$PATH_MTU"
ip -6 route get "$WAN_CLIENT_IP" > "$OUT/egress-route.txt"
grep -F "mtu $PATH_MTU" "$OUT/egress-route.txt" >/dev/null

# Both the oversized echo request and the resulting PTB are ICMPv6 packets
# destined for lwIP and therefore visible on the TUN. Capture two packets so
# the request cannot satisfy the capture by itself; then require the second
# semantic event to be a Packet Too Big advertising the configured path MTU.
timeout 4 tcpdump -i "$TUN_NAME" -c 2 -nn -vv -l \
    "icmp6 and dst host $LWIP_IP" > "$OUT/ptb-wire.txt" 2>&1 &
CAPTURE_PID=$!
sleep 0.1
set +e
ip netns exec "$NS_NAME" ping -6 -c 1 -W 1 -M do -s 1452 "$LWIP_IP" \
    > "$OUT/oversize-ping.txt" 2>&1
PING_RC=$?
set -e
[ "$PING_RC" -ne 0 ] || {
    echo "oversize IPv6 ping unexpectedly crossed the 1280-byte path" >&2
    exit 1
}
if ! wait "$CAPTURE_PID"; then
    CAPTURE_PID=
    cat "$OUT/ptb-wire.txt" >&2 || true
    cat "$OUT/oversize-ping.txt" >&2 || true
    echo "did not observe ICMPv6 Packet Too Big at lwIP TUN" >&2
    exit 1
fi
CAPTURE_PID=
grep -F 'packet too big' "$OUT/ptb-wire.txt" >/dev/null
grep -E 'mtu 1280|mtu 1280,' "$OUT/ptb-wire.txt" >/dev/null

# A subsequent connection to the same destination must now use the learned
# destination PMTU. tcp_eff_send_mss_netif() is expected to advertise
# 1280 - 40-byte IPv6 - 20-byte TCP = 1220 bytes.
sleep 0.1
capture_synack "$OUT/synack-after.txt" "$OUT/connect-after.txt"
grep -F "mss $EXPECTED_MSS]" "$OUT/synack-after.txt" >/dev/null || {
    cat "$OUT/ptb-wire.txt" >&2 || true
    cat "$OUT/synack-after.txt" >&2 || true
    echo "lwIP did not apply learned IPv6 PMTU to subsequent TCP MSS" >&2
    exit 1
}

stop_runtime
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2
grep -E 'tcp_accepts=[2-9][0-9]*' "$OUT/runtime.stderr" >/dev/null

remove_forward_rules
capture_state
printf 'ipv6_ptb_mtu=%u baseline_mss=1440 learned_mss=%u pmtu_adaptation=ok\n' \
    "$PATH_MTU" "$EXPECTED_MSS" | tee "$OUT/pmtu-summary.txt"
echo "P1b routed IPv6 Packet Too Big/PMTU qualification passed"
