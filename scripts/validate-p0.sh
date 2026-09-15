#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
COMPILE_COMMANDS="$BUILD/compile_commands.json"
TARGET_MANIFEST="$BUILD/CMakeFiles/tcp_shift_lwip.dir/build.make"
BINARY="$BUILD/tcp-shift"
CONTRACT="$BUILD/tcp-shift-config-contract"
MAX_BINARY_BYTES=${TCP_SHIFT_P0_MAX_BINARY_BYTES:-524288}
RSS_LIMIT_KIB=${TCP_SHIFT_P0_MAX_RSS_KIB:-16384}

fail()
{
    echo "P0 validation failed: $*" >&2
    exit 1
}

[ -f "$COMPILE_COMMANDS" ] || fail "missing compile_commands.json"
[ -f "$TARGET_MANIFEST" ] || fail "missing tcp_shift_lwip build manifest"
[ -x "$BINARY" ] || fail "missing tcp-shift binary"
[ -x "$CONTRACT" ] || fail "missing config contract binary"

# Filelists.cmake defines broad EXCLUDE_FROM_ALL targets, so the global CMake
# compile database can contain commands for sources that were never built.
# Enforce the product boundary against the manifest for the actual linked
# tcp_shift_lwip target instead.
for required in \
    '/src/core/tcp.c' \
    '/src/core/tcp_in.c' \
    '/src/core/tcp_out.c' \
    '/src/core/ipv4/ip4.c' \
    '/src/core/ipv6/ip6.c' \
    '/src/core/ipv6/icmp6.c' \
    '/src/core/ipv6/nd6.c'
do
    grep -F "$required" "$TARGET_MANIFEST" >/dev/null || \
        fail "required lwIP source not compiled: $required"
done

# IPv6 is explicit, but the low-memory L3 TUN profile excludes Ethernet,
# dynamic address control planes, endpoint fragmentation/reassembly, sequential
# APIs and unrelated network-interface families.
for forbidden in \
    '/src/core/ipv6/ethip6.c' \
    '/src/core/ipv6/dhcp6.c' \
    '/src/core/ipv6/mld6.c' \
    '/src/core/ipv6/ip6_frag.c' \
    '/src/api/' \
    '/src/netif/ppp/' \
    '/src/netif/lowpan6' \
    '/contrib/ports/unix/port/sys_arch.c'
do
    if grep -F "$forbidden" "$TARGET_MANIFEST" >/dev/null; then
        fail "forbidden P0 source compiled: $forbidden"
    fi
done

"$CONTRACT" | tee "$BUILD/p0-config-contract.txt"
grep -F 'config_contract=ok window_scaling=1 tcp_rcv_scale=0 tcpwnd_size_bytes=4 ' \
    "$BUILD/p0-config-contract.txt" >/dev/null || \
    fail "window-scaling config contract did not report the qualified 32-bit profile"

"$BINARY" > "$BUILD/p0-smoke.txt"
grep -F 'tcp-shift: lwIP ' "$BUILD/p0-smoke.txt" >/dev/null || \
    fail "smoke output does not identify initialized lwIP"

nm -g "$BINARY" > "$BUILD/p0-symbols.txt"
if grep -E ' (lwip_socket|lwip_accept|netconn_|udp_new|udp_bind|udp_send)' "$BUILD/p0-symbols.txt" >/dev/null; then
    fail "forbidden socket/netconn/UDP symbol reached the linked binary"
fi

BINARY_BYTES=$(stat -c '%s' "$BINARY")
[ "$BINARY_BYTES" -le "$MAX_BINARY_BYTES" ] || \
    fail "binary size $BINARY_BYTES exceeds $MAX_BINARY_BYTES bytes"

command -v /usr/bin/time >/dev/null 2>&1 || fail "/usr/bin/time is required"
/usr/bin/time -f '%M' -o "$BUILD/p0-maxrss-kib.txt" "$BINARY" >/dev/null
RSS_SAMPLE_KIB=$(tr -d '[:space:]' < "$BUILD/p0-maxrss-kib.txt")
case "$RSS_SAMPLE_KIB" in
    ''|*[!0-9]*) fail "invalid maximum RSS sample: $RSS_SAMPLE_KIB" ;;
esac
[ "$RSS_SAMPLE_KIB" -le "$RSS_LIMIT_KIB" ] || \
    fail "maximum RSS $RSS_SAMPLE_KIB KiB exceeds $RSS_LIMIT_KIB KiB"

readelf -h "$BINARY" > "$BUILD/p0-elf-header.txt"
size "$BINARY" > "$BUILD/p0-size.txt"

cat > "$BUILD/p0-report.json" <<EOF
{
  "schema": "tcp-shift.p0.v1",
  "binary_bytes": $BINARY_BYTES,
  "binary_limit_bytes": $MAX_BINARY_BYTES,
  "max_rss_kib": $RSS_SAMPLE_KIB,
  "max_rss_limit_kib": $RSS_LIMIT_KIB,
  "source_surface": "dualstack-tcp-no-sys-no-ipv6-frag",
  "window_scaling": true,
  "tcp_rcv_scale": 0,
  "tcpwnd_size_bytes": 4
}
EOF

cat "$BUILD/p0-report.json"