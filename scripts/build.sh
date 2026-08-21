#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

BASELINE_FILE="$ROOT/.gvisor-baseline"
GVISOR_REMOTE=${GVISOR_REMOTE:-https://github.com/google/gvisor.git}
GVISOR_REF=${GVISOR_REF:-}
GVISOR_DIR=${GVISOR_DIR:-"$ROOT/.deps/gvisor"}
TCP_SHIFT_VERSION=${TCP_SHIFT_VERSION:-dev}

if [[ -z "$GVISOR_REF" ]]; then
  if [[ ! -f "$BASELINE_FILE" ]]; then
    echo "missing $BASELINE_FILE and GVISOR_REF is not set" >&2
    exit 1
  fi
  GVISOR_REF=$(awk 'NF && $1 !~ /^#/ { print $1; exit }' "$BASELINE_FILE")
fi
if [[ -z "$GVISOR_REF" ]]; then
  echo "empty gVisor ref" >&2
  exit 1
fi

mkdir -p "$ROOT/.deps" "$ROOT/bin"

if [[ ! -d "$GVISOR_DIR/.git" ]]; then
  rm -rf "$GVISOR_DIR"
  git init -q "$GVISOR_DIR"
  git -C "$GVISOR_DIR" remote add origin "$GVISOR_REMOTE"
else
  git -C "$GVISOR_DIR" remote set-url origin "$GVISOR_REMOTE"
fi

# GVISOR_REF may be a verified SHA, a tag, or a moving ref such as master.
# Always resolve it to a concrete commit before patching/building so every
# produced binary can report the exact gVisor source revision it contains.
git -C "$GVISOR_DIR" fetch -q --depth=1 origin "$GVISOR_REF"
GVISOR_SHA=$(git -C "$GVISOR_DIR" rev-parse FETCH_HEAD)
git -C "$GVISOR_DIR" checkout -q --detach "$GVISOR_SHA"
git -C "$GVISOR_DIR" reset -q --hard "$GVISOR_SHA"
git -C "$GVISOR_DIR" clean -q -fd

python3 "$ROOT/scripts/patch_gvisor.py" "$GVISOR_DIR"

# gVisor is Bazel-first. A few _test.go files use package layouts that are
# valid for its Bazel targets but not for an external conventional Go module.
# This checkout is disposable build staging, so exclude tests from it only.
find "$GVISOR_DIR" -type f -name '*_test.go' -delete

gofmt -w \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/bbr.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/protocol.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/snd.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/endpoint_state.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/endpoint.go"

cp "$ROOT/go.mod" "$ROOT/go.local.mod"
rm -f "$ROOT/go.local.sum"
go mod edit -modfile="$ROOT/go.local.mod" -replace="gvisor.dev/gvisor=$GVISOR_DIR"

LDFLAGS="-s -w -X main.buildVersion=$TCP_SHIFT_VERSION -X main.gvisorRef=$GVISOR_REF -X main.gvisorRevision=$GVISOR_SHA"
go build -mod=mod -modfile="$ROOT/go.local.mod" -trimpath -ldflags="$LDFLAGS" -o "$ROOT/bin/tcp-shift" ./cmd/tcp-shift

cat > "$ROOT/bin/gvisor-build.txt" <<EOF
ref=$GVISOR_REF
sha=$GVISOR_SHA
remote=$GVISOR_REMOTE
EOF

printf 'built %s using gVisor ref=%s sha=%s\n' "$ROOT/bin/tcp-shift" "$GVISOR_REF" "$GVISOR_SHA"
