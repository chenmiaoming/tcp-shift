# tcp-shift

`tcp-shift` is an experimental userspace TCP relay for running and evaluating congestion-control algorithms without modifying the host kernel. The first backend is gVisor Netstack, chosen as a mature TCP implementation against which we can measure protocol behavior, throughput, CPU cost, and memory cost before deciding whether a smaller stack is necessary.

The immediate target is restricted VPS/container environments, including OpenVZ-style deployments, where loading kernel modules, eBPF TCP `struct_ops`, or changing the host kernel is unavailable.

> Early research prototype. Do not put production traffic through it yet.

## Project goal

The long-term target is a production-grade relay with three properties:

1. the WAN-facing TCP endpoint is fully userspace and can run selectable congestion-control algorithms;
2. protocol correctness, loss recovery, pacing, stability, observability, and performance are validated against mature native Linux TCP baselines;
3. tcp-shift continuously follows gVisor upstream instead of freezing on one historical Netstack snapshot.

Memory footprint remains an observed regression metric, but there is no artificial 128 MiB/no-swap gate in the main development profile. A separately tuned constrained-device build/profile may be added later without compromising the production-grade baseline.

For external Go users gVisor maintains a protected synthetic `go` branch containing generated sources compatible with standard Go tooling. tcp-shift tracks that branch rather than consuming the raw Bazel `master` tree. Releases use a verified exact synthetic-Go SHA, while CI continuously builds the moving `google/gvisor@go` branch. See [docs/gvisor-upstream.md](docs/gvisor-upstream.md).

## Current architecture

```text
remote TCP client
      |
      | packets routed to TUN
      v
+-------------------------------+
| tcp-shift                     |
|                               |
| gVisor Netstack TCP endpoint  |
|   + Reno / CUBIC              |
|   + experimental BBR          |
|   + tcp-shift generic pacer   |
|             |                 |
|             v                 |
|        byte-stream relay      |
+-------------+-----------------+
              |
              | ordinary host TCP socket
              v
        localhost/backend
```

The WAN-facing connection is terminated by gVisor, so the selected Netstack congestion-control algorithm actually controls that TCP sender. The backend leg deliberately remains a normal host socket.

A native frontend mode is retained as a control. In native mode tcp-shift applies Linux `TCP_CONGESTION` per accepted WAN-facing socket, so `--cc cubic` and `--cc bbr` are real per-socket Linux baselines rather than aliases for the host's global congestion-control sysctl.

## Experimental BBR

The first BBR implementation is a compact, original BBRv1-inspired model. It does **not** copy Linux BBR source code and is not yet a bit-for-bit Linux-compatible implementation.

It currently provides:

- minimum-RTT tracking;
- a sliding maximum bandwidth estimate;
- STARTUP, DRAIN, PROBE_BW, and PROBE_RTT modes;
- model-driven congestion window sizing;
- a generic sender pacer exposed through a small `PacingRate()` interface;
- gVisor's existing RACK/SACK/retransmission machinery for loss recovery.

The initial bandwidth sampler uses cumulatively ACKed data over ACK arrival intervals. A production-quality BBR implementation still needs a Linux-style per-segment delivery-rate sampler, app-limited detection, stronger pacing validation, and substantially more protocol testing.

## gVisor dependency model

The verified synthetic-Go baseline is recorded in:

```text
.gvisor-baseline
```

Normal builds use that exact SHA:

```bash
bash ./scripts/build.sh
./bin/tcp-shift --version
```

To test the latest gVisor external-Go projection instead:

```bash
GVISOR_REF=go bash ./scripts/build.sh
./bin/tcp-shift --version
```

A particular commit from the synthetic `go` branch can be tested the same way:

```bash
GVISOR_REF=<synthetic-go-commit> bash ./scripts/build.sh
```

The build always resolves the requested ref to an exact commit, embeds that SHA in the binary, and writes provenance to `bin/gvisor-build.txt`. It explicitly rejects raw Bazel revisions containing unrendered `*.tmpl.*` sources.

The gVisor source tree is not vendored into this repository. It is fetched into `.deps/gvisor`, patched in disposable build staging, and used through a local Go-module replacement.

## Run

Example Netstack frontend:

```bash
sudo ip tuntap add dev ts0 mode tun user "$USER"
sudo ip link set ts0 up
sudo ip route add 10.99.0.2/32 dev ts0

./bin/tcp-shift \
  --engine netstack \
  --tun ts0 \
  --listen 10.99.0.2:443 \
  --backend 127.0.0.1:8443 \
  --cc bbr \
  --tcp-buffer-mib 4
```

Native Linux CUBIC or BBR control mode:

```bash
./bin/tcp-shift \
  --engine native \
  --listen 0.0.0.0:443 \
  --backend 127.0.0.1:8443 \
  --cc bbr
```

Native mode fails at startup if the requested Linux congestion-control algorithm is not registered on the host.

## Long-fat-network CI

GitHub Actions uses Linux network namespaces, a TUN device, an intermediate router namespace, and `tc netem` to construct a reproducible high-BDP path. The benchmark uses `iperf3 -R` intentionally: the backend/server sends the payload, so the WAN-facing tcp-shift endpoint is the sender whose congestion control is under test.

The benchmark compares four sender configurations:

- native Linux CUBIC through the same Go relay;
- native Linux BBR through the same Go relay;
- gVisor CUBIC;
- gVisor experimental BBR.

Linux native cases use per-socket `TCP_CONGESTION`. CI explicitly verifies that the runner exposes Linux BBR before publishing a comparison.

The impairment topology is deliberately routed:

```text
native/gVisor sender
        |
        | sender-side fq
        v
  router namespace
   |            |
DATA netem   ACK netem
   |            |
   +---- client-+
```

Delay/rate/loss are applied on the intermediate router instead of replacing the sender's qdisc. This matters for native Linux BBR because its kernel pacing baseline should retain `fq`.

CI separates data loss from ACK loss and currently covers lossless, data-loss-only, ACK-loss-only, bidirectional RACK, and bidirectional legacy-recovery scenarios. For each case it records throughput, process peak RSS (`VmHWM`), user+system CPU time, qdisc drop counters, raw iperf3 results, and relay logs. gVisor retransmission/RTO/DSACK counters are also summarized; native Linux TCP counters will be added from `TCP_INFO` rather than displayed as misleading zeros.

There is no cgroup memory limit in the main benchmark. RSS is still measured continuously as a production regression signal.

Run it locally on a Linux host with network-namespace/TUN privileges and native BBR available:

```bash
make build
sudo -E make bench
```

Results are written under `.bench/`.

CI separately builds both the verified synthetic-Go baseline and the moving `google/gvisor@go` branch. A moving-upstream failure does not suppress baseline benchmark results.

## Upstream-maintenance rule

The original PoC replaced gVisor's `sender.sendData()` wholesale to add pacing. The current patcher preserves the upstream function and inserts only three pacing operations—initialization, send admission, and post-send accounting—plus narrow timer/state hooks.

This is a substantial improvement for rebasing, but it still patches private TCP internals. The production design should minimize the gVisor delta to generic hooks that are easy to rebase and ideally suitable for upstream submission:

```text
gVisor TCP sender
      |
      +-- generic congestion-control hook
      +-- generic pacing hook
      |
      +-- Reno/CUBIC upstream
      +-- tcp-shift BBR extension
```

## Development roadmap

1. Keep the verified baseline and moving synthetic-`go` builds green in CI.
2. Establish native Linux CUBIC/BBR vs gVisor CUBIC/BBR throughput, CPU, memory, and recovery baselines.
3. Validate the BBR state machine and pacer under deterministic high-BDP conditions with data loss and ACK loss isolated.
4. Add native Linux `TCP_INFO` sampling so retransmission, RTT, cwnd, delivery-rate, and recovery comparisons use equivalent observability on both stacks.
5. Replace ACK-interval bandwidth sampling with per-segment delivery-rate sampling and app-limited detection.
6. Move the remaining private-source patch toward narrow, generic, upstreamable sender/pacing hooks.
7. Add queue occupancy, RTT inflation, loss-recovery, fairness, multi-flow coexistence, and repeated statistical trials.
8. Add protocol correctness, soak, reconnect, half-close, IPv6, PMTU, SACK/RACK, and failure-injection coverage.
9. Promote newer synthetic-Go SHAs only after compatibility, correctness, stability, CPU, memory, and throughput gates pass.
10. After the mature gVisor implementation is established, optionally add a separately tuned constrained-device profile/backend if there is a concrete deployment need.
