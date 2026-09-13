#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build"
COMPILE_COMMANDS="$BUILD/compile_commands.json"
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
[ -x "$BINARY" ] || fail "missing tcp-shift binary"
[ -x "$CONTRACT" ] || fail "missing config contract binary"

for required in \
    '/src/core/tcp.c' \
    '/src/core/tcp_in.c' \
    '/src/core/tcp_out.c' \
    '/src/core/ipv4/ip4.c'
do
    grep -F "$required" "$COMPILE_COMMANDS" >/dev/null || \
        fail "required lwIP source not compiled: $required"
done

for forbidden in \
    '/src/core/ipv6/' \
    '/src/api/' \
    '/src/netif/ppp/' \
    '/src/netif/lowpan6' \
    '/contrib/ports/unix/port/sys_arch.c'
do
    if grep -F "$forbidden" "$COMPILE_COMMANDS" >/dev/null; then
        fail "forbidden P0 source compiled: $forbidden"
    fi
done

"$CONTRACT"
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
  "source_surface": "ipv4-tcp-no-sys",
  "window_scaling": false
}
EOF

cat "$BUILD/p0-report.json"
