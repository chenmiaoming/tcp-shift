# lwIP route: architecture and milestones

This roadmap defines implementation order and exit evidence. `ARCHITECTURE.md` is the current product source of truth.

## Product hypothesis

A constrained VPS can afford a small userspace TCP endpoint when it cannot control the host kernel's congestion-control policy, but it cannot comfortably afford a hosted Linux kernel or a large general-purpose userspace network stack. Fixed memory cost is the first optimization target.

```text
public IPv4/IPv6 packet -> host routing/netfilter -> L3 TUN -> lwIP TCP
                                                        -> raw TCP callbacks
                                                        -> single-owner bridge
                                                        -> 127.0.0.1 backend
```

The public and backend TCP legs are distinct. Congestion control belongs to the lwIP public leg. The public side is dual-stack/IPv6-only capable; the backend remains IPv4 loopback.

## Hard boundaries

- L3 TUN, not TAP/Ethernet.
- One mutable owner for lwIP; no per-flow forwarding threads.
- `NO_SYS=1`; no lwIP socket/netconn/tcpip-thread path.
- IPv6-only public operation is required.
- Linux host integration, lwIP transport integration, bridge logic, and congestion-control policy remain separate modules.
- `src/cc/` stays pure C and independently buildable.
- Linux timerfd/epoll pacing stays outside `src/cc/`.
- Do not call an experimental controller Linux BBR unless transport semantics actually match.

## P0: reproducible lwIP userspace build — complete

Exact upstream pinning, explicit dual-stack TCP `NO_SYS=1` source allowlist, configuration/source/binary/RSS gates, and clean-runner artifact smoke are qualified.

## P1: dual-stack L3 TUN and ingress lifecycle — complete

P1 qualifies one nonpersistent `IFF_TUN | IFF_NO_PI` L3 adapter, lwIP timer-driven epoll ownership, bounded whole-packet TX retry, nonfatal RX drops, direct IPv4/IPv6 ICMP/TCP, product-owned exact IPv4/IPv6 DNAT/conntrack lifecycle, forwarding prerequisite checks, collision/rollback/cleanup, extension-header-safe IPv6 TCP matching, and routed ICMPv6 PTB learning.

Representative retained PMTU evidence:

```text
ipv6_ptb_mtu=1280 baseline_mss=1440 learned_mss=1220 pmtu_adaptation=ok
```

Provider/OpenVZ capability qualification remains separate.

## P2: dual-stack TCP listener and backend bridge — complete

P2 replaces the probe listener with a real bridge. Public IPv4/IPv6 use the same flow state machine and connect to an ordinary nonblocking `127.0.0.1:<backend-port>` socket.

Public->backend data remains in lwIP pbufs until backend `writev()` commits it. Backend->public removes host bytes only after `tcp_write(..., COPY)` accepts them. The bridge therefore adds no fixed bidirectional application buffer and preserves bounded transport backpressure.

P2 qualifies IPv4/IPv6 stream integrity, real bidirectional blocking, half-close, backend refusal recovery, public/backend reset recovery, concurrent flows, active-flow shutdown, and long-lived reuse without RSS ratcheting.

## P3: memory/capacity baseline — complete

P3 separates tcp-shift process residency from backend Linux TCP/kernel state and measures idle flows, both active-window directions, repeated load/drain floors, and representative CPU.

Original admission baseline:

```text
warm fixed process PSS: 315 KiB
fully-window-resident slope: 37.523438 KiB/flow
32-MiB host / 25% tcp-shift process budget: 8192 KiB
128-active projected PSS: 5118 KiB
remaining process budget: 3074 KiB ~= 24.0 KiB/flow
```

This is a process-PSS planning model, not a full-host capacity guarantee. Backend kernel/application memory and provider overhead remain outside it.

## P4: generic CC boundary + lwIP adapter — complete

P4 is fully runner-qualified on behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac`.

### Pure-C controller boundary

`src/cc/` is independently buildable with caller-owned state, an allowlisted ISO-C include surface, `-ffreestanding -fno-builtin`, and zero unresolved archive symbols.

The generic controller consumes MSS, inflight, peer send window, and a transport cwnd limit. Events are init/ACK/loss/RTO. Policy publishes cwnd, ssthresh, and optional pacing rate. The conventional Reno baseline uses 16 bytes and requests no pacing.

### Narrow lwIP integration

Pinned lwIP remains at `d08f4773edd0182b7910fc8f046eed82ffcd67c9`. A repository-owned patch modifies exactly three congestion-policy sites:

1. ACK cwnd growth;
2. fast-loss cwnd/ssthresh policy;
3. RTO cwnd/ssthresh policy.

Unbound PCBs retain native lwIP policy. Retransmission execution, fast recovery, SACK/recovery, RTT/RTO calculation, queues, sequence space, packet construction, and output remain native lwIP mechanics.

One PCB ext-arg slot carries the project hook. Passive-open initialization explicitly mirrors pinned lwIP's initial-cwnd formula because upstream assigns that cwnd after invoking the accept callback.

### Integrated ownership evidence

P2 now hard-gates every workload with:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

Real external fault injection on the TUN path qualifies fast-loss recovery:

```text
cc_loss_events=1
cc_timeout_events=0
mode=fast-loss payload_bytes=262144 recovery=ok
```

and RTO recovery:

```text
cc_loss_events=0
cc_timeout_events=2
mode=rto payload_bytes=262144 recovery=ok
```

Both preserve exact stream integrity and do not call generic loss/RTO handlers directly.

### Provenance and regression evidence

Final behavior-head runs:

```text
upstream provenance  34815149550  success
P0                   34815149474  success
P1                   34815149646  success
P2                   34815149444  success
P3                   34815149825  success
P4                   34815149645  success
```

The provenance gate proves exact pin, pristine pre-patch hashes, recorded patch SHA, exactly three modified upstream files, no untracked dependency files, reverse-apply correctness, and byte identity against an independently patched worktree.

P3 rerun with the adapter retained:

```text
warm fixed process PSS: 335 KiB
fully-window-resident slope: 37.148438 KiB/flow
128-active projected process PSS: 5090 KiB
remaining 8-MiB process budget: 3102 KiB = 24.234 KiB/flow
three-round 128-flow drain growth: 5 KiB
```

P4 therefore exits with the constrained-host admission headroom intact.

## P5: delivery-rate sampler and pacing prerequisites — active next

P5 must establish the transport observations needed by model-based congestion control before any BBR mode/state machine is added.

Required work:

1. high-resolution monotonic send/ACK timestamps;
2. cumulative delivered-byte accounting;
3. per-segment delivery snapshots/send timestamps;
4. ACK-derived delivery-rate samples;
5. app-limited detection and marking;
6. loss/inflight observations suitable for later controllers;
7. process-wide pacing scheduler, preferably one min-heap plus one timerfd;
8. fixed/per-flow/per-segment memory and CPU qualification against P3/P4.

The sampler should publish transport-neutral observations into the generic CC boundary. Runtime scheduling stays in `runtime/`, not `cc/`.

P5 is complete only when rate samples and app-limited semantics are behaviorally qualified under real traffic, pacing is externally observable and bounded, P0-P4 regressions remain green, and the added metadata/scheduler residency fits the constrained-host model.

## P6: experimental BBR

Only after P5 is qualified should an experimental BBR controller be added.

Reference order: current IETF BBR specification, Google QUICHE, ns-3 `TcpBbr`, Picoquic, then Linux `tcp_bbr.c`/`tcp_rate.c` as TCP behavior cross-checks.

Validation must compare cwnd, pacing rate, bandwidth estimate, min RTT, mode transitions, app-limited behavior, loss response, throughput, retransmissions, CPU, and memory against native Linux baselines under reproducible RTT/loss/bandwidth scenarios.

## Stop criteria

Stop the lwIP route rather than recreating half of Linux TCP if acceptable behavior requires replacing most lwIP recovery/SACK machinery, metadata/pacing memory approaches the hosted-Linux design, correctness requires a large long-lived lwIP TCP fork, or unavoidable BDP buffering dominates the fixed-memory advantage.

The project is still successful if evidence supports only a very small conventional-CC userspace TCP endpoint and rejects BBR.
