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

# tcp-shift consumes gVisor's synthetic `go` branch, which is the upstream-
# supported (best-effort) projection for external Go/Netstack users. It contains
# generated Go/assembly sources that are only templates in the authoritative
# Bazel `master` tree. GVISOR_REF may be the moving `go` branch or an exact
# commit from that branch; always resolve it to a concrete SHA for provenance.
git -C "$GVISOR_DIR" fetch -q --depth=1 origin "$GVISOR_REF"
GVISOR_SHA=$(git -C "$GVISOR_DIR" rev-parse FETCH_HEAD)
git -C "$GVISOR_DIR" checkout -q --detach "$GVISOR_SHA"
git -C "$GVISOR_DIR" reset -q --hard "$GVISOR_SHA"
git -C "$GVISOR_DIR" clean -q -fd

# Fail with a useful diagnostic if somebody points GVISOR_REF at the Bazel
# master tree instead of a synthetic-Go revision. Building raw master with the
# standard Go toolchain would otherwise fail later on unrendered *.tmpl.* files.
if find "$GVISOR_DIR/pkg" -type f -name '*.tmpl.*' -print -quit | grep -q .; then
  echo "gVisor ref $GVISOR_REF contains Bazel template sources." >&2
  echo "Use the synthetic 'go' branch (GVISOR_REF=go) or a commit from it." >&2
  exit 1
fi

python3 "$ROOT/scripts/patch_gvisor.py" "$GVISOR_DIR"
python3 "$ROOT/scripts/patch_recovery_pacing.py" "$GVISOR_DIR"

gofmt -w \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/bbr.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/rate.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/pacing_recovery.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/protocol.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/segment.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/snd.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/rack.go" \
  "$GVISOR_DIR/pkg/tcpip/transport/tcp/sack_recovery.go" \
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
source_branch=go
EOF

printf 'built %s using gVisor synthetic-go ref=%s sha=%s\n' "$ROOT/bin/tcp-shift" "$GVISOR_REF" "$GVISOR_SHA"
