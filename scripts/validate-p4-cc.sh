#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/.build/p4-cc"
OUT="$BUILD/evidence"

rm -rf "$BUILD"
mkdir -p "$OUT"

# The CC source surface may use only ISO C headers plus its own public headers.
# Do not let lwIP or Linux integration leak into the independently buildable
# policy library.
find "$ROOT/src/cc" -type f \( -name '*.c' -o -name '*.h' \) -print | sort \
    > "$OUT/source-files.txt"
grep -h '^[[:space:]]*#include' "$ROOT"/src/cc/*.c "$ROOT"/src/cc/*.h \
    | sort -u > "$OUT/includes.txt" || true

if awk '
    $0 == "#include <limits.h>" { next }
    $0 == "#include <stddef.h>" { next }
    $0 == "#include <stdint.h>" { next }
    $0 == "#include \"cc/cc.h\"" { next }
    $0 == "#include \"cc/registry.h\"" { next }
    $0 == "#include \"cc/reno.h\"" { next }
    $0 == "#include \"cc/bbr.h\"" { next }
    $0 == "#include \"cc/cubic.h\"" { next }
    { print; bad = 1 }
    END { exit bad ? 0 : 1 }
' "$OUT/includes.txt" > "$OUT/forbidden-includes.txt"; then
    echo "P4 CC core contains forbidden dependency includes:" >&2
    cat "$OUT/forbidden-includes.txt" >&2
    exit 1
fi
rm -f "$OUT/forbidden-includes.txt"

cmake -S "$ROOT/src/cc" -B "$BUILD/standalone" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS='-ffreestanding -fno-builtin'
cmake --build "$BUILD/standalone" --parallel

"$BUILD/standalone/tcp-shift-cc-contract" | tee "$OUT/contract.txt"
grep -F 'cc_contract=ok controller=reno state_bytes=16 pacing=none' \
    "$OUT/contract.txt" >/dev/null

"$BUILD/standalone/tcp-shift-cc-registry-contract" | tee "$OUT/registry-contract.txt"
grep -F 'cc_registry=ok default=reno available=reno unavailable=cubic,bbr,bbrv3' \
    "$OUT/registry-contract.txt" >/dev/null

"$BUILD/standalone/tcp-shift-cubic-model-contract" | tee "$OUT/cubic-contract.txt"
grep -F 'cubic_model_contract=ok beta=7/10 C=2/5 reno_alpha=9/17' \
    "$OUT/cubic-contract.txt" >/dev/null

LIB="$BUILD/standalone/libtcp_shift_cc.a"
test -f "$LIB"
ar t "$LIB" | tee "$OUT/archive-members.txt"
nm -g --defined-only "$LIB" | tee "$OUT/defined-symbols.txt"
nm -u "$LIB" | tee "$OUT/undefined-symbols.txt"

if awk '$1 == "U" { print; found = 1 } END { exit found ? 0 : 1 }' \
    "$OUT/undefined-symbols.txt" > "$OUT/unexpected-undefined.txt"; then
    echo "P4 CC archive has external symbol dependencies:" >&2
    cat "$OUT/unexpected-undefined.txt" >&2
    exit 1
fi
rm -f "$OUT/unexpected-undefined.txt"

# Record object/text/data size as the fixed pre-adapter P4 baseline. This is not
# process residency; later P4/P5/P6 measurements still compare real PSS to P3.
size "$LIB" | tee "$OUT/archive-size.txt"

printf 'cc_boundary=pure-c\ncontroller=reno\nregistry=ok\ncubic_model=ok\nstate_bytes=16\nexternal_symbols=0\n' \
    | tee "$OUT/summary.txt"
echo "P4 standalone congestion-control boundary passed"
