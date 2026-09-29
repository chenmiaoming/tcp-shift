#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${TCP_SHIFT_ECN_BUILD:-"$ROOT/.build-ecn"}
BINARY=${TCP_SHIFT_ECN_BINARY:-"$BUILD/tcp-shift-p2"}
OUT=${TCP_SHIFT_ECN_PERSISTENT_OUT:-"$ROOT/.build/ecn-persistent-live"}
TUN_NAME=${TCP_SHIFT_ECN_PERSISTENT_TUN_NAME:-tsecp$$}
LWIP_IP=${TCP_SHIFT_ECN_PERSISTENT_LWIP_IP:-10.251.0.2}
HOST_IP=${TCP_SHIFT_ECN_PERSISTENT_HOST_IP:-10.251.0.1}
NETMASK=255.255.255.252
PUBLIC_PORT=${TCP_SHIFT_ECN_PERSISTENT_PUBLIC_PORT:-18817}
BACKEND_PORT=${TCP_SHIFT_ECN_PERSISTENT_BACKEND_PORT:-19817}
CHUNKS=${TCP_SHIFT_ECN_PERSISTENT_CHUNKS:-18}
CHUNK_BYTES=${TCP_SHIFT_ECN_PERSISTENT_CHUNK_BYTES:-1200}
CHAIN="TSECP$$"
RELEASE="$OUT/release-backend"

RUNTIME_PID=
BACKEND_PID=
CLIENT_PID=
ECN_SYSCTL_OLD=
RULE_INSTALLED=0

mkdir -p "$OUT"
rm -f "$RELEASE"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"

[ "$(id -u)" -eq 0 ] || {
    echo "persistent RFC 3168 qualification requires root" >&2
    exit 1
}
[ -x "$BINARY" ] || { echo "missing ECN binary: $BINARY" >&2; exit 1; }
for tool in ip iptables python3 sysctl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool is required for persistent RFC 3168 qualification" >&2
        exit 1
    }
done
case "$CHUNKS" in ''|*[!0-9]*) echo "CHUNKS must be an integer" >&2; exit 1;; esac
case "$CHUNK_BYTES" in ''|*[!0-9]*) echo "CHUNK_BYTES must be an integer" >&2; exit 1;; esac
[ "$CHUNKS" -ge 18 ] || { echo "CHUNKS must be >=18" >&2; exit 1; }
[ "$CHUNK_BYTES" -gt 0 ] && [ "$CHUNK_BYTES" -le 1200 ] || {
    echo "CHUNK_BYTES must be in 1..1200" >&2
    exit 1
}
EXPECTED=$((CHUNKS * CHUNK_BYTES))

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
    stop_pid "${CLIENT_PID:-}"
    stop_pid "${BACKEND_PID:-}"
    stop_pid "${RUNTIME_PID:-}"
    if [ "$RULE_INSTALLED" -eq 1 ]; then
        iptables -t mangle -D PREROUTING -i "$TUN_NAME" -j "$CHAIN" >/dev/null 2>&1 || true
        iptables -t mangle -F "$CHAIN" >/dev/null 2>&1 || true
        iptables -t mangle -X "$CHAIN" >/dev/null 2>&1 || true
    fi
    if [ -n "${ECN_SYSCTL_OLD:-}" ]; then
        sysctl -q -w "net.ipv4.tcp_ecn=$ECN_SYSCTL_OLD" >/dev/null 2>&1 || true
    fi
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

ECN_SYSCTL_OLD=$(sysctl -n net.ipv4.tcp_ecn)
sysctl -q -w net.ipv4.tcp_ecn=1

python3 - "$BACKEND_PORT" "$RELEASE" "$CHUNKS" "$CHUNK_BYTES" \
    >"$OUT/backend.stdout" 2>"$OUT/backend.stderr" <<'PY' &
import os
import socket
import sys
import time

port = int(sys.argv[1])
release = sys.argv[2]
chunks = int(sys.argv[3])
chunk_bytes = int(sys.argv[4])
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready port={port}", flush=True)
conn, _ = server.accept()
conn.settimeout(40.0)
print("backend-accepted", flush=True)
deadline = time.monotonic() + 10.0
while not os.path.exists(release):
    if time.monotonic() >= deadline:
        raise SystemExit("release timeout")
    time.sleep(0.01)

for index in range(chunks):
    payload = bytes(((index * 37 + offset * 19 + 5) & 0xff)
                    for offset in range(chunk_bytes))
    conn.sendall(payload)
    print(f"backend-chunk={index + 1} bytes={len(payload)}", flush=True)
    # Keep each CE/CWR exchange distinct enough for ACK processing to finish
    # before the following application write becomes available to tcp-shift.
    time.sleep(0.25)

conn.shutdown(socket.SHUT_WR)
while conn.recv(4096):
    pass
conn.close()
server.close()
print(f"backend-total={chunks * chunk_bytes}", flush=True)
PY
BACKEND_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$BACKEND_PID" 2>/dev/null || { cat "$OUT/backend.stderr" >&2; exit 1; }
    i=$((i + 1)); sleep 0.05
done
grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null || {
    echo "backend ready timeout" >&2; exit 1;
}

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" cubic \
    >"$OUT/runtime.stdout" 2>"$OUT/runtime.stderr" &
RUNTIME_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p2: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null 2>&1; then
        break
    fi
    kill -0 "$RUNTIME_PID" 2>/dev/null || {
        cat "$OUT/runtime.stderr" >&2 || true; exit 1;
    }
    i=$((i + 1)); sleep 0.05
done
[ "$i" -lt 100 ] || { echo "runtime ready timeout" >&2; exit 1; }
grep -F 'cc=cubic' "$OUT/runtime.stdout" >/dev/null

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$EXPECTED" "$CHUNK_BYTES" \
    >"$OUT/client.stdout" 2>"$OUT/client.stderr" <<'PY' &
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
expected = int(sys.argv[3])
mss = int(sys.argv[4])
received = 0
start = time.monotonic()
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(40.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, mss)
    sock.connect((host, port))
    while True:
        chunk = sock.recv(8192)
        if not chunk:
            break
        received += len(chunk)
    sock.shutdown(socket.SHUT_WR)
elapsed = time.monotonic() - start
if received != expected:
    raise SystemExit(f"received={received} expected={expected}")
print(f"client-bytes={received} elapsed_s={elapsed:.6f}")
PY
CLIENT_PID=$!

i=0
while [ "$i" -lt 120 ] && ! grep -F 'backend-accepted' "$OUT/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$CLIENT_PID" 2>/dev/null || { cat "$OUT/client.stderr" >&2 || true; exit 1; }
    kill -0 "$BACKEND_PID" 2>/dev/null || { cat "$OUT/backend.stderr" >&2 || true; exit 1; }
    i=$((i + 1)); sleep 0.05
done
grep -F 'backend-accepted' "$OUT/backend.stdout" >/dev/null || {
    echo "backend accept timeout" >&2; exit 1;
}

# Every tcp-shift data packet is CE-marked. CWR itself travels on ECT(0) new
# data and may therefore also become CE; receiver processing clears the prior
# ECE episode on CWR and immediately latches the new CE as the next episode.
iptables -t mangle -N "$CHAIN"
iptables -t mangle -I PREROUTING 1 -i "$TUN_NAME" -j "$CHAIN"
iptables -t mangle -A "$CHAIN" \
    -s "$LWIP_IP" -d "$HOST_IP" -p tcp --sport "$PUBLIC_PORT" \
    -m length --length 80:65535 \
    -j TOS --set-tos 0x03/0x03
iptables -t mangle -A "$CHAIN" -j RETURN
RULE_INSTALLED=1
touch "$RELEASE"

wait "$CLIENT_PID" || {
    CLIENT_PID=
    cat "$OUT/client.stderr" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    exit 1
}
CLIENT_PID=
wait "$BACKEND_PID" || {
    BACKEND_PID=
    cat "$OUT/backend.stderr" >&2 || true
    exit 1
}
BACKEND_PID=

iptables -t mangle -nvxL "$CHAIN" > "$OUT/mangle-rule.txt"
marked=$(awk '$3 == "TOS" {print $1; exit}' "$OUT/mangle-rule.txt")
[ -n "$marked" ] && [ "$marked" -ge 15 ] || {
    cat "$OUT/mangle-rule.txt" >&2
    echo "persistent CE rule marked too few packets: ${marked:-none}" >&2
    exit 1
}

sleep 0.35
kill -TERM "$RUNTIME_PID"
wait "$RUNTIME_PID" || {
    RUNTIME_PID=
    cat "$OUT/runtime.stderr" >&2 || true
    exit 1
}
RUNTIME_PID=

events=$(grep -m1 ' cc_bindings=' "$OUT/runtime.stderr")
ecn_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_events=\([0-9][0-9]*\).*/\1/p')
min_cwnd=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_min_cwnd=\([0-9][0-9]*\).*/\1/p')
pre_one_mss=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_pre_one_mss_events=\([0-9][0-9]*\).*/\1/p')
min_pre_cwnd=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_min_pre_cwnd=\([0-9][0-9]*\).*/\1/p')
last_pre_cwnd=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_last_pre_cwnd=\([0-9][0-9]*\).*/\1/p')
last_post_cwnd=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_last_post_cwnd=\([0-9][0-9]*\).*/\1/p')
last_mss=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_last_mss=\([0-9][0-9]*\).*/\1/p')
gate_enters=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_rto_wait_enters=\([0-9][0-9]*\).*/\1/p')
gate_releases=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_rto_wait_releases=\([0-9][0-9]*\).*/\1/p')
loss_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_loss_events=\([0-9][0-9]*\).*/\1/p')
timeout_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_timeout_events=\([0-9][0-9]*\).*/\1/p')

for value in "$ecn_events" "$min_cwnd" "$pre_one_mss" "$min_pre_cwnd" "$last_pre_cwnd" "$last_post_cwnd" "$last_mss" "$gate_enters" "$gate_releases" "$loss_events" "$timeout_events"; do
    [ -n "$value" ] || { echo "missing persistent ECN telemetry" >&2; cat "$OUT/runtime.stderr" >&2; exit 1; }
done
printf 'rfc3168_persistent_diag ce_marks=%s ecn_events=%s min_cwnd=%s pre_one_mss=%s min_pre_cwnd=%s last_pre_cwnd=%s last_post_cwnd=%s last_mss=%s gate_enters=%s gate_releases=%s loss_events=%s timeout_events=%s\n' \
    "$marked" "$ecn_events" "$min_cwnd" "$pre_one_mss" "$min_pre_cwnd" "$last_pre_cwnd" "$last_post_cwnd" "$last_mss" "$gate_enters" "$gate_releases" "$loss_events" "$timeout_events" \
    | tee "$OUT/diagnostic.txt"

[ "$ecn_events" -ge 8 ] || {
    echo "expected >=8 independent ECN congestion responses, got $ecn_events" >&2
    exit 1
}
[ "$min_cwnd" -le "$CHUNK_BYTES" ] || {
    echo "CUBIC did not reach one peer-advertised SMSS: min_cwnd=$min_cwnd mss=$CHUNK_BYTES" >&2
    exit 1
}
[ "$gate_enters" -ge 1 ] || {
    echo "RFC 3168 one-SMSS timer gate never entered" >&2
    exit 1
}
[ "$gate_releases" -ge 1 ] || {
    echo "RFC 3168 one-SMSS timer gate never released" >&2
    exit 1
}
[ "$loss_events" -eq 0 ] || {
    echo "persistent CE incorrectly created packet-loss events: $loss_events" >&2
    exit 1
}
[ "$timeout_events" -eq 0 ] || {
    echo "ECN timer gate incorrectly surfaced as CC timeout: $timeout_events" >&2
    exit 1
}

printf 'rfc3168_persistent=ok bytes=%s ce_marks=%s ecn_events=%s min_cwnd=%s gate_enters=%s gate_releases=%s loss_events=%s timeout_events=%s\n' \
    "$EXPECTED" "$marked" "$ecn_events" "$min_cwnd" "$gate_enters" "$gate_releases" "$loss_events" "$timeout_events" \
    | tee "$OUT/summary.txt"
