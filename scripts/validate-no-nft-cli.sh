#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

fail()
{
    echo "nft CLI dependency check failed: $*" >&2
    exit 1
}

check_file()
{
    file=$1

    if grep -En '^[[:space:]]*(if[[:space:]]+)?(sudo[[:space:]]+)?nft([[:space:]]|$)' "$file" >/dev/null; then
        grep -En '^[[:space:]]*(if[[:space:]]+)?(sudo[[:space:]]+)?nft([[:space:]]|$)' "$file" >&2 || true
        fail "external nft command invocation found in $file"
    fi
    if grep -En '(^|[;&|][[:space:]]*)(sudo[[:space:]]+)?nft([[:space:]]|$)' "$file" >/dev/null; then
        grep -En '(^|[;&|][[:space:]]*)(sudo[[:space:]]+)?nft([[:space:]]|$)' "$file" >&2 || true
        fail "external nft command invocation found in $file"
    fi
}

find "$ROOT/scripts" "$ROOT/.github" -type f \( -name '*.sh' -o -name '*.yml' -o -name '*.yaml' \) -print |
while IFS= read -r file; do
    check_file "$file"
done

if grep -R -nE 'exec[a-zA-Z0-9_]*\([^;]*(/nft|"nft")' "$ROOT/src" >/dev/null 2>&1; then
    grep -R -nE 'exec[a-zA-Z0-9_]*\([^;]*(/nft|"nft")' "$ROOT/src" >&2 || true
    fail "source tree contains an nft executable launch"
fi

if grep -R -nE '^[[:space:]]+nftables[[:space:]]*\\$' "$ROOT/.github/workflows" >/dev/null 2>&1; then
    grep -R -nE '^[[:space:]]+nftables[[:space:]]*\\$' "$ROOT/.github/workflows" >&2 || true
    fail "workflow installs the nftables CLI package"
fi

echo "nft_cli_dependency=absent libnftables_control=required"
