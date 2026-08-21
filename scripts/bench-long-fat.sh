#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "bench-long-fat.sh must run as root (use sudo -E)" >&2
  exit 1
fi

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/bin/tcp-shift"
OUT="$ROOT/.bench"
CLIENT_NS=${CLIENT_NS:-tcpshift-client}
ROOT_IF=${ROOT_IF:-ts-veth0}
CLIENT_IF=${CLIENT_IF:-ts-veth1}
TUN_IF=${TUN_IF:-ts0}
RATE=${RATE:-50mbit}
ONE_WAY_DELAY=${ONE_WAY_DELAY:-50ms}
LOSS=${LOSS:-0.10%}
DURATION=${DURATION:-12}
TRIALS=${TRIALS:-2}
TCP_BUFFER_MIB=${TCP_BUFFER_MIB:-4}

mkdir -p "$OUT"
rm -f "$OUT"/*

test -x "$BIN" || { echo "missing $BIN; run scripts/build.sh first" >&2; exit 1; }
command -v iperf3 >/dev/null
command -v tc >/dev/null
command -v jq >/dev/null

cleanup() {
  set +e
  pkill -f "$BIN" >/dev/null 2>&1 || true
  pkill -f 'iperf3 -s -1 -B 127.0.0.1 -p 5202' >/dev/null 2>&1 || true
  ip netns del "$CLIENT_NS" >/dev/null 2>&1 || true
  ip link del "$ROOT_IF" >/dev/null 2>&1 || true
  ip link del "$TUN_IF" >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup

sysctl -q -w net.ipv4.ip_forward=1
sysctl -q -w net.ipv4.tcp_congestion_control=cubic || true

ip netns add "$CLIENT_NS"
ip link add "$ROOT_IF" type veth peer name "$CLIENT_IF"
ip link set "$CLIENT_IF" netns "$CLIENT_NS"
ip addr add 10.99.0.1/24 dev "$ROOT_IF"
ip link set "$ROOT_IF" up
ip netns exec "$CLIENT_NS" ip link set lo up
ip netns exec "$CLIENT_NS" ip addr add 10.99.0.3/24 dev "$CLIENT_IF"
ip netns exec "$CLIENT_NS" ip link set "$CLIENT_IF" up
# 10.99.0.2 is owned by gVisor, not by the host veth. Force the client to use
# the root namespace as the router for that /32 rather than ARPing for it.
ip netns exec "$CLIENT_NS" ip route add 10.99.0.2/32 via 10.99.0.1 dev "$CLIENT_IF"

ip tuntap add dev "$TUN_IF" mode tun
ip link set "$TUN_IF" mtu 1500 up
ip route add 10.99.0.2/32 dev "$TUN_IF"

# Shape each egress direction. The resulting RTT is approximately 2*delay and
# both data and ACK paths see the configured bottleneck/loss model.
tc qdisc add dev "$ROOT_IF" root netem delay "$ONE_WAY_DELAY" rate "$RATE" loss "$LOSS"
ip netns exec "$CLIENT_NS" tc qdisc add dev "$CLIENT_IF" root netem delay "$ONE_WAY_DELAY" rate "$RATE" loss "$LOSS"

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

  iperf3 -s -1 -B 127.0.0.1 -p 5202 --json >"${prefix}-server.json" 2>"${prefix}-server.err" &
  local backend_pid=$!

  "$BIN" \
    --engine "$engine" \
    --tun "$TUN_IF" \
    --listen "$listen" \
    --backend 127.0.0.1:5202 \
    --cc "$cc" \
    --tcp-buffer-mib "$TCP_BUFFER_MIB" \
    --stats-interval 1s \
    >"${prefix}-proxy.log" 2>&1 &
  local proxy_pid=$!

  wait_ready "$proxy_pid" "${prefix}-proxy.log"
  local hz start_ticks end_ticks
  hz=$(getconf CLK_TCK)
  start_ticks=$(proc_ticks "$proxy_pid")

  local target=${listen%:*}
  ip netns exec "$CLIENT_NS" iperf3 \
    -c "$target" -p 5201 -R -t "$DURATION" --json \
    >"${prefix}-client.json" 2>"${prefix}-client.err"

  end_ticks=$(proc_ticks "$proxy_pid")
  local peak_rss
  peak_rss=$(awk '/VmHWM:/ {print $2}' "/proc/$proxy_pid/status")
  peak_rss=${peak_rss:-0}

  kill -TERM "$proxy_pid" 2>/dev/null || true
  wait "$proxy_pid" 2>/dev/null || true
  wait "$backend_pid" 2>/dev/null || true

  local bps mbps cpu
  bps=$(jq -r '.end.sum_received.bits_per_second // .end.sum.bits_per_second // 0' "${prefix}-client.json")
  mbps=$(awk -v b="$bps" 'BEGIN { printf "%.3f", b/1000000.0 }')
  cpu=$(awk -v a="$start_ticks" -v b="$end_ticks" -v h="$hz" 'BEGIN { printf "%.3f", (b-a)/h }')
  printf '%s,%d,%s,%s,%s,%s\n' "$name" "$trial" "$bps" "$mbps" "$peak_rss" "$cpu" | tee -a "$OUT/results.csv"
}

for trial in $(seq 1 "$TRIALS"); do
  # Same Go relay, but the frontend TCP socket is the host kernel. This gives a
  # useful lower-bound for incremental netstack memory/CPU overhead.
  run_case native-cubic native cubic 10.99.0.1:5201 "$trial"
  run_case gvisor-cubic netstack cubic 10.99.0.2:5201 "$trial"
  run_case gvisor-bbr netstack bbr 10.99.0.2:5201 "$trial"
done

python3 "$ROOT/scripts/summarize_bench.py" "$OUT/results.csv" "$OUT/summary.md" "$OUT/summary.json"
cat "$OUT/summary.md"
