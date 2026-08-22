#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "bench-long-fat.sh must run as root (use sudo -E)" >&2
  exit 1
fi

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${BIN:-"$ROOT/bin/tcp-shift"}
OUT=${OUT:-"$ROOT/.bench"}
ROUTER_NS=${ROUTER_NS:-tcpshift-router}
CLIENT_NS=${CLIENT_NS:-tcpshift-client}
ROOT_WAN_IF=${ROOT_WAN_IF:-ts-wan0}
ROUTER_WAN_IF=${ROUTER_WAN_IF:-ts-wan1}
ROUTER_CLIENT_IF=${ROUTER_CLIENT_IF:-ts-lan0}
CLIENT_IF=${CLIENT_IF:-ts-lan1}
TUN_IF=${TUN_IF:-ts0}

# Space-separated case list. Keeping this selectable lets CI run a genuinely
# unpatched upstream-gVisor CUBIC+RACK control without asking that build for the
# tcp-shift-only experimental BBR congestion-control name.
BENCH_CASES=${BENCH_CASES:-"native-cubic native-bbr gvisor-cubic gvisor-bbr"}

# RFC 2544 benchmarking space. The WAN-facing endpoints are deliberately
# separate from the transit links so native TCP and gVisor TCP use the same
# routed impairment path.
NATIVE_ADDR=${NATIVE_ADDR:-198.18.0.1}
GVISOR_ADDR=${GVISOR_ADDR:-198.18.0.2}
ROOT_WAN_ADDR=${ROOT_WAN_ADDR:-198.19.0.1/30}
ROUTER_WAN_ADDR=${ROUTER_WAN_ADDR:-198.19.0.2/30}
ROUTER_CLIENT_ADDR=${ROUTER_CLIENT_ADDR:-198.19.0.5/30}
CLIENT_ADDR=${CLIENT_ADDR:-198.19.0.6/30}
ROOT_WAN_IP=${ROOT_WAN_ADDR%/*}
ROUTER_WAN_IP=${ROUTER_WAN_ADDR%/*}
ROUTER_CLIENT_IP=${ROUTER_CLIENT_ADDR%/*}
CLIENT_IP=${CLIENT_ADDR%/*}

RATE=${RATE:-50mbit}
ONE_WAY_DELAY=${ONE_WAY_DELAY:-50ms}
LOSS=${LOSS:-0.10%}
DATA_LOSS=${DATA_LOSS:-$LOSS}
ACK_LOSS=${ACK_LOSS:-$LOSS}
RECOVERY=${RECOVERY:-rack}
NETEM_LIMIT=${NETEM_LIMIT:-10000}
DURATION=${DURATION:-12}
TRIALS=${TRIALS:-2}
TCP_BUFFER_MIB=${TCP_BUFFER_MIB:-4}
CASE_TIMEOUT=${CASE_TIMEOUT:-$((DURATION + 30))}

if [[ "$RECOVERY" != rack && "$RECOVERY" != legacy ]]; then
  echo "RECOVERY must be rack or legacy" >&2
  exit 1
fi

for case_name in $BENCH_CASES; do
  case "$case_name" in
    native-cubic|native-bbr|gvisor-cubic|gvisor-bbr) ;;
    *) echo "unsupported BENCH_CASES entry: $case_name" >&2; exit 1 ;;
  esac
done

mkdir -p "$OUT"
rm -f "$OUT"/*

test -x "$BIN" || { echo "missing $BIN; run scripts/build.sh first" >&2; exit 1; }
command -v iperf3 >/dev/null
command -v tc >/dev/null
command -v jq >/dev/null
command -v timeout >/dev/null
command -v iptables >/dev/null

if command -v modprobe >/dev/null 2>&1; then
  modprobe tcp_bbr >/dev/null 2>&1 || true
fi
AVAILABLE_CC=$(cat /proc/sys/net/ipv4/tcp_available_congestion_control)
if [[ " $BENCH_CASES " == *" native-bbr "* ]] && ! grep -qw bbr <<<"$AVAILABLE_CC"; then
  echo "native Linux BBR is unavailable; available congestion controls: $AVAILABLE_CC" >&2
  exit 1
fi

cleanup() {
  set +e
  pkill -f "$BIN" >/dev/null 2>&1 || true
  pkill -f 'iperf3 -s -1 -B 127.0.0.1 -p 5202' >/dev/null 2>&1 || true
  iptables -D FORWARD -i "$ROOT_WAN_IF" -o "$TUN_IF" -j ACCEPT >/dev/null 2>&1 || true
  iptables -D FORWARD -i "$TUN_IF" -o "$ROOT_WAN_IF" -j ACCEPT >/dev/null 2>&1 || true
  ip netns del "$CLIENT_NS" >/dev/null 2>&1 || true
  ip netns del "$ROUTER_NS" >/dev/null 2>&1 || true
  ip link del "$ROOT_WAN_IF" >/dev/null 2>&1 || true
  ip link del "$TUN_IF" >/dev/null 2>&1 || true
  ip addr del "$NATIVE_ADDR/32" dev lo >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup

wait_pid_bounded() {
  local pid=$1 seconds=$2
  local ticks=$((seconds * 10))
  for _ in $(seq 1 "$ticks"); do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep 0.1
  done
  kill -TERM "$pid" 2>/dev/null || true
  sleep 0.2
  kill -KILL "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

configure_netem() {
  # Keep Linux's sender-side fq intact so native BBR can use the kernel pacing
  # machinery. Delay/rate/loss live in the intermediate router instead:
  #   root sender -> fq -> router -> DATA netem -> client
  #   client -> router -> ACK netem -> root sender
  tc qdisc replace dev "$ROOT_WAN_IF" root fq
  ip netns exec "$ROUTER_NS" tc qdisc replace dev "$ROUTER_CLIENT_IF" root netem \
    limit "$NETEM_LIMIT" delay "$ONE_WAY_DELAY" rate "$RATE" loss "$DATA_LOSS"
  ip netns exec "$ROUTER_NS" tc qdisc replace dev "$ROUTER_WAN_IF" root netem \
    limit "$NETEM_LIMIT" delay "$ONE_WAY_DELAY" rate "$RATE" loss "$ACK_LOSS"
}

dump_network_state() {
  local label=$1
  {
    echo "label=$label"
    date -u +'%Y-%m-%dT%H:%M:%SZ'
    echo "data_loss=$DATA_LOSS"
    echo "ack_loss=$ACK_LOSS"
    echo "native_cc_available=$AVAILABLE_CC"
    echo '--- root routes ---'
    ip route show table main
    echo '--- root WAN ---'
    ip -s -details addr show dev "$ROOT_WAN_IF" || true
    echo '--- sender root fq ---'
    tc -s -d qdisc show dev "$ROOT_WAN_IF" || true
    echo '--- TUN ---'
    ip -s -details addr show dev "$TUN_IF" || true
    echo '--- router routes ---'
    ip netns exec "$ROUTER_NS" ip route show table main || true
    echo '--- router data qdisc ---'
    ip netns exec "$ROUTER_NS" tc -s -d qdisc show dev "$ROUTER_CLIENT_IF" || true
    echo '--- router ACK qdisc ---'
    ip netns exec "$ROUTER_NS" tc -s -d qdisc show dev "$ROUTER_WAN_IF" || true
    echo '--- client routes ---'
    ip netns exec "$CLIENT_NS" ip route show table main || true
    echo '--- client link ---'
    ip netns exec "$CLIENT_NS" ip -s -details addr show dev "$CLIENT_IF" || true
    echo '--- root FORWARD chain ---'
    iptables -nvL FORWARD --line-numbers || true
    echo '--- router FORWARD chain ---'
    ip netns exec "$ROUTER_NS" iptables -nvL FORWARD --line-numbers || true
    echo '--- forwarding sysctls ---'
    sysctl net.ipv4.ip_forward || true
    ip netns exec "$ROUTER_NS" sysctl net.ipv4.ip_forward || true
  } >"$OUT/network-${label}.txt" 2>&1
}

sysctl -q -w net.ipv4.ip_forward=1

ip netns add "$ROUTER_NS"
ip netns add "$CLIENT_NS"
ip netns exec "$ROUTER_NS" ip link set lo up
ip netns exec "$CLIENT_NS" ip link set lo up

# Sender/root <-> impairment router.
ip link add "$ROOT_WAN_IF" type veth peer name "$ROUTER_WAN_IF"
ip link set "$ROUTER_WAN_IF" netns "$ROUTER_NS"
ip addr add "$ROOT_WAN_ADDR" dev "$ROOT_WAN_IF"
ip link set "$ROOT_WAN_IF" up
ip netns exec "$ROUTER_NS" ip addr add "$ROUTER_WAN_ADDR" dev "$ROUTER_WAN_IF"
ip netns exec "$ROUTER_NS" ip link set "$ROUTER_WAN_IF" up

# Impairment router <-> remote client.
ip link add "$ROUTER_CLIENT_IF" type veth peer name "$CLIENT_IF"
ip link set "$ROUTER_CLIENT_IF" netns "$ROUTER_NS"
ip link set "$CLIENT_IF" netns "$CLIENT_NS"
ip netns exec "$ROUTER_NS" ip addr add "$ROUTER_CLIENT_ADDR" dev "$ROUTER_CLIENT_IF"
ip netns exec "$ROUTER_NS" ip link set "$ROUTER_CLIENT_IF" up
ip netns exec "$CLIENT_NS" ip addr add "$CLIENT_ADDR" dev "$CLIENT_IF"
ip netns exec "$CLIENT_NS" ip link set "$CLIENT_IF" up

# Native endpoint is local to root; gVisor owns its own /32 behind TUN.
ip addr add "$NATIVE_ADDR/32" dev lo
ip tuntap add dev "$TUN_IF" mode tun
ip link set "$TUN_IF" mtu 1500 up
ip route add "$GVISOR_ADDR/32" dev "$TUN_IF"

# Routing through the impairment namespace.
ip route add 198.19.0.4/30 via "$ROUTER_WAN_IP" dev "$ROOT_WAN_IF"
ip netns exec "$ROUTER_NS" ip route add 198.18.0.0/24 via "$ROOT_WAN_IP" dev "$ROUTER_WAN_IF"
ip netns exec "$CLIENT_NS" ip route add 198.18.0.0/24 via "$ROUTER_CLIENT_IP" dev "$CLIENT_IF"

# gVisor traffic traverses root forwarding between the routed WAN link and TUN.
iptables -I FORWARD 1 -i "$ROOT_WAN_IF" -o "$TUN_IF" -j ACCEPT
iptables -I FORWARD 1 -i "$TUN_IF" -o "$ROOT_WAN_IF" -j ACCEPT
sysctl -q -w "net.ipv4.conf.${ROOT_WAN_IF}.rp_filter=0" || true
sysctl -q -w "net.ipv4.conf.${TUN_IF}.rp_filter=0" || true

# The router namespace is the only place where WAN impairment is applied.
ip netns exec "$ROUTER_NS" sysctl -q -w net.ipv4.ip_forward=1
ip netns exec "$ROUTER_NS" iptables -I FORWARD 1 -i "$ROUTER_WAN_IF" -o "$ROUTER_CLIENT_IF" -j ACCEPT
ip netns exec "$ROUTER_NS" iptables -I FORWARD 1 -i "$ROUTER_CLIENT_IF" -o "$ROUTER_WAN_IF" -j ACCEPT
ip netns exec "$ROUTER_NS" sysctl -q -w "net.ipv4.conf.${ROUTER_WAN_IF}.rp_filter=0" || true
ip netns exec "$ROUTER_NS" sysctl -q -w "net.ipv4.conf.${ROUTER_CLIENT_IF}.rp_filter=0" || true

configure_netem
dump_network_state setup
cat >"$OUT/scenario.txt" <<EOF
rate=$RATE
one_way_delay=$ONE_WAY_DELAY
loss_compat_default=$LOSS
data_loss=$DATA_LOSS
ack_loss=$ACK_LOSS
recovery=$RECOVERY
bench_cases=$BENCH_CASES
netem_limit_packets=$NETEM_LIMIT
duration_seconds=$DURATION
trials=$TRIALS
tcp_buffer_mib=$TCP_BUFFER_MIB
native_cc_available=$AVAILABLE_CC
native_sender_qdisc=fq
impairment_location=router-namespace
EOF
printf 'case,trial,bps,mbps,peak_rss_kib,cpu_seconds\n' > "$OUT/results.csv"

proc_ticks() {
  local pid=$1
  awk '{print $14+$15}' "/proc/$pid/stat"
}

wait_ready() {
  local pid=$1 log=$2
  for _ in $(seq 1 100); do
    if grep -q 'ready:' "$log" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "tcp-shift exited before becoming ready" >&2
      cat "$log" >&2 || true
      return 1
    fi
    sleep 0.05
  done
  echo "timed out waiting for tcp-shift" >&2
  cat "$log" >&2 || true
  return 1
}

run_case() {
  local name=$1 engine=$2 cc=$3 listen=$4 trial=$5
  local prefix="$OUT/${name}-${trial}"

  echo "=== case=$name trial=$trial engine=$engine cc=$cc recovery=$RECOVERY data_loss=$DATA_LOSS ack_loss=$ACK_LOSS ==="
  configure_netem

  iperf3 -s -1 -B 127.0.0.1 -p 5202 --json >"${prefix}-server.json" 2>"${prefix}-server.err" &
  local backend_pid=$!

  "$BIN" \
    --engine "$engine" \
    --tun "$TUN_IF" \
    --listen "$listen" \
    --backend 127.0.0.1:5202 \
    --cc "$cc" \
    --recovery "$RECOVERY" \
    --tcp-buffer-mib "$TCP_BUFFER_MIB" \
    --stats-interval 1s \
    >"${prefix}-proxy.log" 2>&1 &
  local proxy_pid=$!

  wait_ready "$proxy_pid" "${prefix}-proxy.log"
  local hz start_ticks end_ticks
  hz=$(getconf CLK_TCK)
  start_ticks=$(proc_ticks "$proxy_pid")

  local target=${listen%:*}
  if ! timeout --signal=TERM "${CASE_TIMEOUT}s" \
      ip netns exec "$CLIENT_NS" iperf3 \
        -c "$target" -p 5201 -R -t "$DURATION" --json \
        >"${prefix}-client.json" 2>"${prefix}-client.err"; then
    echo "iperf3 failed or exceeded ${CASE_TIMEOUT}s for $name trial $trial" >&2
    dump_network_state "${name}-${trial}-failure"
    cat "${prefix}-client.err" >&2 || true
    cat "${prefix}-proxy.log" >&2 || true
    kill -TERM "$proxy_pid" 2>/dev/null || true
    wait_pid_bounded "$proxy_pid" 3
    wait_pid_bounded "$backend_pid" 3
    return 1
  fi

  dump_network_state "${name}-${trial}-post"

  if ! kill -0 "$proxy_pid" 2>/dev/null; then
    echo "tcp-shift died during ${name} trial ${trial}" >&2
    cat "${prefix}-proxy.log" >&2 || true
    wait_pid_bounded "$backend_pid" 3
    return 1
  fi

  end_ticks=$(proc_ticks "$proxy_pid")
  local peak_rss
  peak_rss=$(awk '/VmHWM:/ {print $2}' "/proc/$proxy_pid/status")
  peak_rss=${peak_rss:-0}

  kill -TERM "$proxy_pid" 2>/dev/null || true
  wait_pid_bounded "$proxy_pid" 3
  wait_pid_bounded "$backend_pid" 3

  local bps mbps cpu
  bps=$(jq -r '.end.sum_received.bits_per_second // .end.sum.bits_per_second // 0' "${prefix}-client.json")
  mbps=$(awk -v b="$bps" 'BEGIN { printf "%.3f", b/1000000.0 }')
  cpu=$(awk -v a="$start_ticks" -v b="$end_ticks" -v h="$hz" 'BEGIN { printf "%.3f", (b-a)/h }')
  printf '%s,%d,%s,%s,%s,%s\n' "$name" "$trial" "$bps" "$mbps" "$peak_rss" "$cpu" | tee -a "$OUT/results.csv"
}

for trial in $(seq 1 "$TRIALS"); do
  for case_name in $BENCH_CASES; do
    case "$case_name" in
      native-cubic)
        run_case native-cubic native cubic "$NATIVE_ADDR:5201" "$trial"
        ;;
      native-bbr)
        run_case native-bbr native bbr "$NATIVE_ADDR:5201" "$trial"
        ;;
      gvisor-cubic)
        run_case gvisor-cubic netstack cubic "$GVISOR_ADDR:5201" "$trial"
        ;;
      gvisor-bbr)
        run_case gvisor-bbr netstack bbr "$GVISOR_ADDR:5201" "$trial"
        ;;
    esac
  done
done

python3 "$ROOT/scripts/summarize_bench.py" "$OUT/results.csv" "$OUT/summary.md" "$OUT/summary.json"
cat "$OUT/summary.md"
