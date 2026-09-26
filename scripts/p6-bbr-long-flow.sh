#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
BINARY=${TCP_SHIFT_P6_BBR_LONG_BINARY:-"$BUILD/tcp-shift-p6-bbr"}
CC=${TCP_SHIFT_P6_BBR_LONG_CC:-bbr-internal}
RTT_MS=${TCP_SHIFT_P6_BBR_LONG_RTT_MS:-40}
RATE_MBIT=${TCP_SHIFT_P6_BBR_LONG_RATE_MBIT:-10}
PAYLOAD_BYTES=${TCP_SHIFT_P6_BBR_LONG_PAYLOAD_BYTES:-4194304}
LOSS_PCT=${TCP_SHIFT_P6_BBR_LONG_LOSS_PCT:-0}
FAULT_MODE=${TCP_SHIFT_P6_BBR_LONG_FAULT_MODE:-none}
FAULT_FIRST_PACKET=${TCP_SHIFT_P6_BBR_LONG_FAULT_FIRST_PACKET:-80}
FAULT_SECOND_PACKET=${TCP_SHIFT_P6_BBR_LONG_FAULT_SECOND_PACKET:-84}
FAULT_BURST_PACKETS=${TCP_SHIFT_P6_BBR_LONG_FAULT_BURST_PACKETS:-3}
FAULT_BURST_REPEATS=${TCP_SHIFT_P6_BBR_LONG_FAULT_BURST_REPEATS:-1}
FAULT_BURST_GAP_PACKETS=${TCP_SHIFT_P6_BBR_LONG_FAULT_BURST_GAP_PACKETS:-300}
FAULT_RETRANS_MARKER=${TCP_SHIFT_P6_BBR_LONG_FAULT_RETRANS_MARKER:-TCP_SHIFT_SACK_RETRANS_LOSS}
FAULT_MARKER_COUNT=${TCP_SHIFT_P6_BBR_LONG_FAULT_MARKER_COUNT:-28}
FAULT_MARKER_GAP_PACKETS=${TCP_SHIFT_P6_BBR_LONG_FAULT_MARKER_GAP_PACKETS:-96}
FAULT_MARKER_PREFIX=${TCP_SHIFT_P6_BBR_LONG_FAULT_MARKER_PREFIX:-TSFS}
RECOVERY_EXPECTATION=${TCP_SHIFT_P6_BBR_LONG_RECOVERY_EXPECTATION:-strict}
OUT=${TCP_SHIFT_P6_BBR_LONG_OUT:-"$BUILD/p6-bbr-long-flow"}

TUN_NAME=${TCP_SHIFT_P6_BBR_LONG_TUN_NAME:-"tsp6lf$$"}
IFB_NAME=${TCP_SHIFT_P6_BBR_LONG_IFB_NAME:-"p6ifb$$"}
LWIP_IP=${TCP_SHIFT_P6_BBR_LONG_LWIP_IP:-10.246.0.2}
HOST_IP=${TCP_SHIFT_P6_BBR_LONG_HOST_IP:-10.246.0.1}
NETMASK=${TCP_SHIFT_P6_BBR_LONG_NETMASK:-255.255.255.252}
PUBLIC_PORT=${TCP_SHIFT_P6_BBR_LONG_PUBLIC_PORT:-18162}
BACKEND_PORT=${TCP_SHIFT_P6_BBR_LONG_BACKEND_PORT:-19162}

RUNTIME_PID=
BACKEND_PID=
FAULT_CHAIN="P6_$TUN_NAME"
FAULT_CHAIN_CREATED=0
FAULT_JUMP_INSTALLED=0

mkdir -p "$OUT"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"

[ "$(id -u)" -eq 0 ] || {
    echo "P6 BBR long-flow harness must run as root for TUN, IFB and netem" >&2
    exit 1
}
[ -x "$BINARY" ] || {
    echo "missing internal BBR qualification binary: $BINARY" >&2
    exit 1
}
command -v ip >/dev/null 2>&1 || { echo "ip is required" >&2; exit 1; }
command -v tc >/dev/null 2>&1 || { echo "tc is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 1; }

case "$FAULT_MODE" in
    none|multi-loss|burst-loss|repeated-burst|lost-retransmission|first-send-loss) ;;
    *) echo "FAULT_MODE must be none, multi-loss, burst-loss, repeated-burst, lost-retransmission or first-send-loss" >&2; exit 1;;
esac
case "$RECOVERY_EXPECTATION" in
    strict|diagnostic) ;;
    *) echo "RECOVERY_EXPECTATION must be strict or diagnostic" >&2; exit 1;;
esac
if [ "$FAULT_MODE" != none ]; then
    command -v iptables >/dev/null 2>&1 || { echo "iptables is required for deterministic loss" >&2; exit 1; }
    [ "$LOSS_PCT" = 0 ] || [ "$LOSS_PCT" = 0.0 ] || {
        echo "deterministic FAULT_MODE cannot be combined with random LOSS_PCT" >&2
        exit 1
    }
fi

case "$RTT_MS" in ''|*[!0-9]*) echo "RTT_MS must be an integer" >&2; exit 1;; esac
case "$RATE_MBIT" in ''|*[!0-9]*) echo "RATE_MBIT must be an integer" >&2; exit 1;; esac
case "$PAYLOAD_BYTES" in ''|*[!0-9]*) echo "PAYLOAD_BYTES must be an integer" >&2; exit 1;; esac
case "$LOSS_PCT" in ''|*[!0-9.]*|*.*.*) echo "LOSS_PCT must be a nonnegative decimal" >&2; exit 1;; esac
case "$FAULT_FIRST_PACKET" in ''|*[!0-9]*) echo "FAULT_FIRST_PACKET must be an integer" >&2; exit 1;; esac
case "$FAULT_SECOND_PACKET" in ''|*[!0-9]*) echo "FAULT_SECOND_PACKET must be an integer" >&2; exit 1;; esac
case "$FAULT_BURST_PACKETS" in ''|*[!0-9]*) echo "FAULT_BURST_PACKETS must be an integer" >&2; exit 1;; esac
case "$FAULT_BURST_REPEATS" in ''|*[!0-9]*) echo "FAULT_BURST_REPEATS must be an integer" >&2; exit 1;; esac
case "$FAULT_BURST_GAP_PACKETS" in ''|*[!0-9]*) echo "FAULT_BURST_GAP_PACKETS must be an integer" >&2; exit 1;; esac
case "$FAULT_MARKER_COUNT" in ''|*[!0-9]*) echo "FAULT_MARKER_COUNT must be an integer" >&2; exit 1;; esac
case "$FAULT_MARKER_GAP_PACKETS" in ''|*[!0-9]*) echo "FAULT_MARKER_GAP_PACKETS must be an integer" >&2; exit 1;; esac
if [ "$FAULT_MODE" = multi-loss ]; then
    [ "$FAULT_SECOND_PACKET" -gt "$FAULT_FIRST_PACKET" ] || {
        echo "FAULT_SECOND_PACKET must be greater than FAULT_FIRST_PACKET" >&2
        exit 1
    }
fi
[ "$FAULT_BURST_PACKETS" -ge 2 ] && [ "$FAULT_BURST_PACKETS" -le 16 ] || {
    echo "FAULT_BURST_PACKETS must be between 2 and 16" >&2
    exit 1
}
[ "$FAULT_BURST_REPEATS" -ge 1 ] && [ "$FAULT_BURST_REPEATS" -le 16 ] || {
    echo "FAULT_BURST_REPEATS must be between 1 and 16" >&2
    exit 1
}
[ "$FAULT_BURST_GAP_PACKETS" -ge "$FAULT_BURST_PACKETS" ] || {
    echo "FAULT_BURST_GAP_PACKETS must be at least FAULT_BURST_PACKETS" >&2
    exit 1
}
[ "$FAULT_MARKER_COUNT" -ge 1 ] && [ "$FAULT_MARKER_COUNT" -le 64 ] || {
    echo "FAULT_MARKER_COUNT must be between 1 and 64" >&2
    exit 1
}
[ "$FAULT_MARKER_GAP_PACKETS" -ge 4 ] || {
    echo "FAULT_MARKER_GAP_PACKETS must be at least 4" >&2
    exit 1
}
[ -n "$FAULT_MARKER_PREFIX" ] || {
    echo "FAULT_MARKER_PREFIX must not be empty" >&2
    exit 1
}
[ "$RTT_MS" -gt 0 ] && [ $((RTT_MS % 2)) -eq 0 ] || {
    echo "RTT_MS must be a positive even integer" >&2
    exit 1
}
[ "$RATE_MBIT" -gt 0 ] || { echo "RATE_MBIT must be positive" >&2; exit 1; }
[ "$PAYLOAD_BYTES" -gt 0 ] || { echo "PAYLOAD_BYTES must be positive" >&2; exit 1; }

HALF_RTT_MS=$((RTT_MS / 2))
BDP_BYTES=$((RATE_MBIT * RTT_MS * 125))
BDP_PKTS=$(((BDP_BYTES + 1459) / 1460))
# This is the clean long-flow gate, not the loss gate. netem owns the entire
# delayed/shaped path queue on IFB, so a one-BDP limit can turn BBR STARTUP's
# deliberate high inflight gain into artificial drop-tail loss. Keep eight
# BDPs of queue headroom here and require zero qdisc drops below; fast-loss and
# RTO remain qualified separately with explicit fault injection.
QUEUE_PKTS=${TCP_SHIFT_P6_BBR_LONG_QUEUE_PKTS:-$((BDP_PKTS * 8))}
[ "$QUEUE_PKTS" -ge 64 ] || QUEUE_PKTS=64

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
    if [ "$FAULT_JUMP_INSTALLED" -eq 1 ]; then
        iptables -D INPUT -i "$TUN_NAME" -j "$FAULT_CHAIN" >/dev/null 2>&1 || true
        FAULT_JUMP_INSTALLED=0
    fi
    if [ "$FAULT_CHAIN_CREATED" -eq 1 ]; then
        iptables -F "$FAULT_CHAIN" >/dev/null 2>&1 || true
        iptables -X "$FAULT_CHAIN" >/dev/null 2>&1 || true
        FAULT_CHAIN_CREATED=0
    fi
    tc qdisc del dev "$TUN_NAME" root >/dev/null 2>&1 || true
    tc qdisc del dev "$TUN_NAME" ingress >/dev/null 2>&1 || true
    tc qdisc del dev "$IFB_NAME" root >/dev/null 2>&1 || true
    ip link del "$IFB_NAME" >/dev/null 2>&1 || true
    if ip link show "$TUN_NAME" >/dev/null 2>&1; then
        ip link del "$TUN_NAME" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

python3 - "$BACKEND_PORT" "$PAYLOAD_BYTES" "$FAULT_MODE" "$FAULT_FIRST_PACKET" "$FAULT_RETRANS_MARKER" \
    "$FAULT_MARKER_COUNT" "$FAULT_MARKER_GAP_PACKETS" "$FAULT_MARKER_PREFIX" \
    > "$OUT/backend.stdout" 2> "$OUT/backend.stderr" <<'PY' &
import hashlib
import socket
import sys

port = int(sys.argv[1])
length = int(sys.argv[2])
fault_mode = sys.argv[3]
fault_first_packet = int(sys.argv[4])
fault_marker = sys.argv[5].encode("ascii")
fault_marker_count = int(sys.argv[6])
fault_marker_gap_packets = int(sys.argv[7])
fault_marker_prefix = sys.argv[8]
payload = bytearray(((index * 73 + 19) & 0xFF) for index in range(length))

if fault_mode == "first-send-loss":
    for marker_index in range(fault_marker_count):
        packet_index = fault_first_packet + marker_index * fault_marker_gap_packets
        marker = f"{fault_marker_prefix}{marker_index:04d}".encode("ascii")
        marker_region = marker * 4
        marker_offset = packet_index * 1460 + 256
        if marker_offset + len(marker_region) >= length:
            raise SystemExit(
                f"first-send marker exceeds payload: index={marker_index} "
                f"packet={packet_index} offset={marker_offset} "
                f"marker={len(marker_region)} length={length}"
            )
        payload[marker_offset : marker_offset + len(marker_region)] = marker_region
    print(
        f"backend-first-send-markers count={fault_marker_count} "
        f"first_packet={fault_first_packet} gap_packets={fault_marker_gap_packets} "
        f"prefix={fault_marker_prefix}",
        flush=True,
    )

if fault_mode == "lost-retransmission":
    # Embed a marker well inside the nominal target segment. Repeating it keeps
    # at least one complete copy available even if the actual TCP segmentation
    # boundary crosses this region. The iptables rules can then recognize the
    # same stream bytes on the original transmission and its retransmission.
    marker_offset = fault_first_packet * 1460 + 256
    marker_region = fault_marker * 4
    if marker_offset + len(marker_region) >= length:
        raise SystemExit(
            f"lost-retransmission marker exceeds payload: "
            f"offset={marker_offset} marker={len(marker_region)} length={length}"
        )
    payload[marker_offset : marker_offset + len(marker_region)] = marker_region
    print(
        f"backend-fault-marker offset={marker_offset} "
        f"bytes={len(marker_region)} marker={sys.argv[5]}",
        flush=True,
    )

payload = bytes(payload)
digest = hashlib.sha256(payload).hexdigest()

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready port={port}", flush=True)
conn, _ = server.accept()
conn.settimeout(90.0)
conn.sendall(payload)
conn.shutdown(socket.SHUT_WR)
while conn.recv(4096):
    pass
conn.close()
server.close()
print(f"backend-bytes={length} sha256={digest}", flush=True)
PY
BACKEND_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$BACKEND_PID" 2>/dev/null || {
        cat "$OUT/backend.stderr" >&2 || true
        echo "P6 BBR long-flow backend exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null || {
    echo "timed out waiting for P6 BBR long-flow backend" >&2
    exit 1
}

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP" \
    "$PUBLIC_PORT" "$BACKEND_PORT" "$CC" \
    > "$OUT/runtime.stdout" 2> "$OUT/runtime.stderr" &
RUNTIME_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p2: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null 2>&1 &&
       grep -F "cc=$CC" "$OUT/runtime.stdout" >/dev/null 2>&1; then
        break
    fi
    kill -0 "$RUNTIME_PID" 2>/dev/null || {
        cat "$OUT/runtime.stdout" >&2 || true
        cat "$OUT/runtime.stderr" >&2 || true
        echo "P6 BBR long-flow runtime exited before ready" >&2
        exit 1
    }
    i=$((i + 1))
    sleep 0.05
done
[ "$i" -lt 100 ] || {
    echo "timed out waiting for P6 BBR long-flow runtime" >&2
    exit 1
}

modprobe ifb >/dev/null 2>&1 || true
ip link add "$IFB_NAME" type ifb
ip link set "$IFB_NAME" up
tc qdisc add dev "$TUN_NAME" handle ffff: ingress
tc filter add dev "$TUN_NAME" parent ffff: protocol ip prio 1 u32 \
    match u32 0 0 action mirred egress redirect dev "$IFB_NAME"
if [ "$FAULT_MODE" = multi-loss ]; then
    LOSS_MODE=deterministic-multi
    tc qdisc replace dev "$IFB_NAME" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
elif [ "$FAULT_MODE" = burst-loss ]; then
    LOSS_MODE=deterministic-burst
    tc qdisc replace dev "$IFB_NAME" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
elif [ "$FAULT_MODE" = repeated-burst ]; then
    LOSS_MODE=deterministic-repeated-burst
    tc qdisc replace dev "$IFB_NAME" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
elif [ "$FAULT_MODE" = lost-retransmission ]; then
    LOSS_MODE=deterministic-lost-retransmission
    tc qdisc replace dev "$IFB_NAME" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
elif [ "$FAULT_MODE" = first-send-loss ]; then
    LOSS_MODE=deterministic-first-send
    tc qdisc replace dev "$IFB_NAME" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
elif [ "$LOSS_PCT" = 0 ] || [ "$LOSS_PCT" = 0.0 ]; then
    LOSS_MODE=none
    tc qdisc replace dev "$IFB_NAME" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" limit "$QUEUE_PKTS"
else
    LOSS_MODE=random
    tc qdisc replace dev "$IFB_NAME" root netem \
        delay "${HALF_RTT_MS}ms" rate "${RATE_MBIT}mbit" \
        loss random "${LOSS_PCT}%" limit "$QUEUE_PKTS"
fi
tc qdisc replace dev "$TUN_NAME" root netem \
    delay "${HALF_RTT_MS}ms" limit "$QUEUE_PKTS"

if [ "$LOSS_MODE" = deterministic-multi ] || [ "$LOSS_MODE" = deterministic-burst ] || [ "$LOSS_MODE" = deterministic-repeated-burst ] || [ "$LOSS_MODE" = deterministic-lost-retransmission ] || [ "$LOSS_MODE" = deterministic-first-send ]; then
    iptables -N "$FAULT_CHAIN"
    FAULT_CHAIN_CREATED=1
    iptables -I INPUT 1 -i "$TUN_NAME" -j "$FAULT_CHAIN"
    FAULT_JUMP_INSTALLED=1
    if [ "$LOSS_MODE" = deterministic-first-send ]; then
        marker_i=0
        while [ "$marker_i" -lt "$FAULT_MARKER_COUNT" ]; do
            marker=$(printf '%s%04d' "$FAULT_MARKER_PREFIX" "$marker_i")
            iptables -A "$FAULT_CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
                -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 \
                -m string --algo bm --string "$marker" \
                -m statistic --mode nth --every 10000 --packet 0 -j DROP
            marker_i=$((marker_i + 1))
        done
    elif [ "$LOSS_MODE" = deterministic-lost-retransmission ]; then
        # The payload marker identifies one exact stream region. The first
        # independent nth matcher drops the original marked packet. Because a
        # dropped packet never reaches the next rule, the second matcher sees
        # the first retransmission as its first match and drops that copy. Any
        # later retransmission passes both one-shot counters.
        fault_drop_i=0
        while [ "$fault_drop_i" -lt 2 ]; do
            iptables -A "$FAULT_CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
                -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 \
                -m string --algo bm --string "$FAULT_RETRANS_MARKER" \
                -m statistic --mode nth --every 10000 --packet 0 -j DROP
            fault_drop_i=$((fault_drop_i + 1))
        done
    elif [ "$LOSS_MODE" = deterministic-multi ]; then
        # Keep several successful packets between two one-shot losses so the
        # second hole is exposed by a partial ACK inside one recovery flight.
        iptables -A "$FAULT_CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
            -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 \
            -m statistic --mode nth --every 10000 --packet "$FAULT_FIRST_PACKET" -j DROP
        iptables -A "$FAULT_CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
            -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 \
            -m statistic --mode nth --every 10000 --packet "$FAULT_SECOND_PACKET" -j DROP
    else
        # Independent nth matchers in one group all use the same index. A packet
        # dropped by one rule never reaches the next rule, so the following
        # matcher hits the immediately following data packet and forms a
        # consecutive burst. repeated-burst adds later groups at fixed matcher
        # gaps; the large gap is chosen so the prior recovery can fully exit.
        repeat_i=0
        repeat_limit=1
        if [ "$LOSS_MODE" = deterministic-repeated-burst ]; then
            repeat_limit=$FAULT_BURST_REPEATS
        fi
        while [ "$repeat_i" -lt "$repeat_limit" ]; do
            burst_packet=$((FAULT_FIRST_PACKET + repeat_i * FAULT_BURST_GAP_PACKETS))
            [ "$burst_packet" -lt 10000 ] || {
                echo "deterministic burst matcher index must stay below 10000: $burst_packet" >&2
                exit 1
            }
            burst_i=0
            while [ "$burst_i" -lt "$FAULT_BURST_PACKETS" ]; do
                iptables -A "$FAULT_CHAIN" -s "$LWIP_IP" -d "$HOST_IP" \
                    -p tcp --sport "$PUBLIC_PORT" -m length --length 100:65535 \
                    -m statistic --mode nth --every 10000 --packet "$burst_packet" -j DROP
                burst_i=$((burst_i + 1))
            done
            repeat_i=$((repeat_i + 1))
        done
    fi
    iptables -A "$FAULT_CHAIN" -j RETURN
fi

tc -s qdisc show dev "$TUN_NAME" > "$OUT/tun-qdisc-before.txt"
tc -s qdisc show dev "$IFB_NAME" > "$OUT/ifb-qdisc-before.txt"

python3 - "$LWIP_IP" "$PUBLIC_PORT" "$PAYLOAD_BYTES" \
    > "$OUT/client.stdout" 2> "$OUT/client.stderr" <<'PY'
import hashlib
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
expected = int(sys.argv[3])
received = 0
digest = hashlib.sha256()
start = time.monotonic_ns()

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(90.0)
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

if [ "$LOSS_MODE" = deterministic-multi ] || [ "$LOSS_MODE" = deterministic-burst ] || [ "$LOSS_MODE" = deterministic-repeated-burst ] || [ "$LOSS_MODE" = deterministic-lost-retransmission ] || [ "$LOSS_MODE" = deterministic-first-send ]; then
    iptables -nvxL "$FAULT_CHAIN" > "$OUT/iptables-fault.txt"
fi

if ! wait "$BACKEND_PID"; then
    BACKEND_PID=
    cat "$OUT/backend.stderr" >&2 || true
    echo "P6 BBR long-flow backend failed" >&2
    exit 1
fi
BACKEND_PID=

tc -s qdisc show dev "$TUN_NAME" > "$OUT/tun-qdisc-after.txt"
tc -s qdisc show dev "$IFB_NAME" > "$OUT/ifb-qdisc-after.txt"

ifb_drops=$(sed -n 's/.*(dropped \([0-9][0-9]*\),.*/\1/p' "$OUT/ifb-qdisc-after.txt" | head -n 1)
tun_drops=$(sed -n 's/.*(dropped \([0-9][0-9]*\),.*/\1/p' "$OUT/tun-qdisc-after.txt" | head -n 1)
[ -n "$ifb_drops" ] && [ -n "$tun_drops" ] || {
    cat "$OUT/ifb-qdisc-after.txt" >&2 || true
    cat "$OUT/tun-qdisc-after.txt" >&2 || true
    echo "P6 BBR long-flow qdisc counters missing" >&2
    exit 1
}
fault_drops=0
case "$LOSS_MODE" in
    none)
        [ "$ifb_drops" -eq 0 ] && [ "$tun_drops" -eq 0 ] || {
            echo "P6 BBR clean long-flow qdisc dropped packets: ifb=$ifb_drops tun=$tun_drops" >&2
            exit 1
        }
        ;;
    random)
        [ "$ifb_drops" -ge 1 ] && [ "$tun_drops" -eq 0 ] || {
            echo "P6 BBR random-loss path did not isolate data loss: ifb=$ifb_drops tun=$tun_drops" >&2
            exit 1
        }
        ;;
    deterministic-multi)
        [ "$ifb_drops" -eq 0 ] && [ "$tun_drops" -eq 0 ] || {
            echo "P6 BBR deterministic-loss path had qdisc drops: ifb=$ifb_drops tun=$tun_drops" >&2
            exit 1
        }
        fault_drops=$(awk '$1 ~ /^[0-9]+$/ && $3 == "DROP" {sum += $1} END {print sum + 0}' \
            "$OUT/iptables-fault.txt")
        [ "$fault_drops" -eq 2 ] || {
            cat "$OUT/iptables-fault.txt" >&2 || true
            echo "P6 BBR deterministic multi-loss expected exactly two drops: drops=$fault_drops" >&2
            exit 1
        }
        ;;
    deterministic-burst)
        [ "$ifb_drops" -eq 0 ] && [ "$tun_drops" -eq 0 ] || {
            echo "P6 BBR deterministic burst path had qdisc drops: ifb=$ifb_drops tun=$tun_drops" >&2
            exit 1
        }
        fault_drops=$(awk '$1 ~ /^[0-9]+$/ && $3 == "DROP" {sum += $1} END {print sum + 0}' \
            "$OUT/iptables-fault.txt")
        [ "$fault_drops" -eq "$FAULT_BURST_PACKETS" ] || {
            cat "$OUT/iptables-fault.txt" >&2 || true
            echo "P6 BBR deterministic burst expected exactly $FAULT_BURST_PACKETS drops: drops=$fault_drops" >&2
            exit 1
        }
        ;;
    deterministic-repeated-burst)
        [ "$ifb_drops" -eq 0 ] && [ "$tun_drops" -eq 0 ] || {
            echo "P6 BBR repeated-burst path had qdisc drops: ifb=$ifb_drops tun=$tun_drops" >&2
            exit 1
        }
        fault_drops=$(awk '$1 ~ /^[0-9]+$/ && $3 == "DROP" {sum += $1} END {print sum + 0}' \
            "$OUT/iptables-fault.txt")
        expected_fault_drops=$((FAULT_BURST_PACKETS * FAULT_BURST_REPEATS))
        [ "$fault_drops" -eq "$expected_fault_drops" ] || {
            cat "$OUT/iptables-fault.txt" >&2 || true
            echo "P6 BBR repeated bursts expected exactly $expected_fault_drops drops: drops=$fault_drops" >&2
            exit 1
        }
        ;;
    deterministic-lost-retransmission)
        [ "$ifb_drops" -eq 0 ] && [ "$tun_drops" -eq 0 ] || {
            echo "P6 BBR lost-retransmission path had qdisc drops: ifb=$ifb_drops tun=$tun_drops" >&2
            exit 1
        }
        fault_drops=$(awk '$1 ~ /^[0-9]+$/ && $3 == "DROP" {sum += $1} END {print sum + 0}' \
            "$OUT/iptables-fault.txt")
        [ "$fault_drops" -eq 2 ] || {
            cat "$OUT/iptables-fault.txt" >&2 || true
            echo "P6 BBR lost-retransmission expected original + first retransmission drops: drops=$fault_drops" >&2
            exit 1
        }
        ;;
    deterministic-first-send)
        [ "$ifb_drops" -eq 0 ] && [ "$tun_drops" -eq 0 ] || {
            echo "P6 BBR first-send-loss path had qdisc drops: ifb=$ifb_drops tun=$tun_drops" >&2
            exit 1
        }
        fault_drops=$(awk '$1 ~ /^[0-9]+$/ && $3 == "DROP" {sum += $1} END {print sum + 0}' \
            "$OUT/iptables-fault.txt")
        [ "$fault_drops" -eq "$FAULT_MARKER_COUNT" ] || {
            cat "$OUT/iptables-fault.txt" >&2 || true
            echo "P6 BBR first-send-loss expected exactly $FAULT_MARKER_COUNT drops: drops=$fault_drops" >&2
            exit 1
        }
        ;;
esac

sleep 0.2
kill -TERM "$RUNTIME_PID"
if ! wait "$RUNTIME_PID"; then
    RUNTIME_PID=
    cat "$OUT/runtime.stderr" >&2 || true
    echo "P6 BBR long-flow runtime failed" >&2
    exit 1
fi
RUNTIME_PID=

cat "$OUT/client.stdout"
cat "$OUT/backend.stdout"
cat "$OUT/runtime.stderr" >&2

grep -F "cc=$CC" "$OUT/runtime.stdout" >/dev/null
grep -F 'cc_bindings=1' "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_bind_failures=0' "$OUT/runtime.stderr" >/dev/null
grep -F 'cc_controller_errors=0' "$OUT/runtime.stderr" >/dev/null

events=$(grep -m1 ' cc_bindings=' "$OUT/runtime.stderr")
policy_updates=$(printf '%s\n' "$events" | sed -n 's/.* cc_policy_updates=\([0-9][0-9]*\).*/\1/p')
loss_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_loss_events=\([0-9][0-9]*\).*/\1/p')
timeout_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_timeout_events=\([0-9][0-9]*\).*/\1/p')
cwnd_bytes=$(printf '%s\n' "$events" | sed -n 's/.* cc_last_cwnd=\([0-9][0-9]*\).*/\1/p')
[ -n "$policy_updates" ] && [ "$policy_updates" -ge 1 ] &&
[ -n "$loss_events" ] && [ -n "$timeout_events" ] &&
[ -n "$cwnd_bytes" ] && [ "$cwnd_bytes" -ge 1 ] || {
    echo "invalid BBR long-flow controller telemetry" >&2
    exit 1
}
case "$LOSS_MODE" in
    none)
        [ "$loss_events" -eq 0 ] && [ "$timeout_events" -eq 0 ] || {
            echo "clean BBR long-flow entered recovery: loss=$loss_events timeout=$timeout_events" >&2
            exit 1
        }
        ;;
    random)
        [ $((loss_events + timeout_events)) -ge 1 ] || {
            echo "random-loss BBR long-flow produced no recovery observation" >&2
            exit 1
        }
        ;;
    deterministic-multi)
        [ "$loss_events" -eq 1 ] && [ "$timeout_events" -eq 0 ] || {
            echo "deterministic BBR multi-loss did not stay in one recovery episode: loss=$loss_events timeout=$timeout_events" >&2
            exit 1
        }
        ;;
    deterministic-burst)
        [ "$loss_events" -eq 1 ] && [ "$timeout_events" -eq 0 ] || {
            echo "deterministic burst did not stay in one recovery episode: loss=$loss_events timeout=$timeout_events" >&2
            exit 1
        }
        ;;
    deterministic-repeated-burst)
        if [ "$RECOVERY_EXPECTATION" = strict ]; then
            [ "$loss_events" -ge 1 ] &&
            [ "$loss_events" -le "$FAULT_BURST_REPEATS" ] &&
            [ "$timeout_events" -eq 0 ] || {
                echo "repeated bursts did not stay in bounded fast recovery: repeats=$FAULT_BURST_REPEATS loss=$loss_events timeout=$timeout_events" >&2
                exit 1
            }
        else
            [ $((loss_events + timeout_events)) -ge 1 ] || {
                echo "diagnostic repeated bursts produced no recovery observation" >&2
                exit 1
            }
        fi
        ;;
    deterministic-lost-retransmission)
        [ "$loss_events" -ge 1 ] && [ "$timeout_events" -ge 1 ] || {
            echo "lost retransmission did not exercise fast-loss plus RTO fallback: loss=$loss_events timeout=$timeout_events" >&2
            exit 1
        }
        ;;
    deterministic-first-send)
        [ "$loss_events" -ge 1 ] &&
        [ "$loss_events" -le "$FAULT_MARKER_COUNT" ] &&
        [ "$timeout_events" -eq 0 ] || {
            echo "first-send-loss did not stay in bounded fast recovery: markers=$FAULT_MARKER_COUNT loss=$loss_events timeout=$timeout_events" >&2
            exit 1
        }
        ;;
esac

delivery=$(grep -m1 'tcp-shift-p2-delivery:' "$OUT/runtime.stderr")
delivered_bytes=$(printf '%s\n' "$delivery" | sed -n 's/.* delivered_payload_bytes=\([0-9][0-9]*\).*/\1/p')
retransmit_events=$(printf '%s\n' "$delivery" | sed -n 's/.* retransmit_events=\([0-9][0-9]*\).*/\1/p')
metadata_failures=$(printf '%s\n' "$delivery" | sed -n 's/.* metadata_alloc_failures=\([0-9][0-9]*\).*/\1/p')
metadata_misses=$(printf '%s\n' "$delivery" | sed -n 's/.* metadata_misses=\([0-9][0-9]*\).*/\1/p')
live_slots=$(printf '%s\n' "$delivery" | sed -n 's/.* live_slots=\([0-9][0-9]*\).*/\1/p')
[ -n "$delivered_bytes" ] && [ "$delivered_bytes" -eq "$PAYLOAD_BYTES" ] &&
[ -n "$retransmit_events" ] &&
[ -n "$metadata_failures" ] && [ "$metadata_failures" -eq 0 ] &&
[ -n "$metadata_misses" ] && [ "$metadata_misses" -eq 0 ] &&
[ -n "$live_slots" ] && [ "$live_slots" -eq 0 ] || {
    echo "invalid BBR long-flow delivery telemetry" >&2
    exit 1
}
case "$LOSS_MODE" in
    none)
        [ "$retransmit_events" -eq 0 ] || {
            echo "clean BBR long-flow retransmitted unexpectedly: $retransmit_events" >&2
            exit 1
        }
        ;;
    random)
        [ "$retransmit_events" -ge 1 ] || {
            echo "random-loss BBR long-flow observed no retransmission" >&2
            exit 1
        }
        ;;
    deterministic-multi)
        [ "$retransmit_events" -ge 2 ] || {
            echo "deterministic BBR multi-loss expected at least two retransmissions: $retransmit_events" >&2
            exit 1
        }
        ;;
    deterministic-burst)
        [ "$retransmit_events" -ge "$FAULT_BURST_PACKETS" ] || {
            echo "deterministic burst expected at least $FAULT_BURST_PACKETS retransmissions: $retransmit_events" >&2
            exit 1
        }
        ;;
    deterministic-repeated-burst)
        expected_retransmits=$((FAULT_BURST_PACKETS * FAULT_BURST_REPEATS))
        if [ "$RECOVERY_EXPECTATION" = strict ]; then
            [ "$retransmit_events" -eq "$expected_retransmits" ] || {
                echo "repeated bursts amplified retransmissions: expected=$expected_retransmits actual=$retransmit_events" >&2
                exit 1
            }
        else
            [ "$retransmit_events" -ge "$expected_retransmits" ] || {
                echo "repeated bursts expected at least $expected_retransmits retransmissions: $retransmit_events" >&2
                exit 1
            }
        fi
        ;;
    deterministic-lost-retransmission)
        [ "$retransmit_events" -ge 2 ] || {
            echo "lost retransmission expected selective + timeout retransmission activity: $retransmit_events" >&2
            exit 1
        }
        ;;
    deterministic-first-send)
        [ "$retransmit_events" -eq "$FAULT_MARKER_COUNT" ] || {
            echo "first-send-loss retransmission mismatch: expected=$FAULT_MARKER_COUNT actual=$retransmit_events" >&2
            exit 1
        }
        ;;
esac

rate=$(grep -m1 'tcp-shift-p2-rate:' "$OUT/runtime.stderr")
valid_samples=$(printf '%s\n' "$rate" | sed -n 's/.* valid_samples=\([0-9][0-9]*\).*/\1/p')
max_rate=$(printf '%s\n' "$rate" | sed -n 's/.* max_rate_bytes_per_sec=\([0-9][0-9]*\).*/\1/p')
[ -n "$valid_samples" ] && [ "$valid_samples" -ge 1 ] &&
[ -n "$max_rate" ] && [ "$max_rate" -ge 1 ] || {
    echo "invalid BBR long-flow rate telemetry" >&2
    exit 1
}

pacing=$(grep -m1 'tcp-shift-p2-pacing:' "$OUT/runtime.stderr")
pacing_deferrals=$(printf '%s\n' "$pacing" | sed -n 's/.* deferrals=\([0-9][0-9]*\).*/\1/p')
pacing_resumes=$(printf '%s\n' "$pacing" | sed -n 's/.* resume_events=\([0-9][0-9]*\).*/\1/p')
pacing_errors=$(printf '%s\n' "$pacing" | sed -n 's/.* scheduler_errors=\([0-9][0-9]*\).*/\1/p')
pacing_tx_bytes=$(printf '%s\n' "$pacing" | sed -n 's/.* tx_bytes=\([0-9][0-9]*\).*/\1/p')
pacing_max_tx_gap_ns=$(printf '%s\n' "$pacing" | sed -n 's/.* max_tx_gap_ns=\([0-9][0-9]*\).*/\1/p')
pacing_rate=$(printf '%s\n' "$pacing" | sed -n 's/.* last_rate_bytes_per_sec=\([0-9][0-9]*\).*/\1/p')
loop_errors=$(printf '%s\n' "$pacing" | sed -n 's/.* loop_callback_errors=\([0-9][0-9]*\).*/\1/p')
heap_current=$(printf '%s\n' "$pacing" | sed -n 's/.* heap_current=\([0-9][0-9]*\).*/\1/p')
[ -n "$pacing_deferrals" ] && [ "$pacing_deferrals" -ge 1 ] &&
[ -n "$pacing_resumes" ] && [ "$pacing_resumes" -ge 1 ] &&
[ -n "$pacing_errors" ] && [ "$pacing_errors" -eq 0 ] &&
[ -n "$pacing_tx_bytes" ] && [ "$pacing_tx_bytes" -ge "$PAYLOAD_BYTES" ] &&
[ -n "$pacing_max_tx_gap_ns" ] &&
[ -n "$pacing_rate" ] && [ "$pacing_rate" -ge 1 ] &&
[ -n "$loop_errors" ] && [ "$loop_errors" -eq 0 ] &&
[ -n "$heap_current" ] && [ "$heap_current" -eq 0 ] || {
    echo "invalid BBR long-flow shared-pacer telemetry" >&2
    exit 1
}
if [ "$LOSS_MODE" = none ] && [ "$pacing_tx_bytes" -ne "$PAYLOAD_BYTES" ]; then
    echo "clean BBR long-flow paced unexpected bytes: $pacing_tx_bytes" >&2
    exit 1
fi

bbr=$(grep -m1 'tcp-shift-p2-bbr:' "$OUT/runtime.stderr")
recovery_enter_events=$(printf '%s\n' "$bbr" | sed -n 's/.* recovery_enter_events=\([0-9][0-9]*\).*/\1/p')
recovery_exit_events=$(printf '%s\n' "$bbr" | sed -n 's/.* recovery_exit_events=\([0-9][0-9]*\).*/\1/p')
recovery_total_ns=$(printf '%s\n' "$bbr" | sed -n 's/.* recovery_total_ns=\([0-9][0-9]*\).*/\1/p')
recovery_max_ns=$(printf '%s\n' "$bbr" | sed -n 's/.* recovery_max_ns=\([0-9][0-9]*\).*/\1/p')
recovery_packet_conservation_acks=$(printf '%s\n' "$bbr" | sed -n 's/.* recovery_packet_conservation_acks=\([0-9][0-9]*\).*/\1/p')
recovery_last_enter_cwnd_bytes=$(printf '%s\n' "$bbr" | sed -n 's/.* recovery_last_enter_cwnd_bytes=\([0-9][0-9]*\).*/\1/p')
recovery_last_enter_inflight_bytes=$(printf '%s\n' "$bbr" | sed -n 's/.* recovery_last_enter_inflight_bytes=\([0-9][0-9]*\).*/\1/p')
recovery_min_cwnd_bytes=$(printf '%s\n' "$bbr" | sed -n 's/.* recovery_min_cwnd_bytes=\([0-9][0-9]*\).*/\1/p')
[ -n "$recovery_enter_events" ] && [ -n "$recovery_exit_events" ] &&
[ -n "$recovery_total_ns" ] && [ -n "$recovery_max_ns" ] &&
[ -n "$recovery_packet_conservation_acks" ] &&
[ -n "$recovery_last_enter_cwnd_bytes" ] &&
[ -n "$recovery_last_enter_inflight_bytes" ] &&
[ -n "$recovery_min_cwnd_bytes" ] || {
    echo "invalid BBR recovery diagnostic telemetry" >&2
    exit 1
}
if [ "$LOSS_MODE" = none ]; then
    [ "$recovery_enter_events" -eq 0 ] &&
    [ "$recovery_exit_events" -eq 0 ] &&
    [ "$recovery_total_ns" -eq 0 ] &&
    [ "$recovery_max_ns" -eq 0 ] || {
        echo "clean BBR long-flow recorded recovery diagnostics unexpectedly" >&2
        exit 1
    }
else
    [ "$recovery_enter_events" -ge 1 ] || {
        echo "lossy BBR long-flow recorded no recovery entry" >&2
        exit 1
    }
fi

client_sha=$(sed -n 's/.* sha256=\([0-9a-f][0-9a-f]*\).*/\1/p' "$OUT/client.stdout")
backend_sha=$(sed -n 's/.* sha256=\([0-9a-f][0-9a-f]*\).*/\1/p' "$OUT/backend.stdout")
goodput=$(sed -n 's/.* goodput_mbps=\([0-9.][0-9.]*\).*/\1/p' "$OUT/client.stdout")
[ -n "$client_sha" ] && [ "$client_sha" = "$backend_sha" ] && [ -n "$goodput" ] || {
    echo "P6 BBR long-flow payload/hash metrics missing or mismatched" >&2
    exit 1
}

printf 'p6_bbr_long_flow=ok cc=%s base_rtt_ms=%s rate_mbit=%s loss_pct=%s loss_mode=%s recovery_expectation=%s fault_burst_packets=%s fault_burst_repeats=%s fault_burst_gap_packets=%s fault_marker_count=%s fault_marker_gap_packets=%s fault_marker_prefix=%s bdp_bytes=%s queue_pkts=%s payload_bytes=%s goodput_mbps=%s cwnd_bytes=%s policy_updates=%s valid_rate_samples=%s max_rate_bytes_per_sec=%s pacing_deferrals=%s pacing_resumes=%s pacing_tx_bytes=%s pacing_max_tx_gap_ns=%s retransmit_events=%s qdisc_drops=%s/%s fault_drops=%s loss_events=%s timeout_events=%s recovery_enter_events=%s recovery_exit_events=%s recovery_total_ns=%s recovery_max_ns=%s recovery_packet_conservation_acks=%s recovery_last_enter_cwnd_bytes=%s recovery_last_enter_inflight_bytes=%s recovery_min_cwnd_bytes=%s payload_integrity=ok\n' \
    "$CC" "$RTT_MS" "$RATE_MBIT" "$LOSS_PCT" "$LOSS_MODE" "$RECOVERY_EXPECTATION" "$FAULT_BURST_PACKETS" "$FAULT_BURST_REPEATS" "$FAULT_BURST_GAP_PACKETS" "$FAULT_MARKER_COUNT" "$FAULT_MARKER_GAP_PACKETS" "$FAULT_MARKER_PREFIX" "$BDP_BYTES" "$QUEUE_PKTS" "$PAYLOAD_BYTES" \
    "$goodput" "$cwnd_bytes" "$policy_updates" "$valid_samples" "$max_rate" \
    "$pacing_deferrals" "$pacing_resumes" "$pacing_tx_bytes" "$pacing_max_tx_gap_ns" "$retransmit_events" \
    "$ifb_drops" "$tun_drops" "$fault_drops" "$loss_events" "$timeout_events" \
    "$recovery_enter_events" "$recovery_exit_events" "$recovery_total_ns" "$recovery_max_ns" \
    "$recovery_packet_conservation_acks" "$recovery_last_enter_cwnd_bytes" \
    "$recovery_last_enter_inflight_bytes" "$recovery_min_cwnd_bytes" | tee "$OUT/summary.txt"

printf 'cc=%s\nbase_rtt_ms=%s\nrate_mbit=%s\nloss_pct=%s\nloss_mode=%s\nrecovery_expectation=%s\nfault_burst_packets=%s\nfault_burst_repeats=%s\nfault_burst_gap_packets=%s\nfault_marker_count=%s\nfault_marker_gap_packets=%s\nfault_marker_prefix=%s\nfault_drops=%s\nbdp_bytes=%s\nqueue_pkts=%s\npayload_bytes=%s\n' \
    "$CC" "$RTT_MS" "$RATE_MBIT" "$LOSS_PCT" "$LOSS_MODE" "$RECOVERY_EXPECTATION" "$FAULT_BURST_PACKETS" "$FAULT_BURST_REPEATS" "$FAULT_BURST_GAP_PACKETS" "$FAULT_MARKER_COUNT" "$FAULT_MARKER_GAP_PACKETS" "$FAULT_MARKER_PREFIX" "$fault_drops" "$BDP_BYTES" "$QUEUE_PKTS" "$PAYLOAD_BYTES" \
    > "$OUT/path.env"
echo "P6 internal BBR long-flow shared-pacer qualification passed"
