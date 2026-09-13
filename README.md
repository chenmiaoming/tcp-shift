# tcp-shift

`tcp-shift` is a low-memory userspace TCP endpoint experiment for constrained VPS/container environments.

## Current direction: lwIP

The previous gVisor-based implementation has been retired. The project is restarting around **lwIP** with a narrower goal: terminate the WAN-facing TCP connection in a small userspace TCP/IP stack, feed raw L3 packets through TUN, and bridge the accepted byte stream to an ordinary host-loopback backend.

The reason for the reset is memory cost and implementation ownership. The new path should keep the fixed runtime footprint small enough to be useful on 32/64/128-MiB systems while leaving application processes and the host kernel unchanged.

```text
remote client
    |
    | public IPv4 packets
    v
host netfilter / routing
    |
    v
TUN (L3)
    |
    v
lwIP netif
    |
    v
lwIP TCP listener / PCB
    |
    | raw TCP callbacks
    v
single-owner userspace bridge
    |
    v
127.0.0.1 backend
```

The initial implementation deliberately uses lwIP's `NO_SYS=1` raw API: no lwIP socket layer, no netconn layer, no TCP/IP worker thread, no Ethernet/TAP requirement, and IPv4 first. The Linux process event loop will own TUN readiness, backend sockets, lwIP timers, and later pacing.

## Congestion-control plan

The first milestone is a correct lwIP TCP endpoint and bridge, not BBR. Once that path is stable, congestion control will be introduced behind an explicit internal interface:

1. establish a high-resolution monotonic clock and per-segment delivery accounting;
2. implement ACK/delivery-rate sampling and app-limited detection;
3. add a global userspace pacer;
4. validate a conventional controller such as CUBIC;
5. add an experimental BBR implementation and compare it against native Linux baselines.

An lwIP BBR implementation will not be described as upstream Linux BBR unless its transport semantics are actually equivalent. Linux `tcp_bbr.c`, Google QUICHE, the IETF BBR specification, ns-3 TCP BBR, and Picoquic are reference material rather than code to copy blindly.

## Upstream baseline

lwIP is fetched rather than vendored. `.lwip-baseline` pins an exact upstream commit so experiments are reproducible. Run:

```bash
make build
```

The current P0 target only proves that the pinned lwIP core builds and initializes in a Linux userspace process. TUN integration begins in P1.

See [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) for the implementation boundaries and stop criteria.

> Status: architecture reset / P0 bring-up. Do not use on production traffic.
