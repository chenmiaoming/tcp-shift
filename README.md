# tcp-shift

`tcp-shift` is a low-memory userspace TCP endpoint for constrained VPS/container environments where the tenant cannot select the outer kernel's congestion-control implementation.

## Current direction: lwIP

The active design terminates the WAN-facing TCP connection in lwIP, moves raw L3 packets through TUN, and bridges the accepted byte stream to an ordinary host-loopback backend.

```text
remote client
    |
    | public IPv4 or IPv6 TCP
    v
host netfilter / routing
    |
    v
TUN (L3)
    |
    v
lwIP TCP endpoint
    |
    | accepted byte stream
    v
single-owner userspace bridge
    |
    v
127.0.0.1 backend
```

The public TCP connection and backend TCP connection are distinct. Congestion control for the public connection belongs to lwIP/tcp-shift; the loopback backend remains an ordinary host Linux socket.

The runtime uses lwIP `NO_SYS=1`: no lwIP socket layer, no netconn layer, no TCP/IP worker thread, and no TAP/Ethernet requirement. IPv4 is the first packet-path bring-up, but IPv6-only operation is a product requirement and follows immediately after IPv4 rather than as a late optional feature.

## Module direction

Source boundaries are deliberately separated even while the initial product stays a single process. Linux TUN/netfilter/lifecycle code is distinct from lwIP integration, the stream bridge, and the future congestion-control core.

The congestion-control core is intended to become an independently buildable pure-C static library. That library may still be linked into the same process. It must not depend on TUN, epoll, timerfd, nftables, host socket descriptors, or tcp-shift lifecycle code so a later embedded lwIP port can reuse it if the interface proves stable.

A separate privileged helper process is a possible later security boundary, not a prerequisite for portability.

## Congestion-control plan

The first milestones are a correct dual-stack lwIP endpoint, bridge, shutdown behavior, and memory accounting. After that:

1. introduce a platform-independent CC interface;
2. establish high-resolution transport timestamps and per-segment delivery accounting;
3. implement ACK/delivery-rate sampling and app-limited detection;
4. add a runtime pacing scheduler;
5. validate a conventional controller;
6. add an experimental BBR implementation and compare it against native Linux baselines.

An lwIP controller will not be described as Linux BBR unless the relevant transport semantics are actually equivalent.

## Build and status

lwIP is fetched rather than vendored. `.lwip-baseline` pins an exact upstream commit.

```bash
make build
```

P0 remains the unprivileged reproducible initialization artifact. P1a now has retained behavioral evidence on GitHub Actions: a real nonpersistent L3 TUN carries IPv4 ICMP through lwIP (3/3 echo replies) and a host TCP `connect()` reaches an lwIP raw-API listener and triggers `tcp_accept`. The event loop is epoll-driven from lwIP timer deadlines, and TUN write backpressure is bounded by a whole-packet FIFO.

P1a is not complete yet: DNAT/public routing, transactional host lifecycle, explicit idle-wakeup measurement, and queue-pressure/failure-path qualification remain. P1b IPv6 follows before P2 bridge development.

Start here for project state:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — current product architecture and ownership;
- [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) — milestone order and stop criteria;
- [`docs/ci.md`](docs/ci.md) — qualification model and retained evidence;
- [`docs/development.md`](docs/development.md) — development and agent handoff contract;
- [`docs/milestones/p1-l3-tun.md`](docs/milestones/p1-l3-tun.md) — active milestone state and evidence.

> Status: P1a IPv4 L3 TUN bring-up. Do not use on production traffic.
