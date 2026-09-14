# P6: tcp-shift BBR

Status: **active; model/state contract in progress**.

## Reference semantics

The primary algorithm reference for P6 is `draft-ietf-ccwg-bbr-06` (BBRv3, 2026-07-06). QUICHE BBR2/BBRv3-family code and Linux BBR/rate-sampling code are cross-checks, not substitutes for the project transport boundary.

P6 must not be described as Linux BBR-equivalent unless the relevant TCP semantics are demonstrated. The project controller continues to consume transport-neutral observations from P5 while lwIP retains sequence space, packet construction, retransmission, fast recovery, SACK/recovery, RTT/RTO calculation, and segment queues.

## Increment P6a: pure-C model state

The first increment deliberately stops before publishing BBR cwnd/pacing policy or binding a BBR controller to live lwIP PCBs.

New pure-C state in `src/cc/bbr.*` establishes:

- explicit BBR mode representation, initially `STARTUP`;
- `max_bw` estimation using a two-ProbeBW-cycle windowed maximum;
- application-limited bandwidth-sample admission: a lower app-limited rate cannot reduce the model, while an app-limited sample at or above the current maximum remains admissible;
- ProbeRTT minimum-delay candidate tracking with a 5-second interval;
- `min_rtt` refresh with a 10-second filter interval;
- explicit monotonic `now_ns` input rather than a clock syscall inside `src/cc/`;
- deterministic sample counters for qualification;
- no controller-owned heap allocation, POSIX dependency, lwIP object, fd, timer, or pacer dependency.

The two-cycle bandwidth window is advanced by an explicit model API in this increment. A later ProbeBW state machine will own the exact cycle-advance event after its round/cycle semantics are independently qualified.

## P6a deterministic contract

`scripts/p6-bbr-model-contract.sh` compiles the model freestanding with warnings-as-errors and verifies:

1. initialization begins in `STARTUP` with no bandwidth/min-RTT estimate;
2. an RTT-valid sample can update RTT state independently of a delivery-rate-valid sample;
3. a normal valid sample establishes `max_bw` and `min_rtt`;
4. a lower app-limited delivery-rate sample is ignored for `max_bw`;
5. a higher app-limited sample can raise `max_bw`;
6. the current and prior bandwidth-filter cycle are retained and an older maximum ages out after the window advances;
7. ProbeRTT candidate refresh uses the draft's strict `>` 5-second expiration rule;
8. the 10-second `min_rtt` interval allows the estimate to rise only after expiry;
9. a sample without `RTT_VALID` cannot alter RTT model state;
10. caller-time regression is rejected before mutating the model;
11. the initial model state remains bounded to at most 128 bytes on the qualification ABI.

The P6 workflow also reruns `scripts/validate-p4-cc.sh`, so adding the model cannot silently introduce non-ISO-C dependencies or undefined external symbols into the independently buildable CC archive.

## Explicitly out of scope for P6a

P6a does not yet implement:

- round-trip boundary tracking;
- Startup full-bandwidth detection;
- Drain transition;
- ProbeBW phase/state machine;
- ProbeRTT entry/exit;
- ACK aggregation / `extra_acked`;
- loss-derived `inflight_hi` / `inflight_lo` or short-term bandwidth bounds;
- BDP/cwnd target calculation;
- pacing gain or cwnd gain;
- BBR policy publication through `struct tcp_shift_cc_policy`;
- live lwIP controller selection;
- provider/OpenVZ qualification.

These are intentionally split into later independently qualified increments rather than being introduced as one opaque controller.

## Planned order after P6a

1. packet-timed round tracking and Startup bandwidth-growth/full-pipe detection;
2. safe integer BDP/gain arithmetic plus Startup pacing/cwnd policy publication;
3. deterministic Startup -> Drain transition and Drain exit;
4. ProbeBW cycle/phase state and max-bandwidth filter advancement at the correct event;
5. ProbeRTT scheduling/state integration;
6. loss/upper-bound model state and app-limited edge cases;
7. qualification-only live BBR binding to the existing P5 pacer;
8. reproducible RTT/bandwidth/loss scenarios compared with current reference behavior;
9. P0-P5 regression, P3 memory/CPU, pacer wakeup/lateness, and high-BDP reruns before any merge-ready claim.

## Stop criteria

Stop and reassess if BBR model correctness requires moving retransmission/SACK/recovery ownership out of lwIP, introducing periodic controller polling or per-flow timers, materially widening the controlled lwIP patch, or consuming enough fixed/per-flow memory to threaten the retained constrained-host budget.
