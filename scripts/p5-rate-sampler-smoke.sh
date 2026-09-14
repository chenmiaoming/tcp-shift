#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
OUT="$BUILD/p5-rate-sampler-ci"
BINARY=${TCP_SHIFT_P2_BINARY:-"$BUILD/tcp-shift-p2"}
TUN_NAME=${TCP_SHIFT_P5_TUN_NAME:-tsp5rate0}
LWIP_IP=${TCP_SHIFT_P5_LWIP_IP:-10.235.0.2}
NETMASK=${TCP_SHIFT_P5_NETMASK:-255.255.255.252}
HOST_IP=${TCP_SHIFT_P5_HOST_IP:-10.235.0.1}
PUBLIC_PORT=${TCP_SHIFT_P5_PUBLIC_PORT:-18105}
BACKEND_PORT=${TCP_SHIFT_P5_BACKEND_PORT:-19105}
FIRST_BURST=${TCP_SHIFT_P5_FIRST_BURST:-4096}
SECOND_BURST=${TCP_SHIFT_P5_SECOND_BURST:-131072}
PAUSE_SECONDS=${TCP_SHIFT_P5_PAUSE_SECONDS:-0.8}
PID=
BACKEND_PID=
CLIENT_PID=

mkdir -p "$OUT"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"

fail()
{
    echo "P5 rate-sampler qualification failed: $*" >&2
    exit 1
}

stop_pid()
{
    target=$1
    if [ -n "$target" ] && kill -0 "$target" 2>/dev/null; then
        kill -TERM "$target" >/dev/null 2>&1 || true
        wait "$target" >/dev/null 2>&1 || true
    fi
}

capture_state()
{
    ip -d addr show > "$OUT/ip-addr.txt" 2>&1 || true
    ip route show table all > "$OUT/ip-route.txt" 2>&1 || true
}

cleanup()
{
    set +e
    stop_pid "${CLIENT_PID:-}"
    stop_pid "${PID:-}"
    stop_pid "${BACKEND_PID:-}"
    capture_state
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        sudo ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

[ -x "$BINARY" ] || fail "missing P2 binary: $BINARY"

total=$((FIRST_BURST + SECOND_BURST))

python3 - "$BACKEND_PORT" "$FIRST_BURST" "$SECOND_BURST" "$PAUSE_SECONDS" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys
import time

port = int(sys.argv[1])
first_len = int(sys.argv[2])
second_len = int(sys.argv[3])
pause = float(sys.argv[4])
first = bytes(((i * 17 + 11) & 0xff) for i in range(first_len))
second = bytes(((i * 29 + 53) & 0xff) for i in range(second_len))
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready 127.0.0.1:{port}", flush=True)
conn, peer = server.accept()
conn.settimeout(8.0)
conn.sendall(first)
print(f"backend-first-burst={len(first)} pause_seconds={pause}", flush=True)
time.sleep(pause)
conn.sendall(second)
print(f"backend-second-burst={len(second)}", flush=True)
conn.shutdown(socket.SHUT_WR)
while True:
    data = conn.recv(4096)
    if not data:
        break
conn.close()
server.close()
payload = first + second
print(
    f"backend-total={len(payload)} sha256={hashlib.sha256(payload).hexdigest()} complete=ok",
    flush=True,
)
PY
BACKEND_PID=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
        cat "$OUT/backend.stderr" >&2 || true
        fail "backend exited before ready"
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || fail "timed out waiting for backend"

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
PID=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p2: ready tun=$TUN_NAME" \
           "$OUT/runtime.stdout" >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        fail "runtime exited before ready"
    fi
    i=$((i + 1))
    sleep 0.05
done
[ "$ready" -eq 1 ] || fail "timed out waiting for runtime"

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$FIRST_BURST" "$SECOND_BURST" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY' &
import hashlib
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
first_len = int(sys.argv[3])
second_len = int(sys.argv[4])
expected = (
    bytes(((i * 17 + 11) & 0xff) for i in range(first_len)) +
    bytes(((i * 29 + 53) & 0xff) for i in range(second_len))
)
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(8.0)
    sock.connect((host, port))
    sock.shutdown(socket.SHUT_WR)
    chunks = []
    while True:
        chunk = sock.recv(16384)
        if not chunk:
            break
        chunks.append(chunk)
actual = b"".join(chunks)
if actual != expected:
    raise SystemExit(
        f"payload mismatch expected={len(expected)} actual={len(actual)} "
        f"expected_sha={hashlib.sha256(expected).hexdigest()} "
        f"actual_sha={hashlib.sha256(actual).hexdigest()}"
    )
print(
    f"client-total={len(actual)} sha256={hashlib.sha256(actual).hexdigest()} complete=ok",
    flush=True,
)
PY
CLIENT_PID=$!

burst=0
i=0
while [ "$i" -lt 120 ]; do
    if grep -F "backend-first-burst=$FIRST_BURST " \
        "$OUT/backend.stdout" >/dev/null 2>&1; then
        burst=1
        break
    fi
    if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
        cat "$OUT/backend.stdout" >&2 || true
        cat "$OUT/backend.stderr" >&2 || true
        fail "backend exited before first burst marker"
    fi
    i=$((i + 1))
    sleep 0.025
done
[ "$burst" -eq 1 ] || fail "timed out waiting for first burst"

# Give the first 4 KiB flight time to be transmitted/ACKed, then measure a
# window fully inside the backend application's intentional pause. The product
# loop may wake for real lwIP deadlines, but it must not burn CPU in a fixed
# app-limited polling loop.
sleep 0.15
[ -r "/proc/$PID/stat" ] || fail "runtime disappeared during app pause"
cpu_before=$(awk '{print $14 + $15}' "/proc/$PID/stat")
sleep 0.30
[ -r "/proc/$PID/stat" ] || fail "runtime disappeared during app pause"
cpu_after=$(awk '{print $14 + $15}' "/proc/$PID/stat")
cpu_delta=$((cpu_after - cpu_before))
[ "$cpu_delta" -ge 0 ] || fail "negative CPU tick delta"
[ "$cpu_delta" -le 1 ] || fail "runtime consumed $cpu_delta CPU ticks during app pause"
printf 'cpu_ticks_before=%s cpu_ticks_after=%s cpu_ticks_delta=%s\n' \
    "$cpu_before" "$cpu_after" "$cpu_delta" > "$OUT/pause-cpu.txt"

if ! wait "$CLIENT_PID"; then
    CLIENT_PID=
    cat "$OUT/client.stdout" >&2 || true
    cat "$OUT/client.stderr" >&2 || true
    fail "client failed"
fi
CLIENT_PID=

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stdout" >&2 || true
    cat "$OUT/backend.stderr" >&2 || true
    fail "backend failed"
fi
BACKEND_PID=

sleep 0.2
kill -TERM "$PID"
if ! wait "$PID"; then
    PID=
    cat "$OUT/runtime.stdout" >&2 || true
    cat "$OUT/runtime.stderr" >&2 || true
    fail "runtime failed"
fi
PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/pause-cpu.txt"
cat "$OUT/runtime.stdout"
cat "$OUT/runtime.stderr" >&2

rate_value()
{
    key=$1
    awk -v key="$key" '
        $1 == "tcp-shift-p2-rate:" {
            for (i = 2; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == key) value = kv[2]
            }
        }
        END { if (value == "") exit 1; print value }
    ' "$OUT/runtime.stderr"
}

delivery_value()
{
    key=$1
    awk -v key="$key" '
        $1 == "tcp-shift-p2-delivery:" {
            for (i = 2; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == key) value = kv[2]
            }
        }
        END { if (value == "") exit 1; print value }
    ' "$OUT/runtime.stderr"
}

samples=$(rate_value samples) || fail "missing rate samples"
valid=$(rate_value valid_samples) || fail "missing valid rate samples"
invalid=$(rate_value invalid_samples) || fail "missing invalid rate samples"
app_samples=$(rate_value app_limited_samples) || fail "missing app-limited samples"
enters=$(rate_value app_limited_enters) || fail "missing app-limited enters"
exits=$(rate_value app_limited_exits) || fail "missing app-limited exits"
max_rate=$(rate_value max_rate_bytes_per_sec) || fail "missing max delivery rate"
delivered=$(delivery_value delivered_payload_bytes) || fail "missing delivered bytes"
live=$(delivery_value live_slots) || fail "missing live metadata slots"
misses=$(delivery_value metadata_misses) || fail "missing metadata misses"
alloc_failures=$(delivery_value metadata_alloc_failures) || fail "missing allocation failures"
clock_errors=$(delivery_value clock_errors) || fail "missing clock errors"
regressions=$(delivery_value timestamp_regressions) || fail "missing timestamp regressions"

[ "$samples" -gt 0 ] || fail "no rate samples"
[ "$valid" -gt 0 ] || fail "no valid rate samples"
[ "$invalid" -eq 0 ] || fail "invalid rate samples=$invalid"
[ "$app_samples" -gt 0 ] || fail "no app-limited rate sample"
[ "$enters" -gt 0 ] || fail "app-limited state never entered"
[ "$exits" -gt 0 ] || fail "app-limited state never exited"
[ "$max_rate" -gt 0 ] || fail "maximum delivery rate is zero"
[ "$delivered" -eq "$total" ] || fail "delivered bytes=$delivered expected=$total"
[ "$live" -eq 0 ] || fail "live metadata slots after teardown=$live"
[ "$misses" -eq 0 ] || fail "metadata misses=$misses"
[ "$alloc_failures" -eq 0 ] || fail "metadata allocation failures=$alloc_failures"
[ "$clock_errors" -eq 0 ] || fail "clock errors=$clock_errors"
[ "$regressions" -eq 0 ] || fail "timestamp regressions=$regressions"

grep -F "client-total=$total " "$OUT/client.stdout" >/dev/null ||
    fail "client byte total mismatch"
grep -F ' complete=ok' "$OUT/client.stdout" >/dev/null ||
    fail "client integrity marker missing"
grep -F "backend-total=$total " "$OUT/backend.stdout" >/dev/null ||
    fail "backend byte total mismatch"
grep -F 'bridge_backend_failures=0 bridge_public_errors=0' \
    "$OUT/runtime.stderr" >/dev/null || fail "bridge reported failures"

if ip link show "$TUN_NAME" >/dev/null 2>&1; then
    fail "TUN leaked after process exit"
fi

capture_state
printf 'first_burst=%s second_burst=%s delivered=%s rate_samples=%s valid_samples=%s app_limited_samples=%s app_limited_enters=%s app_limited_exits=%s max_rate_bytes_per_sec=%s pause_cpu_ticks=%s event_driven=ok\n' \
    "$FIRST_BURST" "$SECOND_BURST" "$delivered" "$samples" "$valid" \
    "$app_samples" "$enters" "$exits" "$max_rate" "$cpu_delta" \
    | tee "$OUT/summary.txt"
echo "P5 event-driven app-limited rate-sampler smoke passed"
