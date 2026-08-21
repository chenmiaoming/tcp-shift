# tcp-shift

`tcp-shift` is an experimental userspace TCP relay for running and evaluating congestion-control algorithms without modifying the host kernel. The first backend is gVisor netstack, chosen as a mature TCP implementation against which we can measure both performance and memory cost before considering a smaller stack.

The immediate target is restricted VPS/container environments (including OpenVZ-style deployments) where loading kernel modules, eBPF TCP `struct_ops`, or changing the host kernel is not available.

> Early research prototype. Do not put production traffic through it yet.

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

The first BBR implementation is a compact, original BBRv1-inspired model. It does **not** copy Linux BBR source code and it is not yet a bit-for-bit Linux-compatible implementation.

It currently provides:

- minimum-RTT tracking;
- a sliding maximum bandwidth estimate;
- STARTUP, DRAIN, PROBE_BW, and PROBE_RTT modes;
- model-driven congestion window sizing;
- a generic sender pacer exposed through a small `PacingRate()` interface;
- gVisor's existing RACK/SACK/retransmission machinery for loss recovery.

The initial bandwidth sampler uses cumulatively ACKed data over ACK arrival intervals. A later milestone is a Linux-style per-packet delivery-rate sampler with app-limited detection; CI results from this first version should therefore be treated as measurements of the prototype, not a compatibility claim.

## Build

The project pins gVisor to:

```text
80336ad549d71d82c7f77a79cf91d628fa34135a
```

`bash ./scripts/build.sh` checks out exactly that revision under `.deps/gvisor`, applies the small tcp-shift TCP patch, and builds the relay. The gVisor source tree is not vendored into this repository.

```bash
bash ./scripts/build.sh
```

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

## Reproducible long-fat-network CI

The GitHub Actions benchmark creates a network namespace and shapes both directions with `tc netem`:

```text
client namespace                         root namespace
10.99.0.3
    |
    | 50 Mbit/s, 50 ms, 0.10% loss each direction
    |
    +---------------------------> 10.99.0.1  native relay
    |
    +---- route via host ------> TUN 10.99.0.2
                                      |
                                      v
                                gVisor TCP relay
                                      |
                                      v
                               iperf3 backend
                               127.0.0.1:5202
```

`iperf3 -R` is intentional: the server/backend sends the payload, therefore the WAN-facing frontend socket is the sender under test. A normal forward `iperf3` run would mostly exercise the client's congestion control instead.

Each CI run measures:

- native-kernel CUBIC through the same Go relay;
- gVisor CUBIC;
- gVisor experimental BBR;
- throughput;
- process peak RSS (`VmHWM`);
- user+system CPU time during the transfer.

The benchmark reports BBR-vs-gVisor-CUBIC improvement but does not fail CI merely because one noisy run is slower. Once the implementation stabilizes we can add statistical regression thresholds.

Run it locally on a Linux host with network-namespace/TUN privileges:

```bash
make build
sudo -E make bench
```

Results are written under `.bench/`.

## Development roadmap

1. Establish native-vs-gVisor memory/CPU/throughput baseline.
2. Validate the BBR state machine and pacer under deterministic high-BDP conditions.
3. Replace ACK-interval bandwidth sampling with per-segment delivery-rate sampling and app-limited detection.
4. Add queue occupancy / RTT inflation / loss-recovery metrics and repeated statistical trials.
5. Test under a strict memory cgroup representative of a 128 MiB, no-swap VPS.
6. Only after the gVisor baseline is understood, evaluate a smaller lwIP/smoltcp backend.
