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
- Pacing is deadline/event driven: no periodic pacing poll loop, no per-flow timerfd/thread, no busy spin.
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

Pinned lwIP remains at `d08f4773edd0182b7910fc8f046eed82ffcd67c9`. A repository-owned patch is confined to `tcp.c`, `tcp_in.c`, and `tcp_out.c`.

P4 delegates ACK cwnd growth, fast-loss cwnd/ssthresh, and RTO cwnd/ssthresh. Unbound PCBs retain native lwIP policy. Retransmission execution, fast recovery, SACK/recovery, RTT/RTO calculation, queues, sequence space, packet construction, and output remain native lwIP mechanics.

One PCB ext-arg slot carries the project hook. Passive-open initialization explicitly mirrors pinned lwIP's initial-cwnd formula because upstream assigns that cwnd after invoking the accept callback.

### Integrated ownership evidence

P2 hard-gates every workload with:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

External TUN fault injection qualifies:

```text
fast-loss: cc_loss_events=1 cc_timeout_events=0 payload_bytes=262144 recovery=ok
RTO:       cc_loss_events=0 cc_timeout_events=2 payload_bytes=262144 recovery=ok
```

Final P4 behavior-head runs:

```text
upstream provenance  34815149550  success
P0                   34815149474  success
P1                   34815149646  success
P2                   34815149444  success
P3                   34815149825  success
P4                   34815149645  success
```

P3 rerun with the adapter retained:

```text
warm fixed process PSS: 335 KiB
fully-window-resident slope: 37.148438 KiB/flow
128-active projected process PSS: 5090 KiB
remaining 8-MiB process budget: 3102 KiB = 24.234 KiB/flow
```

## P5a: retransmission-safe delivery ledger — complete

P5a supplies the accounting substrate for later delivery-rate estimation without changing controller behavior or adding pacing.

Design:

- do not enlarge upstream `struct tcp_seg`;
- use project-owned lazy per-flow sidecar metadata keyed by `tcp_seg *`;
- start at 8 slots, grow as needed, hard-bounded by current `TCP_SND_QUEUELEN=90`;
- each slot is 32 bytes;
- record first successful transmit timestamp and delivery snapshots;
- retransmission reuses the slot and cannot double-count delivered payload;
- consume metadata before a fully acknowledged segment is freed;
- flow teardown must leave zero live slots.

The first workflow implementation exposed a CI bug: exact runtime data already reported `live_slots=0`, but a greedy parser matched `peak_live_slots`, and `check_delivery | tee` masked the checker failure. Qualification was withheld until both defects were fixed and hidden `.build` diagnostics were retained correctly.

Final P5a behavior head `ce6c89f399bed3535be52138e3c96ea2ea061b38` passed provenance/P0/P1/P2/P3/P4 and P5 run `34821205375`, job `103903019956`. Artifact `10338108552` retains:

```text
normal:    first_tx=184 retransmit=0 acked=184 delivered=262144 live_slots=0
fast-loss: first_tx=180 retransmit=1 acked=180 delivered=262144 live_slots=0
RTO:       first_tx=180 retransmit=4 acked=180 delivered=262144 live_slots=0
metadata_bytes_per_slot=32
```

All modes require zero allocation failures, metadata misses, abandoned slots, clock errors, and timestamp regressions.

P5a memory rerun:

```text
warm fixed process PSS: 343 KiB
fully-window-resident slope: 37.679688 KiB/flow
128-active projected process PSS: 5166 KiB
remaining 8-MiB process budget: 3026 KiB = 23.641 KiB/flow
```

The delivery ledger therefore consumes only a small fraction of the P4 admission headroom.

## P5b: ACK delivery-rate sampler + app-limited — next

P5b converts P5a timestamps/snapshots into a transport-neutral rate observation. It must define and qualify:

1. newly delivered bytes per ACK event;
2. delivery interval and send interval;
3. rate sample selection/validity rules;
4. delayed ACK and ACK aggregation behavior;
5. ACKs covering multiple segments;
6. retransmitted data without duplicate delivery accounting;
7. partial ACK handling;
8. sequence-number wrap safety;
9. latest valid RTT observation;
10. prior inflight/loss state;
11. app-limited marking and exit semantics.

The sampler belongs in the lwIP adapter/transport-observation layer. It may extend the pure-C controller input with transport-neutral sample fields, but it must not move Linux timer or lwIP queue mechanics into `src/cc/`.

P5b exits only when the sample can be reproduced and asserted under normal, delayed-ACK/aggregation, fast-loss, RTO, and app-limited workloads.

## P5c: event-driven pacer

P5c turns controller pacing policy into actual transmission timing without introducing periodic polling.

Target runtime shape:

```text
all paced flows
      |
      v
process-wide min-heap of next eligible send deadlines
      |
      v
one one-shot CLOCK_MONOTONIC timerfd
      |
      v
existing epoll owner
```

Rules:

- one timerfd for the process, not per flow;
- arm to the earliest pending deadline only;
- disarm when no pacing deadline exists;
- no fixed pacing tick;
- no busy spin;
- fd readiness and lwIP timers remain under the same owner;
- CI records timer expirations, pacing wakeups, deadline lateness, CPU, and idle wakeups.

The current loop is already event/deadline driven: epoll timeout comes from `sys_timeouts_sleeptime()` and TUN `EPOLLOUT` is enabled only for real backlog. P5c must preserve or improve that wakeup profile. Unifying lwIP deadlines and the pacer behind one one-shot timerfd is allowed only if behavior/wakeup CI proves the change.

## P6: tcp-shift BBR

After P5b and P5c are qualified, BBR becomes the active controller milestone.

P6 implements tcp-shift's bandwidth/min-RTT model, pacing/cwnd policy, mode transitions, probing, loss behavior, and app-limited treatment over the generic CC boundary.

Reference order: current IETF BBR specification, Google QUICHE, ns-3 `TcpBbr`, Picoquic, then Linux `tcp_bbr.c` / `tcp_rate.c` as TCP behavior cross-checks.

Validation compares cwnd, pacing rate, bandwidth estimate, min RTT, mode transitions, app-limited behavior, loss response, throughput, retransmissions, CPU, timer wakeups, and memory against native Linux reference runs under reproducible RTT/loss/bandwidth scenarios.

## Distance to BBR

The remaining path is now compact:

```text
P5a delivery ledger        complete
P5b rate/app-limited       next
P5c event-driven pacing    then
P6 tcp-shift BBR           then active
```

The difficult TCP ownership/recovery boundary has already been qualified. The largest remaining pre-BBR risk is sampler/pacer correctness under high BDP, ACK aggregation, and loss.

## Stop criteria

Stop the lwIP route rather than recreating half of Linux TCP if acceptable behavior requires replacing most lwIP recovery/SACK machinery, metadata/pacing memory approaches the hosted-Linux design, correctness requires a large long-lived lwIP TCP fork, unavoidable BDP buffering dominates the fixed-memory advantage, or pacing accuracy requires per-flow timers / periodic polling / busy spinning.

The project is still successful if evidence supports only a very small conventional-CC userspace TCP endpoint and rejects BBR.
