# P6: tcp-shift BBR

Status: **active; P6a and P6b merged, P6c delivery snapshots in progress**.

## Algorithm target

P6 no longer targets a full BBRv3 reimplementation. The project target is a **small BBRv1-style core with selected BBRv3 fixes**, chosen only when they improve correctness or robustness without materially increasing state/runtime complexity.

The core behavior baseline is Linux mainline `net/ipv4/tcp_bbr.c`, pinned for P6 design comparison at Linux commit `587858367581b9c55c3690f4e63382ad622719d4` (2026-09-14). That implementation keeps the compact four-mode model:

- `STARTUP`;
- `DRAIN`;
- `PROBE_BW` with the classic 8-phase gain cycle;
- `PROBE_RTT`.

The current IETF `draft-ietf-ccwg-bbr-06` remains a secondary semantic reference, not the implementation target. Existing P6 work may retain a v3 rule when it is independently useful; for example, transport-neutral delivery observations and conservative app-limited handling. Advanced v3 ProbeBW subphases and the full v3 short/long-term upper-bound machinery are not assumed requirements.

P6 must not be described as Linux BBR-equivalent or BBRv3-equivalent unless the relevant transport semantics are demonstrated. lwIP continues to own sequence space, packet construction, retransmission, fast recovery, SACK/recovery, RTT/RTO calculation, and segment queues. tcp-shift consumes transport-neutral observations and publishes cwnd/pacing policy only.

## Complexity policy

The default is to reject algorithm machinery that does not buy measurable value on the constrained-host target. In particular, tcp-shift does not automatically inherit BBRv3 `PROBE_BW_DOWN/CRUISE/REFILL/UP`, `inflight_hi/inflight_lo`, short-term bandwidth bounds, ACK aggregation, or spurious-loss machinery.

A v3-derived mechanism may be added later only if a reproducible RTT/bandwidth/loss test shows a concrete regression in the minimal core and the fix stays within the retained memory/CPU/event-driven boundaries.

## Increment P6a: pure-C model estimation — merged

PR #14 was squash-merged into `main` as `58a9138786f2859efd9b0d61870ef60667bad9b6`.

It established a freestanding pure-C model with bandwidth/min-RTT estimation, app-limited sample admission, explicit monotonic time input, and bounded qualification counters. The retained behavior head `83640cf86687af1ec9b2fccceb13c470a360daf2` passed P0, P1, P2, P4, P5 rate sampler, P5c pacer, and the P6 model workflow. The model contract reported 112 bytes of state and the CC archive retained zero undefined external symbols.

P6a initially used the BBRv3 two-ProbeBW-cycle `max_bw` window. Under the revised minimal-core target this is now explicitly transitional: before live BBR policy is enabled, the bandwidth filter will be realigned to the Linux BBRv1-style 10 packet-timed-round window so the core state machine and estimator use one coherent reference model.

## Increment P6b: packet-timed rounds and Startup full-bandwidth detection — merged

PR #15 was squash-merged into `main` as `a9a8928bc26f78815226cf1d078437089182d3b8`.

P6b added transport-neutral cumulative delivery snapshots to `struct tcp_shift_cc_rate_sample`, packet-timed round tracking, and Startup full-bandwidth detection. The deterministic contract proves same-round ACK suppression, exact 25% growth acceptance, app-limited plateau suppression, and a three-round full-pipe latch.

Those Startup constants are also present in the Linux BBRv1 reference: bandwidth growth of at least 1.25x resets the detector, while three rounds without that growth mark the pipe full. Therefore P6b remains directly useful under the minimal-core strategy.

## Increment P6c: live cumulative delivery snapshots

P6c does not select BBR and does not publish BBR pacing/cwnd policy. It only closes the observation gap between the already-qualified P5 delivery sidecar and the P6 round model.

The adapter publishes:

- `prior_delivered_bytes = candidate->delivered_at_send`;
- `delivered_total_bytes = adapter->delivered_bytes` after charging the current ACK.

Qualification telemetry counts published snapshots and rejects a non-increasing `(prior,total)` pair. A deterministic adapter contract uses a real lwIP PCB plus the existing segment-TX and ACK hooks to verify two successive payloads export `(0, MSS)` and `(MSS, 2*MSS)`. Existing P5/P5c real-TUN workflows remain mandatory regression gates.

## Planned minimal-core order

1. finish P6c delivered-snapshot qualification and keep Reno/live pacing behavior unchanged;
2. replace the transitional two-cycle bandwidth max with a 10 packet-timed-round BBRv1-style max filter;
3. implement overflow-safe BDP/gain arithmetic and the BBRv1 Startup pacing/cwnd policy in pure C;
4. implement deterministic `STARTUP -> DRAIN -> PROBE_BW` transitions;
5. implement the classic 8-phase ProbeBW pacing cycle (`1.25, 0.75, 1, 1, 1, 1, 1, 1`) with event/round-driven advancement, not polling;
6. implement ProbeRTT with the existing event-driven runtime and no per-flow timer thread;
7. bind the minimal BBR controller only in a qualification target, keeping ordinary Reno available as the production/reference controller;
8. compare tcp-shift minimal BBR against Reno and reference BBR behavior across RTT, bandwidth, random loss, fast retransmit, RTO, multi-flow, app-limited, and high-BDP scenarios;
9. add selected BBRv3 fixes only for demonstrated failures of the minimal core;
10. rerun P3 memory/CPU plus all P0-P5/P6 gates before any merge-ready live-controller claim.

## Explicitly deferred v3 machinery

Unless later evidence justifies it, P6 does not plan to implement the full BBRv3 ProbeBW sub-state machine, v3 short/long-term bandwidth and inflight upper bounds, or other large state additions merely for version parity.

The project optimizes for throughput, bounded queues, low memory/CPU overhead, and mechanically testable behavior on constrained userspace TCP—not for claiming the newest BBR version number.

## Stop criteria

Stop and reassess if model correctness requires moving retransmission/SACK/recovery ownership out of lwIP, introducing periodic controller polling or per-flow timers, materially widening the controlled lwIP patch, or consuming enough fixed/per-flow memory to threaten the retained constrained-host budget.
