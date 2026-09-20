#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
BINARY=${TCP_SHIFT_P6_BBR_APP_BINARY:-"$BUILD/tcp-shift-p6-bbr"}
RTT_MS=${TCP_SHIFT_P6_BBR_APP_RTT_MS:-40}
RATE_MBIT=${TCP_SHIFT_P6_BBR_APP_RATE_MBIT:-10}
BURST_BYTES=${TCP_SHIFT_P6_BBR_APP_BURST_BYTES:-524288}
IDLE_MS=${TCP_SHIFT_P6_BBR_APP_IDLE_MS:-1000}
OUT=${TCP_SHIFT_P6_BBR_APP_OUT:-"$BUILD/p6-bbr-app-limited"}

TUN_NAME=${TCP_SHIFT_P6_BBR_APP_TUN_NAME:-"tsp6al$$"}
IFB_NAME=${TCP_SHIFT_P6_BBR_APP_IFB_NAME:-"p6aifb$$"}
LWIP_IP=${TCP_SHIFT_P6_BBR_APP_LWIP_IP:-10.249.0.2}
HOST_IP=${TCP_SHIFT_P6_BBR_APP_HOST_IP:-10.249.0.1}
NETMASK=${TCP_SHIFT_P6_BBR_APP_NETMASK:-255.255.255.252}
PUBLIC_PORT=${TCP_SHIFT_P6_BBR_APP_PUBLIC_PORT:-18164}
BACKEND_PORT=${TCP_SHIFT_P6_BBR_APP_BACKEND_PORT:-19164}

RUNTIME_PID=
BACKEND_PID=

mkdir -p "$OUT"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"

[ "$(id -u)" -eq 0 ] || {
    echo "P6 BBR app-limited harness must run as root for TUN, IFB and netem" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing internal BBR qualification binary: $BINARY" >&2
    exit 1
}
command -v ip >/dev/null 2>&1 || { echo "ip is required" >&2; exit 1; }
command -v tc >/dev/null 2>&1 || { echo "tc is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 1; }

for value_name in RTT_MS RATE_MBIT BURST_BYTES IDLE_MS; do
    eval value=\$$value_name
    case "$value" in ''|*[!0-9]*) echo "$value_name must be an integer" >&2; exit 1;; esac
done
[ "$RTT_MS" -gt 0 ] && [ $((RTT_MS % 2)) -eq 0 ] || {
    echo "RTT_MS must be a positive even integer" >&2
    exit 1
}
[ "$RATE_MBIT" -gt 0 ] || { echo "RATE_MBIT must be positive" >&2; exit 1; }
[ "$BURST_BYTES" -gt 0 ] || { echo "BURST_BYTES must be positive" >&2; exit 1; }
[ "$IDLE_MS" -ge 200 ] || { echo "IDLE_MS must be at least 200" >&2; exit 1; }

HALF_RTT_MS=$((RTT_MS / 2))
BDP_BYTES=$((RATE_MBIT * RTT_MS * 125))
BDP_PKTS=$(((BDP_BYTES + 1459) / 1460))
QUEUE_PKTS=${TCP_SHIFT_P6_BBR_APP_QUEUE_PKTS:-$((BDP_PKTS * 8))}
[ "$QUEUE_PKTS" -ge 64 ] || QUEUE_PKTS=64
TOTAL_BYTES=$((BURST_BYTES * 2))

stop_pid()
{
    pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" >/dev/null 2>&1 || true
        wait "$pid" >/dev/null 2>&1 || true
    fi
}

cleanup()
{
    set +e
    stop_pid "${RUNTIME_PID:-}"
    stop_pid "${BACKEND_PID:-}"
    tc qdisc del dev "$TUN_NAME" root >/dev/null 2>&1 || true
    tc qdisc del dev "$TUN_NAME" ingress >/dev/null 2>&1 || true
    tc qdisc del dev "$IFB_NAME" root >/dev/null 2>&1 || true
    ip link del "$IFB_NAME" >/dev/null 2>&1 || true
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

python3 - "$BACKEND_PORT" "$BURST_BYTES" "$IDLE_MS" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys
import time

port = int(sys.argv[1])
burst_bytes = int(sys.argv[2])
idle_ms = int(sys.argv[3])

burst1 = bytes(((index * 73 + 19) & 0xFF) for index in range(burst_bytes))
burst2 = bytes(((index * 29 + 101) & 0xFF) for index in range(burst_bytes))

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(
    f"backend-ready port={port} burst_bytes={burst_bytes} idle_ms={idle_ms}",
    flush=True,
)
conn, _ = server.accept()
conn.settimeout(90.0)
conn.sendall(burst1)
first_done_ns = time.monotonic_ns()
time.sleep(idle_ms / 1000.0)
second_start_ns = time.monotonic_ns()
conn.sendall(burst2)
conn.shutdown(socket.SHUT_WR)
while conn.recv(4096):
    pass
conn.close()
server.close()
print(
    f"backend-complete total_bytes={burst_bytes * 2} "
    f"backend_idle_ns={second_start_ns - first_done_ns} "
    f"burst1_sha256={hashlib.sha256(burst1).hexdigest()} "
    f"burst2_sha256={hashlib.sha256(burst2).hexdigest()}",
    flush=True,
)
PY
BACKEND_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$BACKEND_PID" 2>/dev/null || {
        cat "$OUT/backend.stderr" >&2 || true
        echo "P6 BBR app-limited backend exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
grep -F "backend-ready port=$BACKEND_PORT " "$OUT/backend.stdout" >/dev/null || {
    echo "timed out waiting for P6 BBR app-limited backend" >&2
    exit 1
}

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" bbr-internal \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
RUNTIME_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p2: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null 2>&1 &&
       grep -F 'cc=bbr-internal' "$OUT/runtime.stdout" >/dev/null 2>&1; then
        break
    fi
    kill -0 "$RUNTIME_PID" 2>/dev/null || {
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "P6 BBR app-limited runtime exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
[ "$i" -lt 100 ] || {
    echo "timed out waiting for P6 BBR app-limited runtime" >&2
    exit 1
}

modprobe ifb >/dev/null 2>&1 || true
ip link add "$IFB_NAME" type ifb
ip link set "$IFB_NAME" up
tc qdisc add dev "$TUN_NAME" handle ffff: ingress
tc filter add dev "$TUN_NAME" parent ffff: protocol ip prio 1 u32 \
    match u32 0 0 action mirred egress redirect dev "$IFB_NAME"
tc qdisc replace dev "$IFB_NAME" root netem \
    delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
tc qdisc replace dev "$TUN_NAME" root netem \
    delay "${HALF_RTT_MS}ms" limit "$QUEUE_PKTS"

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$BURST_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import hashlib
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
burst_bytes = int(sys.argv[3])

expected1 = bytes(((index * 73 + 19) & 0xFF) for index in range(burst_bytes))
expected2 = bytes(((index * 29 + 101) & 0xFF) for index in range(burst_bytes))


def recv_exact(sock, length):
    chunks = []
    remaining = length
    while remaining:
        chunk = sock.recv(min(65536, remaining))
        if not chunk:
            raise RuntimeError(f"early EOF with {remaining} bytes remaining")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(90.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
    sock.connect((host, port))
    start_ns = time.monotonic_ns()
    burst1 = recv_exact(sock, burst_bytes)
    first_done_ns = time.monotonic_ns()
    first_second = sock.recv(1)
    if not first_second:
        raise RuntimeError("EOF before second burst")
    second_first_ns = time.monotonic_ns()
    burst2 = first_second + recv_exact(sock, burst_bytes - 1)
    if sock.recv(1):
        raise RuntimeError("unexpected tail data")
    elapsed_ns = time.monotonic_ns() - start_ns

if burst1 != expected1 or burst2 != expected2:
    raise SystemExit("app-limited payload mismatch")

print(
    f"app-limited-client total_bytes={burst_bytes * 2} "
    f"elapsed_ns={elapsed_ns} receiver_idle_gap_ns={second_first_ns - first_done_ns} "
    f"goodput_mbps={burst_bytes * 2 * 8.0 * 1000.0 / elapsed_ns:.6f} "
    f"burst1_sha256={hashlib.sha256(burst1).hexdigest()} "
    f"burst2_sha256={hashlib.sha256(burst2).hexdigest()}"
)
PY

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stderr" >&2 || true
    cat "$OUT/client.stderr" >&2 || true
    echo "P6 BBR app-limited backend failed" >&2
    exit 1
fi
BACKEND_PID=

tc -s qdisc show dev "$TUN_NAME" > "$OUT/tun-qdisc-after.txt"
tc -s qdisc show dev "$IFB_NAME" > "$OUT/ifb-qdisc-after.txt"
ifb_drops=$(sed -n 's/.*(dropped \([0-9][0-9]*\),.*/\1/p' "$OUT/ifb-qdisc-after.txt" | head -n 1)
tun_drops=$(sed -n 's/.*(dropped \([0-9][0-9]*\),.*/\1/p' "$OUT/tun-qdisc-after.txt" | head -n 1)
[ -n "$ifb_drops" ] && [ "$ifb_drops" -eq 0 ] &&
[ -n "$tun_drops" ] && [ "$tun_drops" -eq 0 ] || {
    echo "P6 BBR app-limited qdisc dropped packets: ifb=${ifb_drops:-missing} tun=${tun_drops:-missing}" >&2
    exit 1
}

sleep 0.2
kill -TERM "$RUNTIME_PID"
if ! wait "$RUNTIME_PID"; then
    RUNTIME_PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P6 BBR app-limited runtime failed" >&2
    exit 1
fi
RUNTIME_PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F 'cc_bindings=1' "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_bind_failures=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_controller_errors=0' "$OUT/runtime.stderr" >/dev/null

events=$(grep -m1 ' cc_bindings=' "$OUT/runtime.stderr")
loss_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_loss_events=\([0-9][0-9]*\).*/\1/p')
timeout_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_timeout_events=\([0-9][0-9]*\).*/\1/p')
[ -n "$loss_events" ] && [ "$loss_events" -eq 0 ] &&
[ -n "$timeout_events" ] && [ "$timeout_events" -eq 0 ] || {
    echo "app-limited clean path entered recovery" >&2
    exit 1
}

rate=$(grep -m1 'tcp-shift-p2-rate:' "$OUT/runtime.stderr")
app_samples=$(printf '%s\n' "$rate" | sed -n 's/.* app_limited_samples=\([0-9][0-9]*\).*/\1/p')
app_enters=$(printf '%s\n' "$rate" | sed -n 's/.* app_limited_enters=\([0-9][0-9]*\).*/\1/p')
app_exits=$(printf '%s\n' "$rate" | sed -n 's/.* app_limited_exits=\([0-9][0-9]*\).*/\1/p')
valid_samples=$(printf '%s\n' "$rate" | sed -n 's/.* valid_samples=\([0-9][0-9]*\).*/\1/p')
[ -n "$app_samples" ] && [ "$app_samples" -ge 1 ] &&
[ -n "$app_enters" ] && [ "$app_enters" -ge 1 ] &&
[ -n "$app_exits" ] && [ "$app_exits" -ge 1 ] &&
[ -n "$valid_samples" ] && [ "$valid_samples" -ge 1 ] || {
    echo "missing live app-limited rate semantics" >&2
    exit 1
}

delivery=$(grep -m1 'tcp-shift-p2-delivery:' "$OUT/runtime.stderr")
delivered_bytes=$(printf '%s\n' "$delivery" | sed -n 's/.* delivered_payload_bytes=\([0-9][0-9]*\).*/\1/p')
retransmit_events=$(printf '%s\n' "$delivery" | sed -n 's/.* retransmit_events=\([0-9][0-9]*\).*/\1/p')
[ -n "$delivered_bytes" ] && [ "$delivered_bytes" -eq "$TOTAL_BYTES" ] &&
[ -n "$retransmit_events" ] && [ "$retransmit_events" -eq 0 ] || {
    echo "invalid app-limited delivery telemetry" >&2
    exit 1
}

pacing=$(grep -m1 'tcp-shift-p2-pacing:' "$OUT/runtime.stderr")
pacing_deferrals=$(printf '%s\n' "$pacing" | sed -n 's/.* deferrals=\([0-9][0-9]*\).*/\1/p')
pacing_resumes=$(printf '%s\n' "$pacing" | sed -n 's/.* resume_events=\([0-9][0-9]*\).*/\1/p')
pacing_errors=$(printf '%s\n' "$pacing" | sed -n 's/.* scheduler_errors=\([0-9][0-9]*\).*/\1/p')
pacing_tx_bytes=$(printf '%s\n' "$pacing" | sed -n 's/.* tx_bytes=\([0-9][0-9]*\).*/\1/p')
[ -n "$pacing_deferrals" ] && [ "$pacing_deferrals" -ge 1 ] &&
[ -n "$pacing_resumes" ] && [ "$pacing_resumes" -ge 1 ] &&
[ -n "$pacing_errors" ] && [ "$pacing_errors" -eq 0 ] &&
[ -n "$pacing_tx_bytes" ] && [ "$pacing_tx_bytes" -eq "$TOTAL_BYTES" ] || {
    echo "invalid app-limited pacing telemetry" >&2
    exit 1
}

receiver_gap_ns=$(sed -n 's/.* receiver_idle_gap_ns=\([0-9][0-9]*\).*/\1/p' "$OUT/client.stdout")
backend_idle_ns=$(sed -n 's/.* backend_idle_ns=\([0-9][0-9]*\).*/\1/p' "$OUT/backend.stdout")
[ -n "$receiver_gap_ns" ] && [ -n "$backend_idle_ns" ] || {
    echo "missing app-limited idle timing" >&2
    exit 1
}

printf 'p6_bbr_app_limited=ok base_rtt_ms=%s rate_mbit=%s bdp_bytes=%s queue_pkts=%s burst_bytes=%s configured_idle_ms=%s backend_idle_ns=%s receiver_idle_gap_ns=%s app_limited_enters=%s app_limited_exits=%s app_limited_samples=%s valid_rate_samples=%s pacing_deferrals=%s pacing_resumes=%s qdisc_drops=%s/%s loss_events=%s timeout_events=%s payload_integrity=ok\n' \
    "$RTT_MS" "$RATE_MBIT" "$BDP_BYTES" "$QUEUE_PKTS" "$BURST_BYTES" "$IDLE_MS" \
    "$backend_idle_ns" "$receiver_gap_ns" "$app_enters" "$app_exits" "$app_samples" \
    "$valid_samples" "$pacing_deferrals" "$pacing_resumes" "$ifb_drops" "$tun_drops" \
    "$loss_events" "$timeout_events" | tee "$OUT/summary.txt"

echo "P6 internal BBR live app-limited qualification passed"
