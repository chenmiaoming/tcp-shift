#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${TCP_SHIFT_ECN_BUILD:-"$ROOT/.build-ecn"}
BINARY=${TCP_SHIFT_ECN_BINARY:-"$BUILD/tcp-shift-p2"}
OUT=${TCP_SHIFT_ECN_OUT:-"$ROOT/.build/ecn-live"}
TUN_NAME=${TCP_SHIFT_ECN_TUN_NAME:-tsecn$$}
LWIP_IP=${TCP_SHIFT_ECN_LWIP_IP:-10.252.0.2}
HOST_IP=${TCP_SHIFT_ECN_HOST_IP:-10.252.0.1}
NETMASK=255.255.255.252
PUBLIC_PORT=${TCP_SHIFT_ECN_PUBLIC_PORT:-18816}
BACKEND_PORT=${TCP_SHIFT_ECN_BACKEND_PORT:-19816}
CHAIN="TSECN$$"
PCAP="$OUT/ecn.pcap"
RELEASE="$OUT/release-backend"

RUNTIME_PID=
BACKEND_PID=
CLIENT_PID=
TCPDUMP_PID=
ECN_SYSCTL_OLD=
RULE_INSTALLED=0

mkdir -p "$OUT"
rm -f "$RELEASE" "$PCAP"
: > "$OUT/runtime.stdout"
: > "$OUT/runtime.stderr"
: > "$OUT/backend.stdout"
: > "$OUT/backend.stderr"
: > "$OUT/client.stdout"
: > "$OUT/client.stderr"

[ "$(id -u)" -eq 0 ] || {
    echo "RFC 3168 live qualification requires root" >&2
    exit 1
}
[ -x "$BINARY" ] || { echo "missing ECN binary: $BINARY" >&2; exit 1; }
for tool in ip iptables tcpdump python3 sysctl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool is required for RFC 3168 live qualification" >&2
        exit 1
    }
done

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
    stop_pid "${TCPDUMP_PID:-}"
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

python3 - "$BACKEND_PORT" "$RELEASE" >"$OUT/backend.stdout" 2>"$OUT/backend.stderr" <<'PY' &
import os
import socket
import sys
import time

port = int(sys.argv[1])
release = sys.argv[2]
first = bytes((i * 17 + 3) & 0xff for i in range(1200))
second = bytes((i * 29 + 11) & 0xff for i in range(5200))
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(1)
print(f"backend-ready port={port}", flush=True)
conn, _ = server.accept()
conn.settimeout(10.0)
print("backend-accepted", flush=True)
deadline = time.monotonic() + 10.0
while not os.path.exists(release):
    if time.monotonic() >= deadline:
        raise SystemExit("release timeout")
    time.sleep(0.01)
conn.sendall(first)
print(f"backend-first={len(first)}", flush=True)
# Give the CE-marked first flight time to produce an ECE ACK and let tcp-shift
# publish the ECN congestion response before new data is queued.
time.sleep(0.75)
conn.sendall(second)
print(f"backend-second={len(second)}", flush=True)
conn.shutdown(socket.SHUT_WR)
while conn.recv(4096):
    pass
conn.close()
server.close()
print(f"backend-total={len(first) + len(second)}", flush=True)
PY
BACKEND_PID=$!

i=0
while [ "$i" -lt 100 ] && ! grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$BACKEND_PID" 2>/dev/null || {
        cat "$OUT/backend.stderr" >&2
        exit 1
    }
    i=$((i + 1)); sleep 0.05
done
grep -F 'backend-ready ' "$OUT/backend.stdout" >/dev/null || {
    echo "backend ready timeout" >&2
    exit 1
}

"$BINARY" "$TUN_NAME" "$LWIP_IP" "$NETMASK" "$HOST_IP"     "$PUBLIC_PORT" "$BACKEND_PORT" cubic     >"$OUT/runtime.stdout" 2>"$OUT/runtime.stderr" &
RUNTIME_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    if ip link show "$TUN_NAME" >/dev/null 2>&1 &&
       grep -F "tcp-shift-p2: ready tun=$TUN_NAME" "$OUT/runtime.stdout" >/dev/null 2>&1; then
        break
    fi
    kill -0 "$RUNTIME_PID" 2>/dev/null || {
        cat "$OUT/runtime.stderr" >&2 || true
        exit 1
    }
    i=$((i + 1)); sleep 0.05
done
[ "$i" -lt 100 ] || { echo "runtime ready timeout" >&2; exit 1; }
grep -F 'cc=cubic' "$OUT/runtime.stdout" >/dev/null

tcpdump -i "$TUN_NAME" -s 0 -U -w "$PCAP" "tcp port $PUBLIC_PORT"     >"$OUT/tcpdump.stdout" 2>"$OUT/tcpdump.stderr" &
TCPDUMP_PID=$!
sleep 0.15

python3 - "$LWIP_IP" "$PUBLIC_PORT" >"$OUT/client.stdout" 2>"$OUT/client.stderr" <<'PY' &
import hashlib
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
expected = 6400
digest = hashlib.sha256()
received = 0
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(12.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1460)
    sock.connect((host, port))
    while True:
        chunk = sock.recv(8192)
        if not chunk:
            break
        received += len(chunk)
        digest.update(chunk)
    sock.shutdown(socket.SHUT_WR)
if received != expected:
    raise SystemExit(f"received={received} expected={expected}")
print(f"client-bytes={received} sha256={digest.hexdigest()}")
PY
CLIENT_PID=$!

i=0
while [ "$i" -lt 120 ] && ! grep -F 'backend-accepted' "$OUT/backend.stdout" >/dev/null 2>&1; do
    kill -0 "$CLIENT_PID" 2>/dev/null || {
        cat "$OUT/client.stderr" >&2 || true
        exit 1
    }
    kill -0 "$BACKEND_PID" 2>/dev/null || {
        cat "$OUT/backend.stderr" >&2 || true
        exit 1
    }
    i=$((i + 1)); sleep 0.05
done
grep -F 'backend-accepted' "$OUT/backend.stdout" >/dev/null || {
    echo "backend accept timeout" >&2
    exit 1
}

# Mark exactly the first tcp-shift -> host data packet CE. The very large nth
# interval keeps the remainder ECT(0), so the trace can prove ECE stops after
# CWR rather than remaining latched because every packet was CE-marked.
iptables -t mangle -N "$CHAIN"
iptables -t mangle -I PREROUTING 1 -i "$TUN_NAME" -j "$CHAIN"
iptables -t mangle -A "$CHAIN"     -s "$LWIP_IP" -d "$HOST_IP" -p tcp --sport "$PUBLIC_PORT"     -m length --length 80:65535     -m statistic --mode nth --every 1000000 --packet 0     -j TOS --set-tos 0x03/0x03
iptables -t mangle -A "$CHAIN" -j RETURN
RULE_INSTALLED=1
touch "$RELEASE"

wait "$CLIENT_PID" || {
    CLIENT_PID=
    cat "$OUT/client.stderr" >&2 || true
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
[ -n "$marked" ] && [ "$marked" -eq 1 ] || {
    cat "$OUT/mangle-rule.txt" >&2
    echo "expected exactly one CE-marked data packet, observed ${marked:-none}" >&2
    exit 1
}

sleep 0.25
stop_pid "$TCPDUMP_PID"
TCPDUMP_PID=
sleep 0.15

kill -TERM "$RUNTIME_PID"
wait "$RUNTIME_PID" || {
    RUNTIME_PID=
    cat "$OUT/runtime.stderr" >&2 || true
    exit 1
}
RUNTIME_PID=

python3 - "$PCAP" "$LWIP_IP" "$HOST_IP" "$PUBLIC_PORT" > "$OUT/trace-summary.txt" <<'PY'
import ipaddress
import struct
import sys

path, lwip_text, host_text, port_text = sys.argv[1:]
lwip = ipaddress.IPv4Address(lwip_text).packed
host = ipaddress.IPv4Address(host_text).packed
port = int(port_text)

with open(path, "rb") as fh:
    raw = fh.read()
if len(raw) < 24:
    raise SystemExit("pcap too short")
magic = raw[:4]
if magic == b"\xd4\xc3\xb2\xa1":
    endian = "<"
elif magic == b"\xa1\xb2\xc3\xd4":
    endian = ">"
elif magic in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d"):
    endian = "<" if magic[0] == 0x4d else ">"
else:
    raise SystemExit(f"unsupported pcap magic={magic.hex()}")
linktype = struct.unpack(endian + "I", raw[20:24])[0]

def ip_offset(frame):
    if linktype == 1:      # Ethernet
        return 14
    if linktype in (12, 101):  # DLT/LINKTYPE_RAW
        return 0
    if linktype == 113:    # Linux cooked v1
        return 16
    if linktype == 276:    # Linux cooked v2
        return 20
    raise SystemExit(f"unsupported pcap linktype={linktype}")

packets = []
off = 24
index = 0
while off + 16 <= len(raw):
    _, _, incl, _ = struct.unpack(endian + "IIII", raw[off:off+16])
    off += 16
    frame = raw[off:off+incl]
    off += incl
    index += 1
    io = ip_offset(frame)
    if len(frame) < io + 20 or frame[io] >> 4 != 4:
        continue
    ihl = (frame[io] & 0x0f) * 4
    total = struct.unpack("!H", frame[io+2:io+4])[0]
    if frame[io+9] != 6 or len(frame) < io + ihl + 20:
        continue
    src, dst = frame[io+12:io+16], frame[io+16:io+20]
    to = io + ihl
    sport, dport = struct.unpack("!HH", frame[to:to+4])
    if sport != port and dport != port:
        continue
    doff = (frame[to+12] >> 4) * 4
    flags = frame[to+13]
    payload = max(0, total - ihl - doff)
    packets.append({
        "index": index, "src": src, "dst": dst, "sport": sport, "dport": dport,
        "ecn": frame[io+1] & 0x03, "flags": flags, "payload": payload,
    })

SYN, ACK, ECE, CWR = 0x02, 0x10, 0x40, 0x80
host_syn = next((p for p in packets if p["src"] == host and
                 (p["flags"] & (SYN|ACK)) == SYN), None)
synack = next((p for p in packets if p["src"] == lwip and
               (p["flags"] & (SYN|ACK)) == (SYN|ACK)), None)
if host_syn is None or (host_syn["flags"] & (ECE|CWR)) != (ECE|CWR):
    raise SystemExit("client SYN did not negotiate ECN with ECE+CWR")
if host_syn["ecn"] != 0:
    raise SystemExit(f"client SYN must be Not-ECT, ecn={host_syn['ecn']}")
if synack is None or (synack["flags"] & ECE) == 0 or (synack["flags"] & CWR) != 0:
    raise SystemExit("tcp-shift SYN-ACK must carry ECE and not CWR")
if synack["ecn"] != 0:
    raise SystemExit(f"tcp-shift SYN-ACK must be Not-ECT, ecn={synack['ecn']}")

data = [p for p in packets if p["src"] == lwip and p["payload"] > 0]
if len(data) < 2:
    raise SystemExit(f"expected >=2 tcp-shift data packets, got {len(data)}")
if data[0]["ecn"] not in (2, 3):
    raise SystemExit(f"first data must be ECT(0) before/CE after marking, ecn={data[0]['ecn']}")

ece_acks = [p for p in packets if p["src"] == host and
            (p["flags"] & (ACK|ECE)) == (ACK|ECE) and p["index"] > data[0]["index"]]
if not ece_acks:
    raise SystemExit("no ECE ACK observed after CE-marked data")
first_ece = ece_acks[0]

cwr_data = next((p for p in data if p["index"] > first_ece["index"] and
                 (p["flags"] & CWR) != 0), None)
if cwr_data is None:
    raise SystemExit("no CWR on first new data after ECE")
if cwr_data["ecn"] != 2:
    raise SystemExit(f"CWR new-data packet must be ECT(0), ecn={cwr_data['ecn']}")

post_cwr_ack = next((p for p in packets if p["src"] == host and
                     p["index"] > cwr_data["index"] and
                     (p["flags"] & ACK) != 0 and (p["flags"] & ECE) == 0), None)
if post_cwr_ack is None:
    raise SystemExit("receiver did not stop ECE after CWR")

print(
    "rfc3168_live=ok "
    f"linktype={linktype} packets={len(packets)} data_packets={len(data)} "
    f"syn_index={host_syn['index']} synack_index={synack['index']} "
    f"first_data_index={data[0]['index']} first_ece_index={first_ece['index']} "
    f"cwr_data_index={cwr_data['index']} post_cwr_ack_index={post_cwr_ack['index']}"
)
PY

events=$(grep -m1 ' cc_bindings=' "$OUT/runtime.stderr")
ecn_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_ecn_events=\([0-9][0-9]*\).*/\1/p')
loss_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_loss_events=\([0-9][0-9]*\).*/\1/p')
timeout_events=$(printf '%s\n' "$events" | sed -n 's/.* cc_timeout_events=\([0-9][0-9]*\).*/\1/p')
[ -n "$ecn_events" ] && [ "$ecn_events" -ge 1 ] || {
    echo "missing controller ECN event" >&2
    cat "$OUT/runtime.stderr" >&2
    exit 1
}
[ -n "$loss_events" ] && [ "$loss_events" -eq 0 ] || {
    echo "CE incorrectly became a packet-loss event" >&2
    exit 1
}
[ -n "$timeout_events" ] && [ "$timeout_events" -eq 0 ] || {
    echo "ordinary timeout unexpectedly fired in single-CE live case" >&2
    exit 1
}

cat "$OUT/trace-summary.txt"
printf 'rfc3168_controller=ok ecn_events=%s loss_events=%s timeout_events=%s ce_marks=%s\n'     "$ecn_events" "$loss_events" "$timeout_events" "$marked"
