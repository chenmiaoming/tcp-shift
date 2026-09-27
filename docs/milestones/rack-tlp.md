# RFC 8985 RACK-TLP

Status: active transport-recovery implementation; timer-driven RACK repair, tail-loss TLP, application-limited tail repair, lost-retransmission recovery, deterministic 28-drop recovery, and reordering/D-SACK adaptation are live-qualified, experimental/default-OFF.

tcp-shift is moving sender loss detection from a fixed DupAck/SACK-count heuristic toward RFC 8985 RACK-TLP. This work is deliberately transport-level and independent of Reno, CUBIC, and BBR controller policy.

## Normative reference

The normative specification is RFC 8985, not Linux implementation behavior. Linux TCP remains a differential reference after the RFC contracts are implemented.

RFC 8985 requires SACK plus per-segment most-recent transmit timestamps. tcp-shift already has both prerequisites in the experimental sender-SACK path and the delivery sidecar:

- a sender SACK scoreboard/tagging path;
- stable sent-segment identity;
- nanosecond monotonic send timestamps;
- retransmission identity;
- cumulative and selective delivery accounting.

The implementation keeps RACK/TLP metadata out of upstream `struct tcp_seg` where practical so the lwIP fork remains bounded.

## Implementation phases

The PR is intentionally dedicated to RACK-TLP. It must not contain BBR gain changes, CUBIC changes, IW10 policy changes, or unrelated memory tuning.

Implementation phases:

1. **RFC 8985 core contracts**
   - RACK.segment ordering by latest transmit timestamp plus end-sequence tie break;
   - recent RTT and reordering-window state;
   - time-based loss deadline;
   - DSACK reordering-window adaptation;
   - PTO calculation and one-probe invariant;
   - TLP repaired-loss ACK classification.

2. **Per-segment integration**
   - reuse existing sidecar `tx_ns` as Segment.xmit_ts;
   - reuse retransmission identity;
   - expose Segment.end_seq and RACK lost state;
   - update RACK.segment on cumulative and selective delivery.

3. **Loss detection and recovery selection**
   - make RACK time evidence the loss oracle instead of the current fixed three-later-SACK proof;
   - preserve the SACK scoreboard as required input;
   - do not run RFC 6675's recovery selection algorithm unchanged alongside RACK-TLP: RFC 8985 section 9.2 explicitly prohibits that combination because RFC 6675 does not handle lost retransmissions;
   - drive a modified retransmission selector from RACK-marked loss state;
   - use the RACK reordering timer when loss is not yet mature.

4. **TLP / PTO**
   - multiplex RACK/PTO/RTO timing without per-flow busy polling;
   - prefer new data when peer receive window permits;
   - otherwise retransmit the highest-sequence sent segment;
   - permit at most one extra probe beyond cwnd;
   - always fall back to the ordinary RTO after a probe.

5. **RTO and congestion-control integration**
   - on RTO, mark SND.UNA lost unconditionally and only mark other segments when the RACK deadline has elapsed;
   - feed the initial congestion event into Reno/CUBIC/BBR without coupling RACK to controller internals;
   - when RACK proves that a retransmission was itself lost, deliver the additional congestion response required by RFC 8985 section 9.3 without starting a second transport recovery episode;
   - reset TLP state on connection start, fast recovery, and RTO recovery.

6. **Qualification**
   - tail loss that previously requires RTO;
   - lost retransmission, including the required additional congestion response;
   - reordering below and above the reordering window;
   - DSACK adaptation;
   - deterministic 28-drop reference with exact retransmission accounting;
   - memory and timer-cost regression gates.

## Current boundary

The implementation is beyond the core-math-only stage. The experimental build now provides:

- transport-neutral RFC 8985 RACK/TLP state and deterministic contracts;
- per-segment latest-transmit timestamps and delivery ordering through the existing sidecar;
- cumulative/SACK delivery processing in the RFC-required timing pass followed by the reordering pass;
- RACK loss deadlines and a process-wide one-shot recovery timer integrated with the existing epoll owner;
- timer-driven RACK repair when loss matures below the ordinary DupThresh path;
- PTO/TLP scheduling with ordinary RTO as the conservative fallback;
- live tail-loss qualification proving one TLP retransmission repairs the tested tail loss without an RTO;
- live application-limited tail qualification where the final packet is first transmitted after the prior flight drains: the application-limited transition re-evaluates PTO, one TLP retransmission completes burst one before burst two is made available, the later ACK produces exactly one congestion indication, and the path records zero RTOs, zero unrelated qdisc drops, and exact payload integrity;
- an RFC-shaped lost-retransmission gate: one original segment and its first retransmission are dropped, a later lost original segment is successfully retransmitted to provide newer RACK timing evidence, and the first hole is then repaired again without RTO;
- the lost-retransmission gate requires exactly three injected drops, three retransmission events, two congestion-loss events, zero timeout fallback, zero unrelated qdisc drops, and exact payload integrity;
- the established 260 ms / 10 Mbit/s / 4 MiB deterministic first-send reference is live-qualified with RACK-TLP enabled: 28 explicit drops, exactly 28 retransmissions, zero timeout/RTO fallback, zero unrelated qdisc drops, and exact payload integrity; the observed run delivered 4.460655 Mbit/s with 11 controller loss/recovery episodes;
- the immediately preceding sender-SACK-only reference on the same 28-drop shape measured 5.444224 Mbit/s with 7 recovery episodes, so RACK-TLP correctness is established here but the enabled internal-BBR recovery interaction remains a controller-matrix/performance follow-up rather than a parity claim;
- congestion-control callbacks kept separate from the transport loss detector; RACK reports the second congestion indication without moving the existing fast-recovery boundary;
- deterministic reordering below the active reordering window is live-qualified with observed reordering, zero retransmissions, zero congestion-loss events, zero RTOs, zero qdisc drops, and exact payload delivery;
- RFC 2883 D-SACK classification uses the cumulative ACK carried in the same packet rather than stale sender state;
- deterministic reordering beyond the initial reordering window is live-qualified to trigger bounded spurious recovery, D-SACK feedback, RACK.reo_wnd_mult growth, and persistence without RTO or qdisc loss.

The feature remains **experimental/default-OFF** because the current live qualification is still narrow. Before considering production/default enablement, add fail-closed live gates for:

- recovery-timer cancellation/stale-release lifecycle under connection teardown;
- incremental memory and wakeup/timer cost;
- Reno, CUBIC, and internal BBR recovery behavior with RACK-TLP enabled, while preserving unchanged production behavior when it is disabled.

Accordingly, the correct current claim is **RFC 8985-driven experimental implementation with live timer-driven RACK repair, ordinary and application-limited tail-loss TLP, lost-retransmission recovery, exact 28-drop recovery accounting, and reordering/D-SACK adaptation qualified on deterministic paths**, not complete RFC 8985 or production conformance.
