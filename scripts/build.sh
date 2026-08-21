#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
GVISOR_SHA=${GVISOR_SHA:-80336ad549d71d82c7f77a79cf91d628fa34135a}
GVISOR_DIR="$ROOT/.deps/gvisor"

mkdir -p "$ROOT/.deps" "$ROOT/bin"

if [[ ! -d "$GVISOR_DIR/.git" ]]; then
  rm -rf "$GVISOR_DIR"
  git init -q "$GVISOR_DIR"
  git -C "$GVISOR_DIR" remote add origin https://github.com/google/gvisor.git
  git -C "$GVISOR_DIR" fetch --depth=1 origin "$GVISOR_SHA"
  git -C "$GVISOR_DIR" checkout -q --detach FETCH_HEAD
fi

actual=$(git -C "$GVISOR_DIR" rev-parse HEAD)
if [[ "$actual" != "$GVISOR_SHA" ]]; then
  echo "gVisor checkout mismatch: expected $GVISOR_SHA, got $actual" >&2
  exit 1
fi

# Always reset before patching so repeated builds are deterministic.
git -C "$GVISOR_DIR" reset -q --hard "$GVISOR_SHA"
git -C "$GVISOR_DIR" clean -q -fd
python3 "$ROOT/scripts/patch_gvisor.py" "$GVISOR_DIR"

gofmt -w \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/bbr.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/protocol.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/snd.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/endpoint_state.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/endpoint.go"

cp "$ROOT/go.mod" "$ROOT/go.local.mod"
rm -f "$ROOT/go.local.sum"
go mod edit -modfile="$ROOT/go.local.mod" -replace="gvisor.dev/gvisor=$GVISOR_DIR"
go mod tidy -modfile="$ROOT/go.local.mod"

go build -modfile="$ROOT/go.local.mod" -trimpath -ldflags='-s -w' -o "$ROOT/bin/tcp-shift" "$ROOT/cmd/tcp-shift"

printf 'built %s using gVisor %s\n' "$ROOT/bin/tcp-shift" "$GVISOR_SHA"
