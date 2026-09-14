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

P4 provides an independently buildable pure-C `src/cc/` static library. It has no lwIP, Linux, TUN, epoll, timerfd, nftables, host-socket, bridge, or lifecycle dependency; controller state is caller-owned. A thin `src/lwip/cc_adapter.c` layer is allowed to depend on both lwIP and the generic controller and binds policy to established public TCP PCBs.

A separate privileged helper process is a possible later security boundary, not a prerequisite for portability.

## Congestion-control plan

The packet path, stream bridge, pre-CC memory/capacity envelope, generic CC boundary, and real lwIP controller integration are runner-qualified.

P4 now proves that public-side ACK, fast-loss, and retransmission-timeout policy decisions reach the generic controller while lwIP retains retransmission, fast-recovery mechanics, SACK/recovery, packet queues, sequence space, RTT/RTO calculation, and packet output. The generic policy publishes `cwnd`, `ssthresh`, and an optional pacing rate; the conventional Reno baseline requests no pacing.

Next work is P5:

1. add high-resolution send/ACK timing and delivered-byte accounting;
2. retain per-segment delivery metadata sufficient for ACK rate samples;
3. implement delivery-rate sampling and app-limited detection;
4. add a process-wide pacing scheduler outside `src/cc/`;
5. measure fixed/per-flow/per-segment memory and CPU increments against P3/P4;
6. only then add an experimental BBR controller and compare it against native Linux baselines.

An lwIP controller will not be described as Linux BBR unless the relevant transport semantics are actually equivalent.

## Build and status

lwIP is fetched rather than vendored. `.lwip-baseline` pins exact upstream commit `d08f4773edd0182b7910fc8f046eed82ffcd67c9`.

```bash
make build
```

The project now carries one explicit repository-owned lwIP integration patch, `patches/lwip-p4-cc-hooks.patch`. Fetch/provenance CI records pristine critical-source hashes before applying it, verifies the patch SHA, requires exactly three modified upstream files, and independently reapplies the patch to a fresh worktree to prove byte-identical results. The patch only inserts policy delegation at ACK, fast-loss, and RTO cwnd/ssthresh sites; unbound PCBs keep native lwIP policy.

P0 remains the unprivileged reproducible initialization artifact. P1 is runner-qualified for both public address families on GitHub Actions.

P1a IPv4 proves a real nonpersistent L3 TUN carrying ICMP and TCP through lwIP; epoll driven by lwIP timer deadlines without a fixed polling tick; bounded TUN TX backpressure; nonfatal oversized RX drops; MTU/checksum behavior; namespace DNAT/conntrack; and product-owned exact IPv4 nftables ingress lifecycle.

P1b extends the same runtime to IPv6 without enabling Ethernet, SLAAC, router solicitation, DHCPv6, MLD, ND6 packet queueing, or IPv6 fragmentation/reassembly. CI proves direct ICMPv6/TCP, exact `ip6` DNAT/conntrack, extension-header-safe TCP matching, IPv6 forwarding prerequisite handling, cleanup, and routed ICMPv6 Packet Too Big learning. Retained PMTU evidence includes:

```text
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
```

P2 replaces the probe listener with a real public-stream-to-loopback bridge. IPv4 and IPv6 public flows share one bridge state machine and both connect to an ordinary nonblocking `127.0.0.1` backend socket. The bridge keeps public-to-backend bytes in lwIP pbufs until the backend accepts them and consumes backend bytes only after `tcp_write()` accepts them into lwIP, so backpressure is tied to transport windows rather than unbounded userspace buffers.

P2 qualifies bidirectional integrity, real backpressure, backend-first half-close without RDHUP spin, backend refusal recovery, public/backend reset recovery, concurrent flows, explicit active-flow shutdown cleanup, and repeated reuse without RSS ratcheting.

P3 runner-qualifies the pre-CC userspace memory and CPU envelope. The original P3 admission baseline used 315 KiB warm fixed process PSS and 37.523438 KiB/flow for a fully-window-resident flow. In the conservative 32-MiB-host planning scenario, tcp-shift receives only 25% of host RAM as an 8-MiB process-PSS budget; backend kernel/application memory and provider overhead are intentionally outside the model.

P4 is now fully runner-qualified on behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac`:

- upstream provenance run `34815149550` passed the exact-pin + controlled-patch gate;
- P0 run `34815149474` passed;
- full P1 run `34815149646` passed;
- P2 run `34815149444` passed the complete bridge regression suite plus cross-workload controller ownership;
- P3 run `34815149825` passed with the adapter enabled;
- P4 run `34815149645` passed both the pure-C contract and integrated recovery jobs.

The P2 controller-ownership gate requires every workload to satisfy:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

P4's external fault-injection gate proves real fast-loss recovery:

```text
cc_bindings=1
cc_ack_events=108
cc_loss_events=1
cc_timeout_events=0
cc_controller_errors=0
mode=fast-loss payload_bytes=262144 recovery=ok
```

and real RTO recovery:

```text
cc_bindings=1
cc_ack_events=144
cc_loss_events=0
cc_timeout_events=2
cc_controller_errors=0
mode=rto payload_bytes=262144 recovery=ok
```

Both tests preserve exact 262144-byte bidirectional stream integrity. They do not call controller event functions directly; loss is injected outside lwIP on the real TUN path.

The adapter-enabled P3 rerun retained:

```text
warm fixed process PSS: 335 KiB
conservative idle PSS slope: 0.523438 KiB/flow
fully-window-resident process slope: 37.148438 KiB/flow
128 active projected process PSS: 5090 KiB
8-MiB process-budget remaining: 3102 KiB
remaining planning headroom: 24.234 KiB/active flow
three-round 128-flow drain growth: 5 KiB
```

The small fixed adapter cost does not consume the prior P3 admission headroom; active residency remains dominated by the 32-KiB lwIP window/pbuf/send-segment footprint. This remains process-PSS planning evidence, not a full-host capacity guarantee.

This remains GitHub-runner qualification, not yet provider/OpenVZ qualification.

Start here for project state:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — current product architecture and ownership;
- [`docs/lwip-roadmap.md`](docs/lwip-roadmap.md) — milestone order and stop criteria;
- [`docs/ci.md`](docs/ci.md) — qualification model and retained evidence;
- [`docs/development.md`](docs/development.md) — development and agent handoff contract;
- [`docs/milestones/p1-l3-tun.md`](docs/milestones/p1-l3-tun.md) — completed P1 packet-path state and evidence;
- [`docs/milestones/p2-bridge.md`](docs/milestones/p2-bridge.md) — completed P2 bridge state and evidence;
- [`docs/milestones/p3-memory-capacity.md`](docs/milestones/p3-memory-capacity.md) — completed P3 memory/capacity baseline;
- [`docs/milestones/p4-cc-boundary.md`](docs/milestones/p4-cc-boundary.md) — completed generic CC + lwIP adapter qualification.

> Status: P0/P1/P2/P3/P4 runner-qualified; P5 delivery-rate sampling/pacing prerequisites next. Do not use on production traffic.
