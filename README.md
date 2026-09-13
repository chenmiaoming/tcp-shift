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

The public TCP connection and backend TCP connection are distinct. Congestion control for the public connection belongs to lwIP/tcp-shift; the loopback backend remains an ordinary host Linux socket. Public IPv6 therefore does not require an IPv6-capable application backend: P2 will initially connect accepted IPv4 or IPv6 public streams to `127.0.0.1`.

The runtime uses lwIP `NO_SYS=1`: no lwIP socket layer, no netconn layer, no TCP/IP worker thread, and no TAP/Ethernet requirement. The same L3 adapter and event-loop owner now qualify both IPv4 and IPv6.

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

P0 remains the unprivileged reproducible initialization artifact. P1 is now runner-qualified for both public address families on GitHub Actions.

P1a IPv4 proves a real nonpersistent L3 TUN carrying ICMP and TCP through lwIP; epoll driven by lwIP timer deadlines without a fixed polling tick; TUN write backpressure bounded to 64 packets / 96 KiB with FIFO ordering; nonfatal oversized RX drops; MTU 1500/1501 and ICMP checksum behavior; namespace DNAT/conntrack; and product-owned exact IPv4 nftables ingress with prerequisite, collision, rollback, signal-cleanup, and unrelated-ruleset preservation gates.

P1b extends the same runtime to IPv6 without enabling Ethernet, SLAAC, router solicitation, DHCPv6, MLD, ND6 packet queueing, or IPv6 fragmentation/reassembly. CI proves direct ICMPv6/TCP, 1500/1501 MTU behavior, product-owned exact `ip6` DNAT/conntrack, Hop-by-Hop extension-header-safe TCP matching, IPv6 forwarding prerequisite handling, cleanup, and routed ICMPv6 Packet Too Big learning. The retained PMTU evidence from P1 run `34767386662` is:

```text
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
P1b routed IPv6 Packet Too Big/PMTU qualification passed
```

A pure L3 TUN bypasses the Ethernet ND path that normally creates lwIP IPv6 destination-cache entries. `src/lwip/l3_tun.c` therefore seeds/refreshes lwIP's existing fixed ND6 destination cache before IPv6 output; it does not allocate a second PMTU table or start neighbor discovery. Upstream `nd6_input()` still owns PTB updates and upstream TCP MSS calculation consumes the learned PMTU.

This is GitHub-runner qualification, not yet provider/OpenVZ qualification. The active milestone is now P2: replace the temporary probe listener with the real bounded public-stream-to-`127.0.0.1` backend bridge while preserving the P1 packet/lifecycle gates.

Start here for project state:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — current product architecture and ownership;
- [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) — milestone order and stop criteria;
- [`docs/ci.md`](docs/ci.md) — qualification model and retained evidence;
- [`docs/development.md`](docs/development.md) — development and agent handoff contract;
- [`docs/milestones/p1-l3-tun.md`](docs/milestones/p1-l3-tun.md) — completed P1 packet-path state and evidence.

> Status: P1 dual-stack packet path runner-qualified; P2 backend bridge next. Do not use on production traffic.
