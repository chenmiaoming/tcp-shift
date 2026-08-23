#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIN_FILE="$ROOT/runtime/linux/lkl-reference.commit"
DEPS_DIR="${DEPS_DIR:-$ROOT/.deps}"
SRC="$DEPS_DIR/lkl-linux"
REF="${LKL_REF:-$(tr -d '[:space:]' < "$PIN_FILE") }"

if [[ -z "$REF" ]]; then
  echo "empty LKL reference" >&2
  exit 1
fi

mkdir -p "$DEPS_DIR"
if [[ ! -d "$SRC/.git" ]]; then
  git clone --no-checkout https://github.com/lkl/linux.git "$SRC"
fi

git -C "$SRC" fetch --depth=1 origin "$REF"
git -C "$SRC" checkout --detach FETCH_HEAD

make -C "$SRC" ARCH=lkl defconfig

required=(
  CONFIG_NET=y
  CONFIG_INET=y
  CONFIG_TCP_CONG_ADVANCED=y
  CONFIG_TCP_CONG_BBR=y
  CONFIG_NET_SCHED=y
  CONFIG_NET_SCH_FQ=y
  CONFIG_NETDEVICES=y
  CONFIG_VIRTIO_NET=y
  CONFIG_HIGH_RES_TIMERS=y
)

for opt in "${required[@]}"; do
  if ! grep -qx "$opt" "$SRC/.config"; then
    echo "required LKL config option missing: $opt" >&2
    exit 1
  fi
done

# Build only the embeddable kernel library, not the filesystem/demo tools.
make -C "$SRC/tools/lkl" -j"$(nproc)" liblkl.a

LIB="$SRC/tools/lkl/liblkl.a"
[[ -s "$LIB" ]]

printf 'LKL reference: %s\n' "$(git -C "$SRC" rev-parse HEAD)"
printf 'Kernel version: '
make -s -C "$SRC" kernelversion
printf 'liblkl.a size: '
du -h "$LIB" | cut -f1

# Record enough provenance for CI artifacts and later comparisons.
mkdir -p "$ROOT/.bench/linux-runtime-reference"
cp "$SRC/.config" "$ROOT/.bench/linux-runtime-reference/lkl.config"
printf '%s\n' "$(git -C "$SRC" rev-parse HEAD)" > "$ROOT/.bench/linux-runtime-reference/lkl.commit"
printf '%s\n' "$(stat -c '%s' "$LIB")" > "$ROOT/.bench/linux-runtime-reference/liblkl.bytes"
