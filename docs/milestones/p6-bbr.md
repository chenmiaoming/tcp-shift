# P6: tcp-shift BBR

Status: **active; PR #34 completed the compact BBRv1-style runtime/reference qualification and PR #35 now qualifies transport-owned NewReno-style sender recovery under multiple loss; public `bbr` selection remains disabled pending broader loss/burst/WAN qualification**.

## Congestion-control architecture

tcp-shift is not a single-algorithm stack. Congestion control remains selectable behind the transport-neutral `struct tcp_shift_cc_ops` boundary. The built-in controller names are intentionally distinct:

- `reno`: standard Reno; production default and live-selectable;
- `cubic`: RFC 9438 CUBIC; live-selectable after independent model/controller/adapter qualification;
- `bbr`: tcp-shift's compact BBRv1-style core with selected BBRv3-informed fixes; not registered yet;
- `bbrv3`: reserved for a future independent controller intended to track the then-current IETF BBRv3 semantics more closely; unavailable today.

`bbr` and `bbrv3` must remain separate implementations and names. A future `bbrv3` must not silently change the semantics of the compact `bbr` controller.

The adapter/runtime contains no algorithm-specific state-machine policy. Selection resolves an ops table plus bounded controller-owned state; all controllers consume the same transport-neutral ACK/loss/timeout observations and emit the same cwnd/pacing policy. lwIP continues to own sequence space, segment queues, retransmission, SACK/recovery, RTT/RTO machinery, and packet construction.

Production `tcp-shift-p2` accepts an optional controller name. Omission retains `reno`; `cubic` is explicitly selectable; unknown or unbuilt names fail before TUN setup. The production listener snapshots the chosen ops table and applies it to each newly accepted child before bridge code sees the flow. P5c's deterministic fixed-pacing qualification wrapper remains separate.

## Algorithm target for `bbr`

P6 does not target a full BBRv3 reimplementation. The `bbr` controller target is a **small BBRv1-style core with selected BBRv3-informed mechanisms**, adopted only when they improve correctness or robustness without materially increasing state/runtime complexity.

The core behavior baseline is Linux mainline `net/ipv4/tcp_bbr.c`, pinned for the P6 design comparison at Linux commit `587858367581b9c55c3690f4e63382ad622719d4` (2026-09-14). That implementation keeps the compact four-mode model:

- `STARTUP`;
- `DRAIN`;
- `PROBE_BW` with the classic 8-phase gain cycle;
- `PROBE_RTT`.

The current IETF `draft-ietf-ccwg-bbr-06` remains a secondary semantic reference for compact `bbr`, not its implementation-equivalence target. It is also the current reference point for a future, separate `bbrv3` controller. Existing P6 work may retain a newer rule when independently useful, such as conservative app-limited bandwidth-sample admission.

## Complexity policy

The default is to reject algorithm machinery that does not buy measurable value on the constrained-host target. Compact `bbr` does not automatically inherit BBRv3 `PROBE_BW_DOWN/CRUISE/REFILL/UP`, `inflight_hi/inflight_lo`, short-term bandwidth bounds, ACK aggregation, or spurious-loss machinery.

A v3-derived mechanism may be added to `bbr` only if a reproducible RTT/bandwidth/loss test demonstrates a concrete failure and the fix stays within retained memory/CPU/event-driven boundaries. Otherwise such machinery belongs, if needed, in the independent future `bbrv3` controller.

## Increment P6a: pure-C model estimation — merged

PR #14 was squash-merged into `main` as `58a9138786f2859efd9b0d61870ef60667bad9b6`.

It established a freestanding pure-C model with bandwidth/min-RTT estimation, app-limited sample admission, explicit monotonic time input, and bounded qualification counters. The retained behavior head `83640cf86687af1ec9b2fccceb13c470a360daf2` passed P0, P1, P2, P4, P5 rate sampler, P5c pacer, and the P6 model workflow. The initial model contract reported 112 bytes of state and the CC archive retained zero undefined external symbols.

P6a initially used the BBRv3 two-ProbeBW-cycle `max_bw` window. That filter was explicitly transitional after the compact-BBR target was chosen and is replaced by P6d before any live `bbr` policy is enabled.

## Increment P6b: packet-timed rounds and Startup full-bandwidth detection — merged

PR #15 was squash-merged into `main` as `a9a8928bc26f78815226cf1d078437089182d3b8`.

P6b added transport-neutral cumulative delivery snapshots to `struct tcp_shift_cc_rate_sample`, packet-timed round tracking, and Startup full-bandwidth detection. The deterministic contract proves same-round ACK suppression, exact 25% growth acceptance, app-limited plateau suppression, and a three-round full-pipe latch.

Those Startup constants are also present in the Linux BBRv1 reference: bandwidth growth of at least 1.25x resets the detector, while three rounds without that growth mark the pipe full. P6b therefore remains directly useful under the compact-BBR strategy.

## Increment P6c: live cumulative delivery snapshots — merged

PR #16 published the existing P5 delivery-sidecar snapshots into the generic CC rate observation without selecting BBR or changing live congestion policy.

The adapter publishes:

- `prior_delivered_bytes = candidate->delivered_at_send`;
- `delivered_total_bytes = adapter->delivered_bytes` after charging the current ACK.

The deterministic adapter contract uses a real lwIP PCB plus the existing segment-TX and ACK hooks to verify successive payloads export cumulative snapshots correctly. Later ACK-observation work extended the same live path with `CLOCK_MONOTONIC` ACK time and RFC 6298-style SRTT, while retaining Karn filtering for retransmitted RTT candidates.

## Shared controller prerequisites — merged

The controller work needed before `bbr` can become a peer is now qualified rather than merely planned:

- PR #17 introduced the freestanding name-to-ops registry;
- PR #18 replaced adapter-specific Reno storage with bounded built-in controller storage;
- PR #19 added the RFC 9438 CUBIC model;
- PR #20 added transport-neutral ACK time/SRTT observations;
- PR #21 registered CUBIC as a peer controller;
- PR #22 wired live `CLOCK_MONOTONIC` ACK/SRTT observations into the lwIP adapter;
- PR #23 enabled production `reno|cubic` runtime selection and qualified full TUN/bridge CUBIC fast-loss recovery.

PR #23 was squash-merged as `b0f44fbb7317cef93e90b1912896194f8c174947`. Its integrated CUBIC recovery gate transferred and echoed 262144 bytes, injected a data loss, required at least one controller loss event, required zero timeout fall-through, and retained zero controller errors. Reno remains the production default.

## Increment P6d: BBRv1-style max-bandwidth horizon — completed

The transitional two-ProbeBW-cycle max-bandwidth window is being replaced with an exact ten packet-timed-round ring, matching the compact core's Linux BBRv1-style horizon (`CYCLE_LEN + 2`).

The filter is intentionally aged only when a bandwidth sample is admissible. Lower app-limited samples are rejected before window aging so a long application-limited period cannot erase the last trustworthy network-rate estimate. When the next trustworthy sample arrives, the filter fast-forwards to the current packet round; a gap of ten or more rounds expires all prior slots before admitting the new sample.

The deterministic contract covers:

- a peak observed in round 1 remains visible through round 10;
- round 11 expires that round-1 peak;
- more than ten lower app-limited rounds retain the last trustworthy max bandwidth;
- the next non-app-limited sample after that long gap expires stale slots;
- an app-limited sample at or above the current model remains admissible.

This straightforward exact ring increases pure-model state relative to the transitional two-slot filter. `bbr` is still not live per-flow state, so P6d prioritizes auditable semantics; when the controller is eventually registered, P3 memory qualification will decide whether a more compact equivalent representation is worthwhile.

## Runtime integration checkpoint — merged PR #34

The compact controller is now bound to a real lwIP runtime through an internal-only qualification target; it is still not registered as a public `bbr` selector.

The runtime boundary keeps ownership split deliberately:

- the generic adapter retains delivery sampling and the process-wide event-driven pacer;
- BBR state is lazily allocated as a PCB sidecar rather than embedded in every generic adapter;
- BBR publishes a `3×cwnd` sender-buffer expansion hint, while allocation, `tcp_wmem.max`, pressure and accounting remain transport-owned;
- fast loss maps pinned-lwIP outstanding sequence space to post-loss inflight before packet-conservation entry;
- controller-owned BBR recovery suppresses only native lwIP recovery cwnd rewrites; Reno/CUBIC retain their existing native recovery path;
- recovery exit restores BBR's prior cwnd before the same ACK resumes normal delivery-sample processing;
- the RTO hook runs after `tcp_rexmit_rto_prepare()`, so the transport-equivalent post-loss inflight observation is zero at that boundary, and an RTO supersedes any active controller-owned fast-recovery episode.

The P6 runtime job now qualifies three real paths. A clean 4 MiB long flow over 40 ms / 10 Mbit/s with an eight-BDP lossless netem queue observed 9.159029 Mbit/s goodput, 2,281 controller policy updates, 2,280 valid delivery-rate samples, 4,658 pacer deferrals, 2,640 pacer resumes, zero qdisc drops, zero controller loss/RTO events, and exact payload integrity. Separate deterministic fault cases qualify fast loss (`loss_events=1`, no timeout) and RTO (`timeout_events=2`) while requiring retransmission and shared-pacer execution.

The earlier hosted-runner P3 repeated-drain failure at 141 KiB did not reproduce as a stable regression: a later run measured 121 KiB with the original 128 KiB gate unchanged. Absolute drained PSS stayed approximately 430–431 KiB while the ready baseline moved materially, so the threshold was not loosened.

Reference qualification is also complete on the PR branch. The clean Linux BBR + `sch_fq` matrix covers low-, edge-, and high-BDP paths; tcp-shift internal BBR remained within roughly 1–2% of the Linux reference goodput in those lossless cases, with zero tcp-shift qdisc drops/loss/RTO. A four-flow 40 ms / 10 Mbit/s shared-bottleneck case measured 8.261989 Mbit/s aggregate tcp-shift goodput with Jain fairness 0.985843 versus 7.993226 Mbit/s and 0.998559 for Linux BBR; the shared tcp-shift pacer reached `heap_peak=4`. A live two-burst application-limited case observed two app-limited entries, two exits, 31 app-limited rate samples, and zero qdisc drops/loss/RTO.

The moderate-loss diagnostic deliberately remains a reference rather than a parity gate. PR #34 established the original signal: on one 260 ms / 10 Mbit/s / 1% random-data-loss realization, internal BBR measured 1.166791 Mbit/s with 73 retransmission events and two RTOs, same-stack CUBIC measured 0.497755 Mbit/s, and Linux BBR measured 5.871238 Mbit/s. That result localized the next investigation below or around sender-side transport recovery rather than justifying BBR gain/state-machine tuning.

PR #35 now adds transport-owned RFC 6582/NewReno-style partial-ACK recovery without changing BBR gains or mode transitions. A deterministic 40 ms / 10 Mbit/s / 1 MiB case injects exactly two data losses inside one recovery flight. Reno, CUBIC, and internal BBR each recover with exactly two retransmission events, one congestion-loss episode, zero RTOs, zero qdisc drops, exact payload integrity, and exact delivery-ledger accounting. On clean code checkpoint `36e820e60d79aab872124a1290b46aa959578ab7`, representative goodputs were 7.495346 Mbit/s for Reno, 7.277130 Mbit/s for CUBIC, and 7.475838 Mbit/s for internal BBR.

Recovery ACK observation is separated from controller policy. Reno/CUBIC partial ACKs immediately update the generic delivery/rate/SRTT observation path before acknowledged segments are freed, but they do not advance the loss-based controller's cwnd/CA state behind the transport-owned NewReno window. Internal BBR continues to consume the full ACK-policy path because it explicitly owns recovery cwnd. Both the production transport-pacing selector wrapper and the internal-BBR wrapper forward the observation-only hook; a live selector contract prevents that callback from being dropped by a future wrapper.

The same follow-up materially improves the long-RTT random-loss diagnostic while preserving the no-parity-gate rule. On the clean code checkpoint, one 260 ms / 10 Mbit/s / 1% realization measured internal BBR at 2.616504 Mbit/s with 33 data qdisc drops/retransmissions, 11 loss observations, and zero RTOs; same-stack CUBIC measured 1.014745 Mbit/s with 18 drops/retransmissions, 16 loss observations, and zero RTOs; Linux BBR measured 5.085388 Mbit/s with 29 drops/retransmissions. The internal/Linux goodput ratio for that particular realization was 0.514514. Random-loss ratios remain diagnostic because each run sees a different loss pattern; the stronger correctness evidence is the deterministic two-loss gate, exact delivery accounting, and the disappearance of RTO fallback without BBR policy tuning.

That clean code checkpoint passed P4 integrated recovery, the complete P6 Linux-reference workflow, and every other PR workflow except one P3 hosted-runner sample. The P3 sample retained an approximately 427–432 KiB drained floor and only 5 KiB first-to-last drain growth, but a low 299 KiB ready baseline made the relative warm-floor delta 133 KiB against the unchanged 128 KiB gate. The gate remains unchanged and is rerun at closeout.

PR #34 was squash-merged as `7bbb175c4d0d69fa5858380b73710a9f8c41d204`, and PR #35 was squash-merged as `25e23c4de98d9fb862343cd6a62cf10bbd84ad33`. Public selector exposure remains intentionally deferred.

PR #36 adds qualification only. On a 260 ms / 10 Mbit/s / 1 MiB path, deterministic consecutive three-packet and six-packet bursts recover for Reno, CUBIC, and internal BBR with exact injected-drop/retransmission counts, one loss episode, zero RTOs, zero unrelated qdisc drops, exact payload integrity, and exact delivery-ledger accounting. Three-packet goodputs were 1.455117 / 1.643490 / 2.372795 Mbit/s for Reno/CUBIC/BBR; six-packet goodputs were 1.277751 / 1.422573 / 1.946821 Mbit/s. Goodput remains diagnostic. The correctness evidence says NewReno is sufficient for the currently tested short/medium consecutive bursts; it does not prove sender SACK has no value under frequent or high aggregate loss.

## Original planned order from P6d

Items 1–7 below are now substantially qualified by the compact-controller/runtime, Linux-reference, deterministic multiple-loss, and deterministic WAN-burst checkpoints. The remaining active loss qualification is frequent/high aggregate loss before deciding whether a more advanced sender recovery mechanism is warranted; public default changes remain out of scope.



1. finish and merge the 10-round bandwidth-filter qualification with `bbr` still unavailable in the registry;
2. add overflow-safe BDP/gain arithmetic and compact Startup pacing/cwnd policy in pure C;
3. implement deterministic `STARTUP -> DRAIN` and Drain exit at approximately one BDP;
4. implement classic 8-phase BBRv1-style `PROBE_BW` and ProbeRTT;
5. wrap the completed compact state machine as its own `tcp_shift_cc_ops` controller and add `bbr` to the registry only through qualification paths;
6. run live BBR through the existing event-driven process-wide pacer; do not add per-flow timers, polling, or recovery ownership;
7. compare Reno, CUBIC, tcp-shift `bbr`, and external/reference BBR behavior across RTT, bandwidth, random loss, recovery, multi-flow, app-limited, and high-BDP cases; clean/reference, multi-flow, app-limited, moderate random-loss, deterministic multiple-loss, and 260 ms three-/six-packet burst recovery are now present, with frequent/high aggregate loss remaining;
8. add selected v3-informed fixes to compact `bbr` only for demonstrated failures;
9. design `bbrv3` as a separate future ops/state implementation if full draft semantics are still desired;
10. rerun P3 memory/CPU plus all P0-P6 gates before changing any production default.

## Stop criteria

Stop and reassess if any controller requires moving retransmission/SACK/recovery ownership out of lwIP, introducing periodic controller polling or per-flow timer threads, materially widening the controlled lwIP patch, or consuming enough fixed/per-flow memory to threaten the constrained-host budget.
