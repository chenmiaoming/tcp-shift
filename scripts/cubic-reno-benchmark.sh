#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
BINARY=${TCP_SHIFT_CC_BENCH_BINARY:-"$BUILD/tcp-shift-p2"}
CASE=${TCP_SHIFT_CC_BENCH_CASE:-wan}
RTT_MS=${TCP_SHIFT_CC_BENCH_RTT_MS:-40}
RATE_MBIT=${TCP_SHIFT_CC_BENCH_RATE_MBIT:-10}
LOSS_PCT=${TCP_SHIFT_CC_BENCH_LOSS_PCT:-0}
PAYLOAD_BYTES=${TCP_SHIFT_CC_BENCH_PAYLOAD_BYTES:-4194304}
OUT=${TCP_SHIFT_CC_BENCH_OUT:-"$BUILD/cubic-reno-benchmark/$CASE"}

LWIP_IP=10.254.0.2
HOST_IP=10.254.0.1
NETMASK=255.255.255.252
PUBLIC_PORT=18742
BACKEND_PORT=19742

ACTIVE_TUN=
ACTIVE_IFB=
ACTIVE_CHAIN=
RUNTIME_PID=
BACKEND_PID=
LOSS_INSTALLED=0

mkdir -p "$OUT/cubic" "$OUT/reno"

[ "$(id -u)" -eq 0 ] || {
    echo "CUBIC/Reno benchmark requires root for TUN, IFB and netem" >&2
    exit 1
}
[ -x "$BINARY" ] || { echo "missing benchmark binary: $BINARY" >&2; exit 1; }
command -v tc >/dev/null 2>&1 || { echo "tc is required" >&2; exit 1; }
command -v ip >/dev/null 2>&1 || { echo "ip is required" >&2; exit 1; }
command -v iptables >/dev/null 2>&1 || { echo "iptables is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 1; }

case "$RTT_MS" in ''|*[!0-9]*) echo "RTT_MS must be an integer" >&2; exit 1;; esac
case "$RATE_MBIT" in ''|*[!0-9]*) echo "RATE_MBIT must be an integer" >&2; exit 1;; esac
case "$PAYLOAD_BYTES" in ''|*[!0-9]*) echo "PAYLOAD_BYTES must be an integer" >&2; exit 1;; esac
case "$LOSS_PCT" in ''|*[!0-9.]*|*.*.*) echo "LOSS_PCT must be a nonnegative decimal" >&2; exit 1;; esac
[ "$RTT_MS" -gt 0 ] && [ $((RTT_MS % 2)) -eq 0 ] || {
    echo "RTT_MS must be a positive even integer" >&2
    exit 1
}
[ "$RATE_MBIT" -gt 0 ] || { echo "RATE_MBIT must be positive" >&2; exit 1; }
[ "$PAYLOAD_BYTES" -gt 0 ] || { echo "PAYLOAD_BYTES must be positive" >&2; exit 1; }

HALF_RTT_MS=$((RTT_MS / 2))
BDP_BYTES=$((RATE_MBIT * RTT_MS * 125))
QUEUE_PKTS=${TCP_SHIFT_CC_BENCH_QUEUE_PKTS:-$(((BDP_BYTES + 1459) / 1460))}
[ "$QUEUE_PKTS" -ge 16 ] || QUEUE_PKTS=16

if [ "$LOSS_PCT" = 0 ] || [ "$LOSS_PCT" = 0.0 ]; then
    LOSS_EVERY=0
    LOSS_PACKET=0
    LOSS_MODE=none
else
    LOSS_EVERY=$(python3 - "$LOSS_PCT" <<'PY'
import sys
pct = float(sys.argv[1])
if not (0.0 < pct < 100.0):
    raise SystemExit("LOSS_PCT must be in (0,100) for a loss case")
print(max(2, int(round(100.0 / pct))))
PY
)
    LOSS_PACKET=$((LOSS_EVERY - 1))
    LOSS_MODE=periodic_nth
fi

stop_pid()
{
    pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" >/dev/null 2>&1 || true
        wait "$pid" >/dev/null 2>&1 || true
    fi
}

cleanup_active()
{
    set +e
    if [ "$LOSS_INSTALLED" -eq 1 ] && [ -n "$ACTIVE_CHAIN" ]; then
        iptables -D INPUT -i "$ACTIVE_TUN" -j "$ACTIVE_CHAIN" >/dev/null 2>&1 || true
        iptables -F "$ACTIVE_CHAIN" >/dev/null 2>&1 || true
        iptables -X "$ACTIVE_CHAIN" >/dev/null 2>&1 || true
    fi
    LOSS_INSTALLED=0
    stop_pid "${RUNTIME_PID:-}"
    stop_pid "${BACKEND_PID:-}"
    RUNTIME_PID=
    BACKEND_PID=
    if [ -n "$ACTIVE_TUN" ]; then
        tc qdisc del dev "$ACTIVE_TUN" root >/dev/null 2>&1 || true
        tc qdisc del dev "$ACTIVE_TUN" ingress >/dev/null 2>&1 || true
        ip link del "$ACTIVE_TUN" >/dev/null 2>&1 || true
    fi
    if [ -n "$ACTIVE_IFB" ]; then
        tc qdisc del dev "$ACTIVE_IFB" root >/dev/null 2>&1 || true
        ip link del "$ACTIVE_IFB" >/dev/null 2>&1 || true
    fi
    ACTIVE_TUN=
    ACTIVE_IFB=
    ACTIVE_CHAIN=
    set -e
}
trap cleanup_active EXIT HUP INT TERM

run_receiver()
{
    output=$1
    python3 - "$LWIP_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" > "$output" <<'PY'
import hashlib
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
expected = int(sys.argv[3])
start = time.monotonic_ns()
received = 0
digest = hashlib.sha256()
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(60.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
    sock.connect((host, port))
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        received += len(chunk)
        digest.update(chunk)
elapsed = time.monotonic_ns() - start
if received != expected:
    raise SystemExit(f"received={received} expected={expected}")
goodput_mbps = received * 8.0 * 1000.0 / elapsed
print(
    f"bytes={received} elapsed_ns={elapsed} goodput_mbps={goodput_mbps:.6f} "
    f"sha256={digest.hexdigest()}"
)
PY
}

run_controller()
{
    cc=$1
    tag=$2
    dir="$OUT/$cc"

    cleanup_active
    ACTIVE_TUN="tc${tag}$$"
    ACTIVE_IFB="ti${tag}$$"
    ACTIVE_CHAIN="TCCR${tag}$$"
    mkdir -p "$dir"
    : > "$dir/backend.stdout"
    : > "$dir/backend.stderr"
    : > "$dir/runtime.stdout"
    : > "$dir/runtime.stderr"

    python3 - "$BACKEND_PORT" "$PAYLOAD_BYTES" \
        > "$dir/backend.stdout" 2> "$dir/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
length = int(sys.argv[2])
payload = bytes(((i * 73 + 19) & 0xff) for i in range(length))
digest = hashlib.sha256(payload).hexdigest()
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready port={port}", flush=True)
conn, _ = server.accept()
conn.settimeout(60.0)
conn.sendall(payload)
conn.shutdown(socket.SHUT_WR)
while conn.recv(4096):
    pass
conn.close(); server.close()
print(f"backend-bytes={length} sha256={digest}", flush=True)
PY
    BACKEND_PID=$!

    i=0
    while [ "$i" -lt 100 ] && ! grep -F 'backend-ready ' "$dir/backend.stdout" >/dev/null 2>&1; do
        kill -0 "$BACKEND_PID" 2>/dev/null || { cat "$dir/backend.stderr" >&2 || true; exit 1; }
        i=$((i + 1)); sleep 0.05
    done
    grep -F 'backend-ready ' "$dir/backend.stdout" >/dev/null || {
        echo "$cc backend ready timeout" >&2; exit 1;
    }

    "$BINARY" "$ACTIVE_TUN" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
        "$PUBLIC_PORT" "$BACKEND_PORT" "$cc" \
        > "$dir/runtime.stdout" 2> "$dir/runtime.stderr" &
    RUNTIME_PID=$!

    i=0
    while [ "$i" -lt 100 ]; do
        if ip link show "$ACTIVE_TUN" >/dev/null 2>&1 && \
           grep -F "tcp-shift-p2: ready tun=$ACTIVE_TUN" "$dir/runtime.stdout" >/dev/null 2>&1; then
            break
        fi
        kill -0 "$RUNTIME_PID" 2>/dev/null || { cat "$dir/runtime.stderr" >&2 || true; exit 1; }
        i=$((i + 1)); sleep 0.05
    done
    [ "$i" -lt 100 ] || { echo "$cc runtime ready timeout" >&2; exit 1; }
    grep -F "cc=$cc" "$dir/runtime.stdout" >/dev/null

    modprobe ifb >/dev/null 2>&1 || true
    ip link add "$ACTIVE_IFB" type ifb
    ip link set "$ACTIVE_IFB" up
    tc qdisc add dev "$ACTIVE_TUN" handle ffff: ingress
    tc filter add dev "$ACTIVE_TUN" parent ffff: protocol ip prio 1 u32 \
        match u32 0 0 action mirred egress redirect dev "$ACTIVE_IFB"
    tc qdisc replace dev "$ACTIVE_IFB" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
    tc qdisc replace dev "$ACTIVE_TUN" root netem \
        delay "${HALF_RTT_MS}ms" limit "$QUEUE_PKTS"

    if [ "$LOSS_EVERY" -gt 0 ]; then
        iptables -N "$ACTIVE_CHAIN"
        iptables -I INPUT 1 -i "$ACTIVE_TUN" -j "$ACTIVE_CHAIN"
        iptables -A "$ACTIVE_CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
            -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 \
            -m statistic --mode nth --every "$LOSS_EVERY" --packet "$LOSS_PACKET" \
            -j DROP
        iptables -A "$ACTIVE_CHAIN" -j RETURN
        LOSS_INSTALLED=1
    fi

    tc -s qdisc show dev "$ACTIVE_TUN" > "$dir/tun-qdisc-before.txt"
    tc -s qdisc show dev "$ACTIVE_IFB" > "$dir/ifb-qdisc-before.txt"
    if [ "$LOSS_EVERY" -gt 0 ]; then
        iptables -nvxL "$ACTIVE_CHAIN" > "$dir/loss-rule-before.txt"
    fi

    run_receiver "$dir/client.txt"
    wait "$BACKEND_PID" || { BACKEND_PID=; cat "$dir/backend.stderr" >&2 || true; exit 1; }
    BACKEND_PID=

    tc -s qdisc show dev "$ACTIVE_TUN" > "$dir/tun-qdisc-after.txt"
    tc -s qdisc show dev "$ACTIVE_IFB" > "$dir/ifb-qdisc-after.txt"
    if [ "$LOSS_EVERY" -gt 0 ]; then
        iptables -nvxL "$ACTIVE_CHAIN" > "$dir/loss-rule-after.txt"
        forced=$(awk '$3 == "DROP" {print $1; exit}' "$dir/loss-rule-after.txt")
        [ -n "$forced" ] && [ "$forced" -gt 0 ] || {
            cat "$dir/loss-rule-after.txt" >&2
            echo "$cc deterministic loss rule dropped no data packet" >&2
            exit 1
        }
    else
        forced=0
    fi

    sleep 0.2
    kill -TERM "$RUNTIME_PID"
    wait "$RUNTIME_PID" || { RUNTIME_PID=; cat "$dir/runtime.stderr" >&2 || true; exit 1; }
    RUNTIME_PID=

    grep -F 'cc_controller_errors=0' "$dir/runtime.stderr" >/dev/null
    goodput=$(sed -n 's/.* goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$dir/client.txt")
    cwnd=$(sed -n 's/.* cc_last_cwnd=\([0-9][0-9]*\).*/\1/p' "$dir/runtime.stderr" | tail -n 1)
    losses=$(sed -n 's/.* cc_loss_events=\([0-9][0-9]*\).*/\1/p' "$dir/runtime.stderr" | tail -n 1)
    timeouts=$(sed -n 's/.* cc_timeout_events=\([0-9][0-9]*\).*/\1/p' "$dir/runtime.stderr" | tail -n 1)
    [ -n "$goodput" ] && [ -n "$cwnd" ] && [ -n "$losses" ] && [ -n "$timeouts" ] || {
        echo "missing $cc benchmark metrics" >&2; exit 1;
    }

    printf 'goodput_mbps=%s\ncwnd_bytes=%s\nloss_events=%s\ntimeout_events=%s\nforced_drops=%s\n' \
        "$goodput" "$cwnd" "$losses" "$timeouts" "$forced" > "$dir/metrics.env"
    cleanup_active
}

run_controller cubic c
run_controller reno r

CUBIC_GOODPUT=$(sed -n 's/^goodput_mbps=//p' "$OUT/cubic/metrics.env")
CUBIC_CWND=$(sed -n 's/^cwnd_bytes=//p' "$OUT/cubic/metrics.env")
CUBIC_LOSS=$(sed -n 's/^loss_events=//p' "$OUT/cubic/metrics.env")
CUBIC_TIMEOUT=$(sed -n 's/^timeout_events=//p' "$OUT/cubic/metrics.env")
CUBIC_FORCED=$(sed -n 's/^forced_drops=//p' "$OUT/cubic/metrics.env")
RENO_GOODPUT=$(sed -n 's/^goodput_mbps=//p' "$OUT/reno/metrics.env")
RENO_CWND=$(sed -n 's/^cwnd_bytes=//p' "$OUT/reno/metrics.env")
RENO_LOSS=$(sed -n 's/^loss_events=//p' "$OUT/reno/metrics.env")
RENO_TIMEOUT=$(sed -n 's/^timeout_events=//p' "$OUT/reno/metrics.env")
RENO_FORCED=$(sed -n 's/^forced_drops=//p' "$OUT/reno/metrics.env")

if [ "$LOSS_EVERY" -gt 0 ] && [ "$CUBIC_FORCED" -ne "$RENO_FORCED" ]; then
    echo "forced drop mismatch cubic=$CUBIC_FORCED reno=$RENO_FORCED" >&2
    exit 1
fi

python3 - "$CASE" "$RTT_MS" "$RATE_MBIT" "$LOSS_PCT" "$LOSS_MODE" "$LOSS_EVERY" \
    "$PAYLOAD_BYTES" "$BDP_BYTES" "$QUEUE_PKTS" \
    "$CUBIC_GOODPUT" "$RENO_GOODPUT" "$CUBIC_CWND" "$RENO_CWND" \
    "$CUBIC_LOSS" "$RENO_LOSS" "$CUBIC_TIMEOUT" "$RENO_TIMEOUT" \
    "$CUBIC_FORCED" "$RENO_FORCED" <<'PY' | tee "$OUT/summary.txt"
import sys
(case, rtt_ms, rate_mbit, loss_pct, loss_mode, loss_every, payload, bdp,
 queue_pkts, cubic_g, reno_g, cubic_cwnd, reno_cwnd, cubic_loss, reno_loss,
 cubic_timeout, reno_timeout, cubic_forced, reno_forced) = sys.argv[1:]
cubic = float(cubic_g)
reno = float(reno_g)
ratio = cubic / reno if reno else 0.0
print(
    f"cubic_reno_benchmark=ok case={case} base_rtt_ms={rtt_ms} rate_mbit={rate_mbit} "
    f"loss_pct={loss_pct} loss_mode={loss_mode} loss_every={loss_every} "
    f"payload_bytes={payload} bdp_bytes={bdp} queue_pkts={queue_pkts} "
    f"cubic_goodput_mbps={cubic:.6f} reno_goodput_mbps={reno:.6f} ratio={ratio:.6f} "
    f"cubic_cwnd_bytes={cubic_cwnd} reno_cwnd_bytes={reno_cwnd} "
    f"cubic_loss_events={cubic_loss} reno_loss_events={reno_loss} "
    f"cubic_timeout_events={cubic_timeout} reno_timeout_events={reno_timeout} "
    f"cubic_forced_drops={cubic_forced} reno_forced_drops={reno_forced}"
)
PY

printf 'base_rtt_ms=%s\nrate_mbit=%s\nloss_pct=%s\nloss_mode=%s\nloss_every=%s\nbdp_bytes=%s\nqueue_pkts=%s\n' \
    "$RTT_MS" "$RATE_MBIT" "$LOSS_PCT" "$LOSS_MODE" "$LOSS_EVERY" "$BDP_BYTES" "$QUEUE_PKTS" \
    > "$OUT/path.env"
echo "tcp-shift CUBIC/Reno benchmark case $CASE completed"
