# P6: tcp-shift BBR

Status: **active; P6a and P6b merged, P6c delivery snapshots in progress**.

## Congestion-control architecture

tcp-shift is not a single-algorithm stack. Congestion control remains selectable behind the existing transport-neutral `struct tcp_shift_cc_ops` boundary. The intended built-in controller names are:

- `reno`: standard Reno; current production/default controller while compatibility is retained;
- `cubic`: standard CUBIC implementation, to be added as an independently qualified controller;
- `bbr`: tcp-shift's small BBRv1-style core with selected BBRv3 fixes;
- `bbrv3`: a future independent controller intended to track the then-current IETF BBRv3 draft semantics more closely.

`bbr` and `bbrv3` must remain separate implementations and names. A future `bbrv3` must not silently change the semantics of the compact `bbr` controller.

The adapter/runtime must not contain algorithm-specific state-machine logic. Selection resolves an ops table plus controller-owned state; all controllers consume the same transport-neutral ACK/loss/timeout observations and emit the same cwnd/pacing policy. lwIP continues to own sequence space, segment queues, retransmission, SACK/recovery, RTT/RTO calculation, and packet construction.

The current adapter still binds Reno directly and embeds Reno state. A follow-up architecture increment will replace that hard-coded binding with a controller registry/selector and bounded per-flow algorithm state. Until CUBIC and selection are independently qualified, the runtime default remains `reno`; changing the default is a separate policy decision rather than a side effect of P6.

## Algorithm target for `bbr`

P6 no longer targets a full BBRv3 reimplementation. The `bbr` controller target is a **small BBRv1-style core with selected BBRv3 fixes**, chosen only when they improve correctness or robustness without materially increasing state/runtime complexity.

The core behavior baseline is Linux mainline `net/ipv4/tcp_bbr.c`, pinned for P6 design comparison at Linux commit `587858367581b9c55c3690f4e63382ad622719d4` (2026-09-14). That implementation keeps the compact four-mode model:

- `STARTUP`;
- `DRAIN`;
- `PROBE_BW` with the classic 8-phase gain cycle;
- `PROBE_RTT`.

The current IETF `draft-ietf-ccwg-bbr-06` remains a secondary semantic reference for `bbr`, not its implementation target. It is also the current reference point for the future, separate `bbrv3` controller. Existing P6 work may retain a v3 rule when independently useful, such as conservative app-limited handling.

## Complexity policy

The default is to reject algorithm machinery that does not buy measurable value on the constrained-host target. The compact `bbr` controller does not automatically inherit BBRv3 `PROBE_BW_DOWN/CRUISE/REFILL/UP`, `inflight_hi/inflight_lo`, short-term bandwidth bounds, ACK aggregation, or spurious-loss machinery.

A v3-derived mechanism may be added to `bbr` only if a reproducible RTT/bandwidth/loss test demonstrates a concrete failure and the fix stays within retained memory/CPU/event-driven boundaries. Otherwise such machinery belongs, if needed, in the independent future `bbrv3` controller.

## Increment P6a: pure-C model estimation — merged

PR #14 was squash-merged into `main` as `58a9138786f2859efd9b0d61870ef60667bad9b6`.

It established a freestanding pure-C model with bandwidth/min-RTT estimation, app-limited sample admission, explicit monotonic time input, and bounded qualification counters. The retained behavior head `83640cf86687af1ec9b2fccceb13c470a360daf2` passed P0, P1, P2, P4, P5 rate sampler, P5c pacer, and the P6 model workflow. The model contract reported 112 bytes of state and the CC archive retained zero undefined external symbols.

P6a initially used the BBRv3 two-ProbeBW-cycle `max_bw` window. Under the revised compact-BBR target this is transitional: before live `bbr` policy is enabled, the bandwidth filter will be realigned to the Linux BBRv1-style 10 packet-timed-round window.

## Increment P6b: packet-timed rounds and Startup full-bandwidth detection — merged

PR #15 was squash-merged into `main` as `a9a8928bc26f78815226cf1d078437089182d3b8`.

P6b added transport-neutral cumulative delivery snapshots to `struct tcp_shift_cc_rate_sample`, packet-timed round tracking, and Startup full-bandwidth detection. The deterministic contract proves same-round ACK suppression, exact 25% growth acceptance, app-limited plateau suppression, and a three-round full-pipe latch.

Those Startup constants are also present in the Linux BBRv1 reference: bandwidth growth of at least 1.25x resets the detector, while three rounds without that growth mark the pipe full. P6b therefore remains directly useful under the compact-BBR strategy.

## Increment P6c: live cumulative delivery snapshots

P6c does not select BBR and does not publish BBR pacing/cwnd policy. It only closes the observation gap between the already-qualified P5 delivery sidecar and packet-timed controllers.

The adapter publishes:

- `prior_delivered_bytes = candidate->delivered_at_send`;
- `delivered_total_bytes = adapter->delivered_bytes` after charging the current ACK.

Qualification telemetry counts published snapshots and rejects a non-increasing `(prior,total)` pair. A deterministic adapter contract uses a real lwIP PCB plus existing segment-TX and ACK hooks to verify two successive payloads export `(0, MSS)` and `(MSS, 2*MSS)`. Existing P5/P5c real-TUN workflows remain mandatory regression gates.

## Planned order

1. finish P6c delivered-snapshot qualification with current Reno behavior unchanged;
2. add a bounded controller registry/selector so `reno`, future `cubic`, `bbr`, and future `bbrv3` share one adapter boundary without runtime-specific branches;
3. implement and independently qualify `cubic` while retaining Reno as the compatibility default during rollout;
4. realign compact `bbr` bandwidth filtering to a 10 packet-timed-round BBRv1-style max filter;
5. implement overflow-safe BDP/gain arithmetic and compact BBR Startup pacing/cwnd policy in pure C;
6. implement deterministic `STARTUP -> DRAIN -> PROBE_BW`, the classic 8-phase ProbeBW gain cycle, and ProbeRTT;
7. bind compact `bbr` only through the selector/qualification path, never by replacing Reno internals;
8. compare Reno, CUBIC, tcp-shift `bbr`, and external/reference BBR behavior across RTT, bandwidth, random loss, recovery, multi-flow, app-limited, and high-BDP cases;
9. add selected v3 fixes to compact `bbr` only for demonstrated failures;
10. design `bbrv3` as a separate future ops/state implementation if full draft semantics are still desired;
11. rerun P3 memory/CPU plus all P0-P6 gates before changing any production default.

## Stop criteria

Stop and reassess if any controller requires moving retransmission/SACK/recovery ownership out of lwIP, introducing periodic controller polling or per-flow timer threads, materially widening the controlled lwIP patch, or consuming enough fixed/per-flow memory to threaten the constrained-host budget.
