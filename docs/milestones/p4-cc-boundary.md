# P4: generic congestion-control boundary

Status: **runner-qualified; NewReno multiple-loss/burst recovery is merged through PR #37, and PR #41 adds a default-OFF sender-SACK transport experiment without changing the production recovery default**.

## Goal

Establish a platform-independent congestion-control policy boundary and prove that real public-side lwIP ACK, fast-loss, and RTO decisions can delegate through it without taking retransmission/recovery mechanics away from lwIP or consuming the constrained-host memory budget.

## Pure-C policy boundary

`src/cc/` is independently buildable pure C. It may use only its own headers plus ISO C integer/size/limits headers. It must not depend on lwIP, Linux/POSIX APIs, TUN, epoll, timerfd, nftables, bridge/runtime objects, or controller-owned heap allocation.

The generic transport observation contains MSS, inflight bytes, peer send window, and a transport-representable cwnd limit. Events are init, ACK, loss, and RTO. Policy output contains cwnd, ssthresh, and optional pacing rate.

The conventional byte-counting Reno baseline uses 16 bytes of caller-owned state and always requests zero pacing rate. `cwnd_limit_bytes` prevents its 32-bit state from silently diverging from the current unscaled 16-bit lwIP cwnd representation.

The standalone contract covers slow start, congestion avoidance, loss, timeout, MSS changes, cwnd limits, saturation, invalid arguments, failed-init handle safety, explicit cwnd/ssthresh publication, and no pacing request.

Final P4 behavior head `0a3054159db03b017526b3faabfbe7f6a6c826ac` passed the pure-C job in run `34815149645`; artifact `10335288981` retains the build/include/symbol evidence.

```text
cc_contract=ok controller=reno state_bytes=16 pacing=none
cc_boundary=pure-c
external_symbols=0
```

## lwIP adapter boundary

Pinned lwIP remains at:

```text
d08f4773edd0182b7910fc8f046eed82ffcd67c9
```

`scripts/fetch-lwip.sh` records pristine critical-source hashes and then applies `patches/lwip-p4-cc-hooks.patch` followed by `patches/lwip-sack-recovery.patch`.

The controlled patch chain remains confined to the same three TCP core files: `tcp_in.c`, `tcp_out.c`, and `tcp.c`. Original P4 policy delegation covers ACK cwnd growth, fast-retransmit loss cwnd/ssthresh policy, and RTO cwnd/ssthresh policy. PR #35 extends the sender ACK/recovery path with bounded NewReno-style partial-ACK handling. PR #41 adds inbound SACK parsing and a minimal sender scoreboard/selective requeue path. That second path is compile-time gated by `TCP_SHIFT_EXPERIMENTAL_SACK_RECOVERY` and remains OFF in all legacy/production qualification builds. PR #45 extends the hook boundary so newly SACKed delivery can be charged to the project delivery sidecar exactly once for internal-BBR sampling; Reno/CUBIC recovery policy remains native.

Unbound PCBs retain native pinned-lwIP behavior. For bound tcp-shift PCBs, the patched lwIP transport still owns duplicate-ACK processing, retransmission execution, `TF_INFR`, recovery-window mechanics, RTT/RTO calculation, queues, sequence space, packet construction, and `tcp_output()`. The congestion controller receives observations and may own only its published recovery cwnd when explicitly declared (internal BBR); sender recovery itself is not moved into `src/cc/`.

One PCB ext-arg slot stores a project hook. `src/lwip/cc_adapter.c` translates lwIP state/events into generic observations and applies returned cwnd/ssthresh policy. The adapter may depend on lwIP; `src/cc/` may not.

Pinned lwIP calls the passive accept callback before setting `LWIP_TCP_CALC_INITIAL_CWND(pcb->mss)`. The adapter therefore initializes the controller with that same formula instead of weakening the generic `initial_cwnd >= MSS` invariant.

A public child that cannot bind a controller is rejected rather than silently falling back to native policy. PCB destroy callbacks release adapter-owned per-flow state.

## Controlled upstream provenance

Final provenance run `34815149550`, job `103884173736`, artifact `10335752874` proves:

- dependency `HEAD` equals the exact pin;
- pristine critical-source hashes are retained before patching;
- patch path/SHA256 match the repository patch;
- only `tcp.c`, `tcp_in.c`, and `tcp_out.c` are modified;
- no untracked dependency files exist;
- reverse-apply succeeds;
- an independent worktree created from the same pin, after applying the repository patch, is byte-identical for all three modified files.

## Integrated ACK ownership

P2 run `34815149444`, job `103884173371`, artifact `10335932665` passed the full bridge regression suite with the adapter enabled: IPv4, IPv6-to-IPv4-loopback backend, bidirectional backpressure, half-close, refusal/reset recovery, eight concurrent flows, active shutdown, and 64-flow reuse.

Every P2 workload now has a hard controller-ownership gate:

```text
cc_bindings == bridge_accepts
cc_bind_failures = 0
cc_ack_events > 0
cc_controller_errors = 0
```

This prevents a green data-path test from accidentally using native lwIP congestion policy.

## Integrated fast-loss and RTO evidence

P4 run `34815149645`, integrated-recovery job `103884174233`, artifact `10336213268` injects packet loss outside lwIP on the real TUN path.

Fast-loss mode drops one response data packet while later packets continue. Native duplicate-ACK/fast-retransmit mechanics recover the exact 262144-byte stream while the generic controller receives the loss policy event:

```text
cc_bindings=1
cc_bind_failures=0
cc_ack_events=108
cc_loss_events=1
cc_timeout_events=0
cc_controller_errors=0
mode=fast-loss payload_bytes=262144 recovery=ok
```

RTO mode suppresses response data past the initial retransmission timeout and then restores delivery:

```text
cc_bindings=1
cc_bind_failures=0
cc_ack_events=144
cc_loss_events=0
cc_timeout_events=2
cc_controller_errors=0
mode=rto payload_bytes=262144 recovery=ok
```

Neither test calls generic loss/RTO functions directly.

## Sender-side multiple-loss recovery follow-up — PR #35

The pinned sender's classic Reno recovery retransmits only the first unacknowledged segment after three duplicate ACKs and clears `TF_INFR` on the next ACK of new data. With multiple losses in one transmitted window, that ACK can be only a partial ACK; exiting recovery at that point can require another fast-retransmit episode or an RTO. The pinned `LWIP_TCP_SACK_OUT` option defaults to 0 and implements receiver-side SACK advertisement rather than a sender SACK scoreboard, so #35 does not attempt to solve this by enabling SACK output.

The follow-up keeps recovery transport-owned and adds the RFC 6582/NewReno mechanism needed for the non-SACK sender:

- first fast-retransmit entry records the current `snd_nxt` as the recovery-end boundary in the existing project hook sidecar;
- an ACK below that boundary is a partial ACK and does not clear `TF_INFR`;
- after acknowledged segments are freed, native `tcp_rexmit()` requeues the next first-unacknowledged segment immediately instead of waiting for another three duplicate ACKs;
- Reno/CUBIC retain transport-owned partial-window deflation, while internal BBR retains its already-qualified controller-owned recovery cwnd;
- every partial ACK still traverses the adapter's observation-only ACK path before upstream frees acknowledged segments, so delivery/rate/SRTT accounting remains current without advancing Reno/CUBIC controller cwnd/CA policy behind the native recovery window;
- the production transport-pacing selector and the internal-BBR wrapper both explicitly forward that observation-only hook; the live selector contract proves the observation updates delivery/SRTT while leaving controller ACK/policy counters and cwnd unchanged;
- partial ACKs keep the duplicate-ACK baseline at three so later duplicate ACKs continue the existing fast-recovery inflation rule;
- a full ACK exits recovery normally; an RTO supersedes and clears the recovery episode.

The deterministic qualification uses a real 40 ms / 10 Mbit/s path, a 1 MiB transfer, two one-shot data drops, and an eight-BDP queue. Representative successful results on the #35 branch are:

```text
reno   goodput=7.495346 Mbit/s  fault_drops=2  retransmit_events=2  loss_events=1  timeout_events=0
cubic  goodput=7.277130 Mbit/s  fault_drops=2  retransmit_events=2  loss_events=1  timeout_events=0
bbr    goodput=7.475838 Mbit/s  fault_drops=2  retransmit_events=2  loss_events=1  timeout_events=0
```

All three therefore repair the second hole inside one recovery episode without an RTO or a second congestion-loss signal. Single-loss and explicit-RTO gates remain separate and still pass.

A 260 ms / 10 Mbit/s / 1% random-loss diagnostic also improved materially without changing BBR gains or state-machine policy. On clean code checkpoint `36e820e60d79aab872124a1290b46aa959578ab7`, one realization measured internal BBR at 2.616504 Mbit/s with 33 qdisc data drops/retransmissions, 11 loss observations and zero RTOs; same-stack CUBIC measured 1.014745 Mbit/s with 18 drops/retransmissions, 16 loss observations and zero RTOs; the Linux BBR reference measured 5.085388 Mbit/s with 29 drops/retransmissions. Both tcp-shift flows retained exact delivery accounting. Because each random-loss run sees a different realization, these goodput ratios remain diagnostic rather than pass/fail parity thresholds.

The same clean code checkpoint passed P4 integrated recovery, the complete P6 Linux-reference workflow, and every other PR workflow except one P3 hosted-runner sample. That P3 sample retained an approximately 427–432 KiB drained floor and only 5 KiB first-to-last drain growth, but a low 299 KiB ready baseline made the relative warm-floor delta 133 KiB against the unchanged 128 KiB gate. The gate is not relaxed; closeout reruns P3 on the documentation checkpoint.

## Deterministic long-RTT burst qualification — PR #36

PR #36 changes qualification only and leaves the #35 NewReno transport semantics unchanged. The long-flow harness can inject 2–16 consecutive one-shot data drops by chaining independent iptables nth matchers at one packet index; each dropped packet short-circuits the chain, so the following matcher lands on the immediately following data packet. The harness verifies the exact injected drop count, zero unrelated qdisc drops, exact payload hash and delivery-ledger accounting, one loss episode, enough retransmissions to cover the burst, and zero RTO fallback.

On the 260 ms / 10 Mbit/s / 1 MiB WAN-like path, both a three-packet representative burst and a six-packet stress burst recover inside one NewReno episode:

```text
3-packet burst
reno   goodput=1.455117 Mbit/s  fault_drops=3  retransmit_events=3  loss_events=1  timeout_events=0
cubic  goodput=1.643490 Mbit/s  fault_drops=3  retransmit_events=3  loss_events=1  timeout_events=0
bbr    goodput=2.372795 Mbit/s  fault_drops=3  retransmit_events=3  loss_events=1  timeout_events=0

6-packet burst
reno   goodput=1.277751 Mbit/s  fault_drops=6  retransmit_events=6  loss_events=1  timeout_events=0
cubic  goodput=1.422573 Mbit/s  fault_drops=6  retransmit_events=6  loss_events=1  timeout_events=0
bbr    goodput=1.946821 Mbit/s  fault_drops=6  retransmit_events=6  loss_events=1  timeout_events=0
```

Goodput is diagnostic rather than a parity threshold. The correctness result is that transport-owned NewReno repairs consecutive bursts up to the current six-packet stress case without RTO amplification and without delivery-accounting loss across Reno, CUBIC, or internal BBR. This removes the immediate correctness justification for adding a sender SACK scoreboard. Frequent/high-loss behavior remains a separate performance qualification and may still justify a later transport mechanism if reproducible evidence requires it.

## Memory/CPU requalification

P3 rerun `34815149825`, job `103884174726`, artifact `10335968830` qualifies the integrated adapter against the P3 planning model.

```text
warm fixed process PSS: 335 KiB
conservative idle slope: 0.523438 KiB/flow
controlled active payload delta: 36.625 KiB/flow
fully-window-resident slope: 37.148438 KiB/flow
128-active projected process PSS: 5090 KiB
8-MiB process budget remaining: 3102 KiB = 24.234 KiB/flow
three-round 128-flow drain growth: 5 KiB
maximum warm drain floor above ready: 69 KiB
```

The original P3 admission baseline was 315 KiB warm fixed PSS and 37.523438 KiB/fully-window-resident flow. P4 therefore adds a small fixed cost but does not consume the active-flow planning headroom. This remains tcp-shift process PSS only; backend kernel/application and provider memory are excluded.

## Final behavior-head regression matrix

```text
upstream provenance  34815149550  success
P0                   34815149474  success
P1                   34815149646  success
P2                   34815149444  success
P3                   34815149825  success
P4                   34815149645  success
```

## Exit criteria

P4 is complete because CI now retains all of the following:

- pure-C standalone controller library with no lwIP/Linux dependency;
- conventional state-machine contract and explicit cwnd/ssthresh policy;
- narrow three-site lwIP policy hook instead of a TCP recovery fork;
- exact controlled-patch provenance;
- real ACK ownership across P2 workloads;
- real fast-loss and RTO controller events on the integrated packet path;
- unchanged P0/P1/P2 behavior and passing P3 memory/capacity gates;
- explicit documentation of recovery mechanics that remain native lwIP responsibilities.

## P5 next

P5 adds BBR prerequisites, not BBR itself:

- high-resolution monotonic send/ACK timestamps;
- cumulative delivered-byte accounting;
- minimal per-segment delivery metadata;
- ACK-derived delivery-rate sampling;
- app-limited detection;
- loss/inflight sample observations;
- one process-wide pacing scheduler, preferably a heap plus one timerfd;
- fixed/per-flow/per-segment memory and CPU qualification.

The sampler publishes transport-neutral observations through the generic CC boundary. Linux timerfd/epoll scheduling remains outside `src/cc/`.

## Stop signal

Stop and reassess before BBR if P5 requires rebuilding lwIP retransmission/SACK/recovery, materially enlarging the lwIP fork, consuming the constrained-host memory budget with metadata, or using per-flow timers/busy spinning to obtain pacing accuracy.
