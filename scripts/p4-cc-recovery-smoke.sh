#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
MODE=${1:-}
BINARY=${TCP_SHIFT_P4_BINARY:-"$BUILD/tcp-shift-p2"}
PAYLOAD_BYTES=${TCP_SHIFT_P4_PAYLOAD_BYTES:-262144}
CC=${TCP_SHIFT_P4_CC:-reno}
REQUIRE_PACING=${TCP_SHIFT_P4_REQUIRE_PACING:-0}
REQUIRE_RETRANSMIT=${TCP_SHIFT_P4_REQUIRE_RETRANSMIT:-0}

case "$REQUIRE_PACING:$REQUIRE_RETRANSMIT" in
    0:0|0:1|1:0|1:1) ;;
    *) echo "TCP_SHIFT_P4_REQUIRE_PACING/RETRANSMIT must be 0 or 1" >&2; exit 2 ;;
esac

case "$MODE" in
    fast-loss)
        DEFAULT_OUT="$BUILD/p4-fast-loss-ci"
        TUN_NAME=${TCP_SHIFT_P4_TUN_NAME:-tsp4loss0}
        PUBLIC_PORT=${TCP_SHIFT_P4_PUBLIC_PORT:-18140}
        BACKEND_PORT=${TCP_SHIFT_P4_BACKEND_PORT:-19140}
        ;;
    multi-loss)
        DEFAULT_OUT="$BUILD/p4-multi-loss-ci"
        TUN_NAME=${TCP_SHIFT_P4_TUN_NAME:-tsp4multi0}
        PUBLIC_PORT=${TCP_SHIFT_P4_PUBLIC_PORT:-18143}
        BACKEND_PORT=${TCP_SHIFT_P4_BACKEND_PORT:-19143}
        ;;
    rto)
        DEFAULT_OUT="$BUILD/p4-rto-ci"
        TUN_NAME=${TCP_SHIFT_P4_TUN_NAME:-tsp4rto0}
        PUBLIC_PORT=${TCP_SHIFT_P4_PUBLIC_PORT:-18141}
        BACKEND_PORT=${TCP_SHIFT_P4_BACKEND_PORT:-19141}
        ;;
    *)
        echo "usage: $0 <fast-loss|multi-loss|rto>" >&2
        exit 2
        ;;
esac

OUT=${TCP_SHIFT_P4_OUT:-$DEFAULT_OUT}
LWIP_IP=${TCP_SHIFT_P4_LWIP_IP:-10.244.0.2}
HOST_IP=${TCP_SHIFT_P4_HOST_IP:-10.244.0.1}
NETMASK=${TCP_SHIFT_P4_NETMASK:-255.255.255.252}
CHAIN="TCP_SHIFT_P4_$$_$(printf '%s' "$MODE" | tr '-' '_' | tr '[:lower:]' '[:upper:]')"
PID=
BACKEND_PID=
CLIENT_PID=
JUMP_INSTALLED=0
CHAIN_CREATED=0

mkdir -p "$OUT"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"

[ "$(id -u)" -eq 0 ] || {
    echo "P4 recovery harness must run as root for TUN and isolated INPUT fault injection" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing P4 bridge binary: $BINARY" >&2
    exit 1
}
command -v iptables >/dev/null 2>&1 || {
    echo "iptables is required for P4 recovery fault injection" >&2
    exit 1
}

remove_filter()
{
    set +e
    if [ "$JUMP_INSTALLED" -eq 1 ]; then
        iptables -D INPUT -i "$TUN_NAME" -j "$CHAIN" >/dev/null 2>&1 || true
        JUMP_INSTALLED=0
    fi
    if [ "$CHAIN_CREATED" -eq 1 ]; then
        iptables -F "$CHAIN" >/dev/null 2>&1 || true
        iptables -X "$CHAIN" >/dev/null 2>&1 || true
        CHAIN_CREATED=0
    fi
    set -e
}

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
    remove_filter
    stop_pid "${CLIENT_PID:-}"
    stop_pid "${PID:-}"
    stop_pid "${BACKEND_PID:-}"
    ip -d addr show > "$OUT/ip-addr.txt" 2>&1 || true
    ip route show table all > "$OUT/ip-route.txt" 2>&1 || true
    iptables-save > "$OUT/iptables-after.txt" 2>&1 || true
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

python3 - "$BACKEND_PORT" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
conn, _ = server.accept()
conn.settimeout(20.0)
chunks = []
while True:
    chunk = conn.recv(16384)
    if not chunk:
        break
    chunks.append(chunk)
payload = b"".join(chunks)
conn.sendall(payload)
conn.shutdown(socket.SHUT_WR)
conn.close()
server.close()
print(
    f"backend-bytes={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} echo=ok",
    flush=True,
)
PY
BACKEND_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$BACKEND_PID" 2>/dev/null || {
        cat "$OUT/backend.stderr" >&2 || true
        echo "P4 backend exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null || {
    echo "timed out waiting for P4 backend" >&2
    exit 1
}

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" "$CC" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p2: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null 2>&1 &&
       grep -F "cc=$CC" "$OUT/runtime.stdout" >/dev/null 2>&1; then
        break
    fi
    kill -0 "$PID" 2>/dev/null || {
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "P4 runtime exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
[ "$i" -lt 100 ] || {
    echo "timed out waiting for P4 runtime controller=$CC" >&2
    exit 1
}

iptables -N "$CHAIN"
CHAIN_CREATED=1
iptables -I INPUT 1 -i "$TUN_NAME" -j "$CHAIN"
JUMP_INSTALLED=1

case "$MODE" in
    fast-loss)
        iptables -A "$CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
            -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 \
            -m statistic --mode nth --every 10000 --packet 10 -j DROP
        ;;
    multi-loss)
        # Drop two data packets in the same early flight. Separate nth matchers
        # each fire once in this transfer; the first DROP short-circuits the
        # chain for that packet, so the second matcher lands a few packets later.
        iptables -A "$CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
            -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 \
            -m statistic --mode nth --every 10000 --packet 10 -j DROP
        iptables -A "$CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
            -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 \
            -m statistic --mode nth --every 10000 --packet 12 -j DROP
        ;;
    rto)
        iptables -A "$CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
            -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 -j DROP
        ;;
esac
iptables -A "$CHAIN" -j RETURN

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY' &
import hashlib
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
length = int(sys.argv[3])
payload = bytes(((index * 73 + 19) & 0xFF) for index in range(length))
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(20.0)
    sock.connect((host, port))
    sock.sendall(payload)
    sock.shutdown(socket.SHUT_WR)
    chunks = []
    while True:
        chunk = sock.recv(16384)
        if not chunk:
            break
        chunks.append(chunk)
echo = b"".join(chunks)
if echo != payload:
    raise SystemExit(
        f"echo mismatch sent={len(payload)} received={len(echo)} "
        f"sent_sha={hashlib.sha256(payload).hexdigest()} "
        f"received_sha={hashlib.sha256(echo).hexdigest()}"
    )
print(
    f"p4-client-mode={sys.argv[0] if False else 'ok'} bytes={len(payload)} "
    f"sha256={hashlib.sha256(payload).hexdigest()} echo=ok"
)
PY
CLIENT_PID=$!

if [ "$MODE" = rto ]; then
    sleep 4
    iptables -nvxL "$CHAIN" > "$OUT/iptables-fault.txt"
    remove_filter
fi

if ! wait "$CLIENT_PID"; then
    CLIENT_PID=
    cat "$OUT/client.stderr" >&2 || true
    iptables -nvxL "$CHAIN" > "$OUT/iptables-fault.txt" 2>&1 || true
    echo "P4 $MODE client failed" >&2
    exit 1
fi
CLIENT_PID=

if [ "$MODE" = fast-loss ] || [ "$MODE" = multi-loss ]; then
    iptables -nvxL "$CHAIN" > "$OUT/iptables-fault.txt"
fi

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stderr" >&2 || true
    echo "P4 $MODE backend failed" >&2
    exit 1
fi
BACKEND_PID=

sleep 0.2
kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P4 $MODE runtime failed" >&2
    exit 1
fi
PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stderr" >&2

fault_drops=$(awk '$1 ~ /^[0-9]+$/ && $3 == "DROP" {sum += $1} END {print sum + 0}' \
    "$OUT/iptables-fault.txt")
[ "$fault_drops" -ge 1 ] || {
    cat "$OUT/iptables-fault.txt" >&2 || true
    echo "P4 $MODE fault rule dropped no packet" >&2
    exit 1
}
if [ "$MODE" = multi-loss ] && [ "$fault_drops" -ne 2 ]; then
    cat "$OUT/iptables-fault.txt" >&2 || true
    echo "P4 multi-loss expected exactly two dropped data packets: drops=$fault_drops" >&2
    exit 1
fi

grep -F "backend-bytes=$PAYLOAD_BYTES " "$OUT/backend.stdout" >/dev/null
grep -F ' echo=ok' "$OUT/backend.stdout" >/dev/null
grep -F "bytes=$PAYLOAD_BYTES " "$OUT/client.stdout" >/dev/null
grep -F ' echo=ok' "$OUT/client.stdout" >/dev/null
grep -F "cc=$CC" "$OUT/runtime.stdout" >/dev/null
grep -F 'cc_bindings=1' "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_bind_failures=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_controller_errors=0' "$OUT/runtime.stderr" >/dev/null

events=$(grep -m1 ' cc_bindings=' "$OUT/runtime.stderr")
loss_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_loss_events=\([0-9][0-9]*\).*/\1/p')
timeout_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_timeout_events=\([0-9][0-9]*\).*/\1/p')
[ -n "$loss_events" ] && [ -n "$timeout_events" ] || {
    echo "missing P4 recovery event counters" >&2
    exit 1
}

if [ "$REQUIRE_RETRANSMIT" -eq 1 ]; then
    delivery=$(grep -m1 'tcp-shift-p2-delivery:' "$OUT/runtime.stderr")
    retransmit_events=$(printf '%s\n' "$delivery" |
        sed -n 's/.* retransmit_events=\([0-9][0-9]*\).*/\1/p')
    [ -n "$retransmit_events" ] && [ "$retransmit_events" -ge 1 ] || {
        echo "required retransmission telemetry missing: retransmit_events=${retransmit_events:-missing}" >&2
        exit 1
    }
fi

if [ "$REQUIRE_PACING" -eq 1 ]; then
    pacing=$(grep -m1 'tcp-shift-p2-pacing:' "$OUT/runtime.stderr")
    pacing_deferrals=$(printf '%s\n' "$pacing" |
        sed -n 's/.* deferrals=\([0-9][0-9]*\).*/\1/p')
    pacing_resumes=$(printf '%s\n' "$pacing" |
        sed -n 's/.* resume_events=\([0-9][0-9]*\).*/\1/p')
    pacing_errors=$(printf '%s\n' "$pacing" |
        sed -n 's/.* scheduler_errors=\([0-9][0-9]*\).*/\1/p')
    pacing_tx=$(printf '%s\n' "$pacing" |
        sed -n 's/.* tx_events=\([0-9][0-9]*\).*/\1/p')
    pacing_rate=$(printf '%s\n' "$pacing" |
        sed -n 's/.* last_rate_bytes_per_sec=\([0-9][0-9]*\).*/\1/p')
    [ -n "$pacing_deferrals" ] && [ "$pacing_deferrals" -ge 1 ] &&
    [ -n "$pacing_resumes" ] && [ "$pacing_resumes" -ge 1 ] &&
    [ -n "$pacing_tx" ] && [ "$pacing_tx" -ge 1 ] &&
    [ -n "$pacing_rate" ] && [ "$pacing_rate" -ge 1 ] &&
    [ -n "$pacing_errors" ] && [ "$pacing_errors" -eq 0 ] || {
        echo "required pacing telemetry invalid: deferrals=${pacing_deferrals:-missing} resumes=${pacing_resumes:-missing} tx=${pacing_tx:-missing} rate=${pacing_rate:-missing} errors=${pacing_errors:-missing}" >&2
        exit 1
    }
fi

case "$MODE" in
    fast-loss)
        [ "$loss_events" -ge 1 ] || {
            echo "fast-loss path produced no controller loss event" >&2
            exit 1
        }
        [ "$timeout_events" -eq 0 ] || {
            echo "fast-loss path fell through to RTO: timeout_events=$timeout_events" >&2
            exit 1
        }
        ;;
    multi-loss)
        [ "$loss_events" -ge 1 ] || {
            echo "multi-loss path produced no controller loss event" >&2
            exit 1
        }
        [ "$timeout_events" -eq 0 ] || {
            echo "multi-loss path fell through to RTO: timeout_events=$timeout_events" >&2
            exit 1
        }
        ;;
    rto)
        [ "$timeout_events" -ge 1 ] || {
            echo "RTO path produced no controller timeout event" >&2
            exit 1
        }
        ;;
esac

printf 'mode=%s cc=%s payload_bytes=%u fault_drops=%s loss_events=%s timeout_events=%s pacing_required=%s retransmit_required=%s recovery=ok\n' \
    "$MODE" "$CC" "$PAYLOAD_BYTES" "$fault_drops" "$loss_events" "$timeout_events" \
    "$REQUIRE_PACING" "$REQUIRE_RETRANSMIT" \
    | tee "$OUT/summary.txt"
echo "P4 integrated $MODE controller=$CC recovery qualification passed"
