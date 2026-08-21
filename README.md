# tcp-shift

`tcp-shift` is an experimental userspace TCP relay for running and evaluating congestion-control algorithms without modifying the host kernel. The first backend is gVisor netstack, chosen as a mature TCP implementation against which we can measure performance, protocol behavior, CPU cost, and memory cost before deciding whether a smaller stack is necessary.

The immediate target is restricted VPS/container environments, including OpenVZ-style deployments, where loading kernel modules, eBPF TCP `struct_ops`, or changing the host kernel is unavailable.

> Early research prototype. Do not put production traffic through it yet.

## Project goal

The long-term target is a production-grade relay with three properties:

1. the WAN-facing TCP endpoint is fully userspace and can run selectable congestion-control algorithms;
2. the runtime remains usable in a constrained 128 MiB, no-swap VPS;
3. tcp-shift continuously follows gVisor upstream instead of freezing on one historical netstack snapshot.

Reproducibility and upstream tracking are handled separately: releases use a verified exact gVisor SHA, while CI continuously builds against `google/gvisor@master`. See [docs/gvisor-upstream.md](docs/gvisor-upstream.md).

## Current architecture

```text
remote TCP client
      |
      | packets routed to TUN
      v
+-------------------------------+
| tcp-shift                     |
|                               |
| gVisor netstack TCP endpoint  |
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

The WAN-facing connection is terminated by gVisor, so the selected netstack congestion-control algorithm actually controls that TCP sender. The backend leg deliberately remains a normal host socket.

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

The verified baseline is recorded in:

```text
.gvisor-baseline
```

Normal builds use that SHA:

```bash
bash ./scripts/build.sh
./bin/tcp-shift --version
```

To test current gVisor upstream instead:

```bash
GVISOR_REF=master bash ./scripts/build.sh
./bin/tcp-shift --version
```

Any commit or tag can be tested the same way:

```bash
GVISOR_REF=<commit-or-tag> bash ./scripts/build.sh
```

The build always resolves the requested ref to an exact commit, embeds that SHA in the binary, and writes provenance to `bin/gvisor-build.txt`.

The gVisor source tree is not vendored into this repository. It is fetched into `.deps/gvisor`, patched in disposable build staging, and used through a local Go-module replacement.

## Run

Example netstack frontend:

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

For a same-process lower-bound without gVisor TCP termination:

```bash
./bin/tcp-shift \
  --engine native \
  --listen 0.0.0.0:443 \
  --backend 127.0.0.1:8443
```

## Long-fat-network CI

GitHub Actions uses Linux network namespaces, a TUN device, and `tc netem` to construct a reproducible high-BDP path. The benchmark uses `iperf3 -R` intentionally: the backend/server sends the payload, so the WAN-facing tcp-shift endpoint is the sender whose congestion control is under test.

The baseline benchmark currently compares:

- native-kernel CUBIC through the same Go relay;
- gVisor CUBIC;
- gVisor experimental BBR.

For each case CI records:

- throughput;
- process peak RSS (`VmHWM`);
- user+system CPU time;
- raw client/server results and relay logs.

The current CI path is approximately 100 Mbit/s with 200 ms RTT and 0.10% bidirectional loss, giving a BDP large enough that congestion-control behavior and buffer sizing matter.

Run it locally on a Linux host with network-namespace/TUN privileges:

```bash
make build
sudo -E make bench
```

Results are written under `.bench/`.

CI also has a separate compatibility matrix that builds both the verified baseline and current `google/gvisor@master`. A floating-upstream failure does not suppress baseline benchmark results.

## Upstream-maintenance rule

The current PoC still patches gVisor's TCP sender to add a generic pacer. That patch is intentionally treated as transitional technical debt. The production design should minimize the gVisor delta to narrow, generic hooks that are easy to rebase and ideally suitable for upstream submission.

In particular, replacing large upstream functions is not an acceptable long-term maintenance strategy. The desired end state is approximately:

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

1. Keep baseline and upstream-master builds green in CI.
2. Establish native-vs-gVisor memory/CPU/throughput baselines.
3. Validate the BBR state machine and pacer under deterministic high-BDP conditions.
4. Replace ACK-interval bandwidth sampling with per-segment delivery-rate sampling and app-limited detection.
5. Reduce the current large `sendData()` patch to narrow generic sender/pacing hooks.
6. Add queue occupancy, RTT inflation, loss-recovery, fairness, and repeated statistical trials.
7. Add a strict 128 MiB/no-swap cgroup test and define an explicit memory regression budget.
8. Add protocol correctness, soak, reconnect, half-close, IPv6, PMTU, SACK/RACK, and failure-injection coverage.
9. Promote newer gVisor SHAs only after compatibility, correctness, memory, CPU, and throughput gates pass.
10. Only after the mature gVisor baseline is understood, decide whether a smaller lwIP/smoltcp backend is justified.
