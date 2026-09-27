# RFC 8985 RACK-TLP

Status: active transport-recovery implementation, experimental/default-OFF.

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

## Scope of this PR

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
   - feed one congestion event into Reno/CUBIC/BBR without coupling RACK to controller internals;
   - reset TLP state on connection start, fast recovery, and RTO recovery.

6. **Qualification**
   - tail loss that previously requires RTO;
   - application-limited loss;
   - lost retransmission;
   - reordering below and above the reordering window;
   - DSACK adaptation;
   - deterministic 28-drop reference with exact retransmission accounting;
   - memory and timer-cost regression gates.

## Current boundary

The current implementation provides the transport-neutral RFC state/math, feeds cumulative/SACK delivery into RACK using separate RFC-ordered timing and reordering passes, and lets the experimental SACK retransmission selector query RACK time evidence. It does **not** enable RACK-TLP in default production builds, and the reordering timer/TLP PTO path is not complete yet.

The feature remains default-OFF until live lwIP integration proves:

- no retransmission amplification;
- no extra RTO regressions;
- bounded per-flow metadata/timer cost;
- Reno/CUBIC production behavior remains unchanged when the feature is OFF;
- BBR remains an independent consumer of transport loss events.
