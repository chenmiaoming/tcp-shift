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

Source boundaries are deliberately separated even while the initial product stays a single process. Linux TUN/netfilter/lifecycle code is distinct from lwIP integration, the stream bridge, and congestion-control policy.

P4 now contains an independently buildable pure-C `src/cc/` static library. That library may still be linked into the same process. It has no lwIP, Linux, TUN, epoll, timerfd, nftables, host-socket, bridge, or lifecycle dependency, and controller state is caller-owned. This keeps later embedded reuse possible if the interface proves stable.

A separate privileged helper process is a possible later security boundary, not a prerequisite for portability.

## Congestion-control plan

The packet path, stream bridge, pre-CC memory/capacity envelope, and standalone generic CC boundary are runner-qualified. The generic policy now publishes `cwnd`, `ssthresh`, and an optional pacing rate; the active P4 work is the smallest lwIP adapter/hook surface that delegates public-side policy without moving retransmission, recovery, queueing, or sequence-space mechanics out of lwIP. After that:

1. integrate and qualify the conventional controller through real public-side lwIP traffic;
2. establish high-resolution transport timestamps and per-segment delivery accounting;
3. implement ACK/delivery-rate sampling and app-limited detection;
4. add a runtime pacing scheduler;
5. validate the sampler/pacer/controller combination;
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

P2 replaces the probe-only listener with a real public-stream-to-loopback bridge. IPv4 and IPv6 public flows share one bridge state machine and both connect to an ordinary nonblocking `127.0.0.1` backend socket. The bridge keeps public-to-backend bytes in lwIP pbufs until the backend accepts them and consumes backend bytes only after `tcp_write()` accepts them into lwIP, so backpressure is tied to transport windows rather than unbounded userspace buffers.

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

P3 runner-qualifies the pre-CC userspace memory and CPU envelope. Behavior head `775ea5832f7e308e2c908c2f5abedfa4175c69be` passed P3 run `34805306193`. The final run measured 262 KiB ready PSS and 307 KiB at 128 idle flows. Under controlled window pressure, public-to-backend residency added 36.5 KiB/flow and backend-to-public added 37.125 KiB/flow. Three rounds of 128 flows returned fd count to 5 and showed only 5 KiB first-to-last drained PSS growth.

The conservative capacity model uses a 315-KiB warm fixed PSS and about 37.52 KiB/flow for a fully-window-resident flow. In its P4 admission scenario, tcp-shift gets only 25% of a 32-MiB host (8 MiB process-PSS budget): 128 active flows project to about 5.12 MiB, leaving about 3.07 MiB, or 24.0 KiB/flow, for future CC/sampler/pacer process structures. Backend kernel TCP memory, backend application memory, public-client kernel memory, and provider-specific overhead are deliberately excluded, so this is not a full-host capacity guarantee.

P3 also retained 0 runtime CPU ticks over a one-second idle sample and about 34.18 us of runtime CPU per operation for 2048 four-flow 64-byte request/echo operations on the GitHub runner. These are regression baselines, not provider performance claims.

P4 standalone behavior head `9e8cb2fa6091418ac4ed3dcb52f963337fdc25d0` passed dedicated run `34810033939`, job `103869414629`, while P0 run `34810033921` and full P1 run `34810033970` also stayed green. Retained evidence is:

```text
cc_contract=ok controller=reno state_bytes=16 pacing=none
cc_boundary=pure-c
external_symbols=0
```

Artifact `10334775895` retains the standalone build, include-surface and symbol diagnostics. The final contract also proves failed controller initialization leaves the generic handle invalid and that controller `ssthresh` is published explicitly beside `cwnd`. This qualifies only the generic pure-C boundary and conventional state-machine contract; public-side cwnd is still owned directly by native lwIP until the next P4 adapter increment is qualified.

This remains GitHub-runner qualification, not yet provider/OpenVZ qualification.

Start here for project state:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — current product architecture and ownership;
- [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) — milestone order and stop criteria;
- [`docs/ci.md`](docs/ci.md) — qualification model and retained evidence;
- [`docs/development.md`](docs/development.md) — development and agent handoff contract;
- [`docs/milestones/p1-l3-tun.md`](docs/milestones/p1-l3-tun.md) — completed P1 packet-path state and evidence;
- [`docs/milestones/p2-bridge.md`](docs/milestones/p2-bridge.md) — completed P2 bridge state and evidence;
- [`docs/milestones/p3-memory-capacity.md`](docs/milestones/p3-memory-capacity.md) — completed P3 memory/capacity state and evidence;
- [`docs/milestones/p4-cc-boundary.md`](docs/milestones/p4-cc-boundary.md) — active P4 CC boundary and lwIP-adapter work.

> Status: P0/P1/P2/P3 runner-qualified; P4 standalone CC boundary runner-qualified, lwIP adapter next. Do not use on production traffic.
