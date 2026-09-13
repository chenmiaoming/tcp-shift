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

The public TCP connection and backend TCP connection are distinct. Congestion control for the public connection belongs to lwIP/tcp-shift; the loopback backend remains an ordinary host Linux socket. Public IPv6 therefore does not require an IPv6-capable application backend: the same bridge accepts IPv4 or IPv6 public streams and connects them to `127.0.0.1`.

The runtime uses lwIP `NO_SYS=1`: no lwIP socket layer, no netconn layer, no TCP/IP worker thread, and no TAP/Ethernet requirement. The same L3 adapter and event-loop owner qualify both IPv4 and IPv6.

## Module direction

Source boundaries are deliberately separated even while the initial product stays a single process. Linux TUN/netfilter/lifecycle code is distinct from lwIP integration, the stream bridge, and the future congestion-control core.

The congestion-control core is intended to become an independently buildable pure-C static library. That library may still be linked into the same process. It must not depend on TUN, epoll, timerfd, nftables, host socket descriptors, or tcp-shift lifecycle code so a later embedded lwIP port can reuse it if the interface proves stable.

A separate privileged helper process is a possible later security boundary, not a prerequisite for portability.

## Congestion-control plan

The packet path and stream bridge are now runner-qualified. Before modifying congestion control, the next milestone establishes the actual memory/capacity envelope for 32/64/128-MiB targets. After that:

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

P0 remains the unprivileged reproducible initialization artifact. P1 is runner-qualified for both public address families on GitHub Actions.

P1a IPv4 proves a real nonpersistent L3 TUN carrying ICMP and TCP through lwIP; epoll driven by lwIP timer deadlines without a fixed polling tick; TUN write backpressure bounded to 64 packets / 96 KiB with FIFO ordering; nonfatal oversized RX drops; MTU 1500/1501 and ICMP checksum behavior; namespace DNAT/conntrack; and product-owned exact IPv4 nftables ingress with prerequisite, collision, rollback, signal-cleanup, and unrelated-ruleset preservation gates.

P1b extends the same runtime to IPv6 without enabling Ethernet, SLAAC, router solicitation, DHCPv6, MLD, ND6 packet queueing, or IPv6 fragmentation/reassembly. CI proves direct ICMPv6/TCP, 1500/1501 MTU behavior, product-owned exact `ip6` DNAT/conntrack, Hop-by-Hop extension-header-safe TCP matching, IPv6 forwarding prerequisite handling, cleanup, and routed ICMPv6 Packet Too Big learning. The retained PMTU evidence from P1 run `34767386662` is:

```text
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
P1b routed IPv6 Packet Too Big/PMTU qualification passed
```

P2 now replaces the probe-only listener with a real public-stream-to-loopback bridge. IPv4 and IPv6 public flows share one bridge state machine and both connect to an ordinary nonblocking `127.0.0.1` backend socket. The bridge keeps public-to-backend bytes in lwIP pbufs until the backend accepts them and consumes backend bytes only after `tcp_write()` accepts them into lwIP, so backpressure is tied to transport windows rather than unbounded userspace buffers.

Behavior head `601a49648610513d98173e3e3add722326591ffc` passed P0 run `34769960299`, the full P1 regression run `34769960302`, and P2 run `34769960275`. P2 qualifies 128-KiB IPv4 and IPv6 bidirectional integrity, a 1-MiB blocked-peer gate, backend-first half-close without RDHUP spin, backend refusal recovery, public/backend reset recovery, eight simultaneous flows, explicit cleanup with an active flow, and 64 sequential reuse flows.

Retained P2 memory/backpressure observations include:

```text
bridge_peak_pending_public_bytes=32768
bridge_backend_socket_sndbuf_bytes=32768
bridge_backend_socket_rcvbuf_bytes=32768
rss_warmup_kb=1800
rss_mid_kb=1800
rss_final_kb=1800
bridge_reuse_no_ratcheting=ok
```

Those RSS values are only a P2 lifecycle/no-ratcheting gate, not the product capacity baseline. P3 is now the active milestone: measure idle RSS/PSS/private dirty, incremental established-flow memory, controlled active-flow residency, repeated load/drain floors, connection-count capacity, and CPU for 32/64/128-MiB targets.

This remains GitHub-runner qualification, not yet provider/OpenVZ qualification.

Start here for project state:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — current product architecture and ownership;
- [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) — milestone order and stop criteria;
- [`docs/ci.md`](docs/ci.md) — qualification model and retained evidence;
- [`docs/development.md`](docs/development.md) — development and agent handoff contract;
- [`docs/milestones/p1-l3-tun.md`](docs/milestones/p1-l3-tun.md) — completed P1 packet-path state and evidence;
- [`docs/milestones/p2-bridge.md`](docs/milestones/p2-bridge.md) — completed P2 bridge state and evidence.

> Status: P0/P1/P2 runner-qualified; P3 memory/capacity baseline next. Do not use on production traffic.
