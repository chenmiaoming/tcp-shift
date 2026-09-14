# P6: tcp-shift BBR

Status: **active; P6a model estimation merged, P6b packet-timed rounds / Startup full-bandwidth detection in progress**.

## Reference semantics

The primary algorithm reference for P6 is `draft-ietf-ccwg-bbr-06` (BBRv3, 2026-07-06). QUICHE BBR2/BBRv3-family code and Linux BBR/rate-sampling code are cross-checks, not substitutes for the project transport boundary.

P6 must not be described as Linux BBR-equivalent unless the relevant TCP semantics are demonstrated. The project controller continues to consume transport-neutral observations from P5 while lwIP retains sequence space, packet construction, retransmission, fast recovery, SACK/recovery, RTT/RTO calculation, and segment queues.

## Increment P6a: pure-C model estimation — merged

PR #14 established the first BBR model checkpoint and was squash-merged into `main` as `58a9138786f2859efd9b0d61870ef60667bad9b6`.

Pure-C state in `src/cc/bbr.*` establishes:

- explicit BBR mode representation, initially `STARTUP`;
- `max_bw` estimation using a two-ProbeBW-cycle windowed maximum;
- application-limited bandwidth-sample admission: a lower app-limited rate cannot reduce the model, while an app-limited sample at or above the current maximum remains admissible;
- ProbeRTT minimum-delay candidate tracking with a 5-second interval;
- `min_rtt` refresh with a 10-second filter interval;
- explicit monotonic `now_ns` input rather than a clock syscall inside `src/cc/`;
- deterministic sample counters for qualification;
- no controller-owned heap allocation, POSIX dependency, lwIP object, fd, timer, or pacer dependency.

The P6a behavior head `83640cf86687af1ec9b2fccceb13c470a360daf2` passed P0, P1, P2, P4, P5 rate sampler, P5c pacer, and the dedicated P6 BBR model workflow. The retained model contract reported 112 bytes of BBR model state and the pure-C archive retained zero undefined external symbols.

The two-cycle bandwidth window remains advanced by an explicit model API. A later ProbeBW state machine will own the exact cycle-advance event after its round/cycle semantics are independently qualified.

## Increment P6b: packet-timed rounds and Startup full-bandwidth detection

P6b adds only the next model semantics required before publishing live BBR policy.

### Transport-neutral round snapshots

`struct tcp_shift_cc_rate_sample` now reserves two cumulative delivery snapshots:

- `prior_delivered_bytes`: cumulative delivered bytes captured when the sample's reference packet/segment was sent;
- `delivered_total_bytes`: cumulative delivered bytes after the current ACK.

These fields are transport semantics, not lwIP objects. They are intentionally added to the generic observation surface before live adapter population is enabled. Existing controllers ignore them.

### Packet-timed round tracking

The BBR model tracks:

- `next_round_delivered`;
- `round_count`;
- transient `round_start`.

When a sample publishes nonzero cumulative delivery snapshots and `prior_delivered_bytes >= next_round_delivered`, the ACK starts a new packet-timed round, advances `next_round_delivered` to the current cumulative delivered total, and increments `round_count`. ACKs for packets sent within that marker do not start another round.

### Startup full-bandwidth detector

P6b adds the draft's Startup bandwidth-growth plateau rule without changing mode or publishing pacing/cwnd policy yet:

- only a valid, non-app-limited sample at a packet-timed round start contributes;
- the baseline resets when bandwidth grows by at least 25%;
- the integer threshold is computed as `ceil(5 * full_bw / 4)` without overflowing `uint64_t`;
- otherwise `full_bw_count` increments;
- after three qualifying rounds without 25% growth, `full_bw_reached` latches true;
- app-limited rounds still advance the packet-timed round counter but do not contribute plateau evidence;
- `full_bw_now` is a transient detection event while `full_bw_reached` is durable state.

The P6b deterministic contract explicitly proves same-round ACK suppression, exact 25% growth acceptance, app-limited plateau suppression, and the three-round full-pipe latch.

## Still out of scope after P6b

P6b still does not implement:

- automatic population of the new cumulative delivery snapshots by the lwIP adapter;
- Drain transition;
- ProbeBW phase/state machine;
- ProbeRTT entry/exit;
- ACK aggregation / `extra_acked`;
- loss-derived `inflight_hi` / `inflight_lo` or short-term bandwidth bounds;
- BDP/cwnd target calculation;
- pacing gain or cwnd gain;
- BBR policy publication through `struct tcp_shift_cc_policy`;
- live lwIP BBR controller selection;
- provider/OpenVZ qualification.

These remain independently qualified increments rather than one opaque controller change.

## Planned order after P6b

1. populate prior/current delivered snapshots from the already-existing P5 delivery sidecar and qualify their live ACK semantics without selecting BBR;
2. safe integer BDP/gain arithmetic plus Startup pacing/cwnd policy publication in a pure-C controller contract;
3. deterministic Startup -> Drain transition and Drain exit;
4. ProbeBW cycle/phase state and max-bandwidth filter advancement at the correct event;
5. ProbeRTT scheduling/state integration;
6. loss/upper-bound model state and app-limited edge cases;
7. qualification-only live BBR binding to the existing P5 pacer;
8. reproducible RTT/bandwidth/loss scenarios compared with current reference behavior;
9. P0-P5 regression, P3 memory/CPU, pacer wakeup/lateness, and high-BDP reruns before any merge-ready live-controller claim.

## Stop criteria

Stop and reassess if BBR model correctness requires moving retransmission/SACK/recovery ownership out of lwIP, introducing periodic controller polling or per-flow timers, materially widening the controlled lwIP patch, or consuming enough fixed/per-flow memory to threaten the retained constrained-host budget.
