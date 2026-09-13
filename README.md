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

The runtime uses lwIP `NO_SYS=1`: no lwIP socket layer, no netconn layer, no TCP/IP worker thread, and no TAP/Ethernet requirement. IPv4 is qualified first; IPv6-only operation is a product requirement and is the active next milestone rather than a late optional feature.

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

P0 remains the unprivileged reproducible initialization artifact. P1a IPv4 is now runner-qualified end to end on GitHub Actions: a real nonpersistent L3 TUN carries ICMP and TCP through lwIP; epoll is driven by lwIP timer deadlines without a fixed polling tick; TUN write backpressure is bounded to 64 packets / 96 KiB with FIFO ordering; oversized RX packets are nonfatal drops; MTU 1500/1501 and ICMP checksum behavior are qualified; and namespace traffic reaches lwIP through DNAT/conntrack.

P1a also owns its IPv4 ingress resource rather than relying on the test harness. `src/host/nft_ingress.*` performs a read-only `nft -c` validation, then atomically creates one exclusive `ip tcp_shift_p1` table containing an exact public-address + TCP-port DNAT rule. Existing/stale table collisions are rejected rather than adopted. `net.ipv4.ip_forward` and broad host forwarding policy remain operator-managed prerequisites; tcp-shift diagnoses but does not rewrite them. On SIGTERM or startup rollback, tcp-shift deletes only its owned table before closing the nonpersistent TUN.

The retained product-owned lifecycle evidence is `lwIP P1 IPv4 TUN` run `34763055040`: forwarding-disabled preflight passed without sysctl mutation, exclusive table-collision rejection passed, a namespace client connected through product-owned DNAT at `198.51.101.1:18081`, signal cleanup removed the TUN/table, and an unrelated nftables table was unchanged.

This is GitHub-runner qualification, not yet provider/OpenVZ qualification. The active milestone is P1b IPv6: extend the same L3/runtime/lifecycle design to IPv6-only operation, ICMPv6/TCP, exact IPv6 ingress, and Packet Too Big/PMTU qualification before P2 backend-bridge work begins.

Start here for project state:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — current product architecture and ownership;
- [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) — milestone order and stop criteria;
- [`docs/ci.md`](docs/ci.md) — qualification model and retained evidence;
- [`docs/development.md`](docs/development.md) — development and agent handoff contract;
- [`docs/milestones/p1-l3-tun.md`](docs/milestones/p1-l3-tun.md) — active milestone state and evidence.

> Status: P1a IPv4 runner-qualified; P1b IPv6 in progress. Do not use on production traffic.
